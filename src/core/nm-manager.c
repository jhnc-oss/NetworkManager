/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2007 - 2009 Novell, Inc.
 * Copyright (C) 2007 - 2017 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-manager.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "NetworkManagerUtils.h"
#include "devices/nm-device-factory.h"
#include "devices/nm-device-generic.h"
#include "devices/nm-device-loopback.h"
#include "devices/nm-device.h"
#include "dns/nm-dns-manager.h"
#include "dhcp/nm-dhcp-manager.h"
#include "libnm-core-aux-intern/nm-common-macros.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-glib-aux/nm-c-list.h"
#include "libnm-platform/nm-platform.h"
#include "libnm-platform/nmp-object.h"
#include "libnm-std-aux/nm-dbus-compat.h"
#include "nm-act-request.h"
#include "nm-audit-manager.h"
#include "nm-auth-manager.h"
#include "nm-auth-utils.h"
#include "nm-checkpoint-manager.h"
#include "nm-checkpoint.h"
#include "nm-config.h"
#include "nm-connectivity.h"
#include "nm-dbus-manager.h"
#include "nm-dbus-object.h"
#include "nm-dispatcher.h"
#include "nm-hostname-manager.h"
#include "nm-keep-alive.h"
#include "nm-policy.h"
#include "nm-priv-helper-call.h"
#include "nm-rfkill-manager.h"
#include "nm-session-monitor.h"
#include "nm-power-monitor.h"
#include "settings/nm-settings-connection.h"
#include "settings/nm-settings.h"
#include "vpn/nm-vpn-manager.h"

#define DEVICE_STATE_PRUNE_RATELIMIT_MAX 100u

/*****************************************************************************/

typedef struct {
    guint                        prop_id;
    guint                        hw_prop_id;
    NMConfigRunStatePropertyType key;
} RfkillTypeDesc;

typedef struct {
    bool available : 1;
    bool user_enabled : 1;
    bool sw_enabled : 1;
    bool hw_enabled : 1;
} RfkillRadioState;

#define AUTOCONNECT_RESET_RETRIES_TIMER_SEC 300

typedef struct {
    NMDevice             *device;
    NMSettingsConnection *sett_conn;
    CList                 dev_lst;
    CList                 con_lst;

    /* Autoconnet retries needs to be tracked for each (device, connection)
     * tuple because when a connection is a multiconnect one, each valid device
     * must try to autoconnect the retries defined in the connection. */
    struct {
        guint32                            retries;
        gint32                             blocked_until_sec;
        NMSettingsAutoconnectBlockedReason blocked_reason;
        bool                               initialized : 1;
    } autoconnect;

} DevConData;

#define DEV_CON_DATA_LOG_FMT \
    "device[" NM_HASH_OBFUSCATE_PTR_FMT ",%s]-profile[" NM_HASH_OBFUSCATE_PTR_FMT ",%s]"

/* This is an unsafe macro (it evaluates the macro arguments multiple times and is non-function-like). */
#define DEV_CON_DATA_LOG_ARGS(device, sett_conn)                                                  \
    NM_HASH_OBFUSCATE_PTR(device), nm_device_get_iface(device), NM_HASH_OBFUSCATE_PTR(sett_conn), \
        nm_settings_connection_get_id(sett_conn)

#define DEV_CON_DATA_LOG_ARGS_DATA(data) DEV_CON_DATA_LOG_ARGS((data)->device, (data)->sett_conn)

typedef enum {
    ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_INTERNAL,
    ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_USER,
    ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE,
    ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE2,
} AsyncOpType;

typedef struct {
    CList       async_op_lst;
    NMManager  *self;
    AsyncOpType async_op_type;
    union {
        struct {
            NMActiveConnection *active;
            union {
                struct {
                    GDBusMethodInvocation *invocation;
                } activate_user;
                struct {
                    GDBusMethodInvocation          *invocation;
                    NMConnection                   *connection;
                    NMSettingsConnectionPersistMode persist_mode;
                    bool                            is_volatile : 1;
                } add_and_activate;
            };
        } ac_auth;
    };
} AsyncOpData;

enum {
    DEVICE_ADDED,
    INTERNAL_DEVICE_ADDED,
    DEVICE_REMOVED,
    INTERNAL_DEVICE_REMOVED,
    ACTIVE_CONNECTION_ADDED,
    ACTIVE_CONNECTION_REMOVED,
    CONFIGURE_QUIT,
    DEVICE_IFINDEX_CHANGED,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

NM_GOBJECT_PROPERTIES_DEFINE(NMManager,
                             PROP_VERSION,
                             PROP_VERSION_INFO,
                             PROP_CAPABILITIES,
                             PROP_STATE,
                             PROP_STARTUP,
                             PROP_NETWORKING_ENABLED,
                             PROP_WIRELESS_ENABLED,
                             PROP_WIRELESS_HARDWARE_ENABLED,
                             PROP_WWAN_ENABLED,
                             PROP_WWAN_HARDWARE_ENABLED,
                             PROP_WIMAX_ENABLED,
                             PROP_WIMAX_HARDWARE_ENABLED,
                             PROP_RADIO_FLAGS,
                             PROP_ACTIVE_CONNECTIONS,
                             PROP_CONNECTIVITY,
                             PROP_CONNECTIVITY_CHECK_AVAILABLE,
                             PROP_CONNECTIVITY_CHECK_ENABLED,
                             PROP_CONNECTIVITY_CHECK_URI,
                             PROP_PRIMARY_CONNECTION,
                             PROP_PRIMARY_CONNECTION_TYPE,
                             PROP_ACTIVATING_CONNECTION,
                             PROP_DEVICES,
                             PROP_METERED,
                             PROP_GLOBAL_DNS_CONFIGURATION,
                             PROP_ALL_DEVICES,
                             PROP_CHECKPOINTS,

                             /* Not exported */
                             PROP_SLEEPING, );

typedef struct {
    NMPlatform *platform;

    NMDnsManager *dns_mgr;
    gulong        dns_mgr_update_pending_signal_id;

    GArray *capabilities;

    CList               active_connections_lst_head; /* Oldest ACs at the beginning */
    CList               async_op_lst_head;
    guint               ac_cleanup_id;
    NMActiveConnection *primary_connection;
    NMActiveConnection *activating_connection;
    NMMetered           metered;

    CList devices_lst_head;

    NMState            state;
    NMConfig          *config;
    NMConnectivity    *concheck_mgr;
    NMPolicy          *policy;
    NMHostnameManager *hostname_manager;

    struct {
        GDBusConnection *connection;
        guint            id;
    } prop_filter;
    NMRfkillManager *rfkill_mgr;

    GHashTable *devcon_data_dict;

    CList link_cb_lst;

    NMCheckpointManager *checkpoint_mgr;

    NMSettings *settings;

    RfkillRadioState radio_states[NM_RFKILL_TYPE_MAX];

    NMVpnManager *vpn_manager;

    NMPowerMonitor *power_monitor;

    NMAuthManager *auth_mgr;

    GHashTable *device_route_metrics;

    CList auth_lst_head;

    GHashTable *sleep_devices;

    /* Firmware dir monitor */
    GFileMonitor *fw_monitor;
    guint         fw_changed_id;

    guint timestamp_update_id;

    guint devices_inited_id;

    guint radio_flags;

    NMConnectivityState connectivity_state;

    guint8 device_state_prune_ratelimit_count;

    bool startup : 1;
    bool devices_inited : 1;

    bool sleeping : 1;
    bool net_enabled : 1;

    unsigned connectivity_check_enabled_last : 2;

    /* List of GDBusMethodInvocation of in progress Sleep() and Enable()
     * calls. They return only if all in-flight deactivations finished. */
    GSList *sleep_invocations;

    guint delete_volatile_connection_idle_id;
    CList delete_volatile_connection_lst_head;
} NMManagerPrivate;

struct _NMManager {
    NMDBusObject     parent;
    NMManagerPrivate _priv;
};

typedef struct {
    NMDBusObjectClass parent;

#if WITH_OPENVSWITCH
    /* these fields only serve the purpose to use the symbols.*/
    void (*_use_symbol_nm_priv_helper_call_get_fd)(void);
    void (*_use_symbol_nm_priv_helper_utils_open_fd)(void);
#endif

} NMManagerClass;

G_DEFINE_TYPE(NMManager, nm_manager, NM_TYPE_DBUS_OBJECT)

#define NM_MANAGER_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMManager, NM_IS_MANAGER)

/*****************************************************************************/

NM_DEFINE_SINGLETON_INSTANCE(NMManager);

/*****************************************************************************/

#define _NMLOG_PREFIX_NAME "manager"
#define _NMLOG(level, domain, ...)                                                                 \
    G_STMT_START                                                                                   \
    {                                                                                              \
        const NMLogLevel  _level  = (level);                                                       \
        const NMLogDomain _domain = (domain);                                                      \
                                                                                                   \
        if (nm_logging_enabled(_level, _domain)) {                                                 \
            const NMManager *const _self = (self);                                                 \
            char                   _sbuf[32];                                                      \
                                                                                                   \
            _nm_log(_level,                                                                        \
                    _domain,                                                                       \
                    0,                                                                             \
                    NULL,                                                                          \
                    NULL,                                                                          \
                    "%s%s: " _NM_UTILS_MACRO_FIRST(__VA_ARGS__),                                   \
                    _NMLOG_PREFIX_NAME,                                                            \
                    ((_self && _self != singleton_instance) ? nm_sprintf_buf(_sbuf, "[%p]", _self) \
                                                            : "")                                  \
                        _NM_UTILS_MACRO_REST(__VA_ARGS__));                                        \
        }                                                                                          \
    }                                                                                              \
    G_STMT_END

#define _NMLOG2(level, domain, device, ...)                                                        \
    G_STMT_START                                                                                   \
    {                                                                                              \
        const NMLogLevel  _level  = (level);                                                       \
        const NMLogDomain _domain = (domain);                                                      \
                                                                                                   \
        if (nm_logging_enabled(_level, _domain)) {                                                 \
            const NMManager *const _self   = (self);                                               \
            const char *const      _ifname = _nm_device_get_iface(device);                         \
            char                   _sbuf[32];                                                      \
                                                                                                   \
            _nm_log(_level,                                                                        \
                    _domain,                                                                       \
                    0,                                                                             \
                    _ifname,                                                                       \
                    NULL,                                                                          \
                    "%s%s: %s%s%s" _NM_UTILS_MACRO_FIRST(__VA_ARGS__),                             \
                    _NMLOG_PREFIX_NAME,                                                            \
                    ((_self && _self != singleton_instance) ? nm_sprintf_buf(_sbuf, "[%p]", _self) \
                                                            : ""),                                 \
                    NM_PRINT_FMT_QUOTED(_ifname, "(", _ifname, "): ", "")                          \
                        _NM_UTILS_MACRO_REST(__VA_ARGS__));                                        \
        }                                                                                          \
    }                                                                                              \
    G_STMT_END

#define _NMLOG3(level, domain, connection, ...)                                                    \
    G_STMT_START                                                                                   \
    {                                                                                              \
        const NMLogLevel  _level  = (level);                                                       \
        const NMLogDomain _domain = (domain);                                                      \
                                                                                                   \
        if (nm_logging_enabled(_level, _domain)) {                                                 \
            const NMManager *const _self       = (self);                                           \
            NMConnection *const    _connection = (connection);                                     \
            const char *const      _con_id     = _nm_connection_get_id(_connection);               \
            char                   _sbuf[32];                                                      \
                                                                                                   \
            _nm_log(_level,                                                                        \
                    _domain,                                                                       \
                    0,                                                                             \
                    NULL,                                                                          \
                    _nm_connection_get_uuid(_connection),                                          \
                    "%s%s: %s%s%s" _NM_UTILS_MACRO_FIRST(__VA_ARGS__),                             \
                    _NMLOG_PREFIX_NAME,                                                            \
                    ((_self && _self != singleton_instance) ? nm_sprintf_buf(_sbuf, "[%p]", _self) \
                                                            : ""),                                 \
                    NM_PRINT_FMT_QUOTED(_con_id, "(", _con_id, ") ", "")                           \
                        _NM_UTILS_MACRO_REST(__VA_ARGS__));                                        \
        }                                                                                          \
    }                                                                                              \
    G_STMT_END

/*****************************************************************************/

static const NMDBusInterfaceInfoExtended interface_info_manager;
static const GDBusSignalInfo             signal_info_check_permissions;
static const GDBusSignalInfo             signal_info_state_changed;
static const GDBusSignalInfo             signal_info_device_added;
static const GDBusSignalInfo             signal_info_device_removed;

static void update_connectivity_value(NMManager *self);

static gboolean add_device(NMManager *self, NMDevice *device, GError **error);

static void _emit_device_added_removed(NMManager *self, NMDevice *device, gboolean is_added);

static NMActiveConnection *_new_active_connection(NMManager             *self,
                                                  gboolean               is_vpn,
                                                  NMSettingsConnection  *sett_conn,
                                                  NMConnection          *incompl_conn,
                                                  NMConnection          *applied,
                                                  const char            *specific_object,
                                                  NMDevice              *device,
                                                  NMAuthSubject         *subject,
                                                  NMActivationType       activation_type,
                                                  NMActivationReason     activation_reason,
                                                  NMActivationStateFlags initial_state_flags,
                                                  GError               **error);

static void policy_activating_ac_changed(GObject *object, GParamSpec *pspec, gpointer user_data);

static void device_has_pending_action_changed(NMDevice *device, GParamSpec *pspec, NMManager *self);
static void check_if_startup_complete(NMManager *self);

static gboolean find_controller(NMManager             *self,
                                NMConnection          *connection,
                                NMDevice              *device,
                                NMSettingsConnection **out_controller_connection,
                                NMDevice             **out_controller_device,
                                NMActiveConnection   **out_controller_ac,
                                GError               **error);

static void nm_manager_update_state(NMManager *manager);

static void connection_changed(NMManager *self, NMSettingsConnection *sett_conn);
static void device_sleep_cb(NMDevice *device, GParamSpec *pspec, NMManager *self);

static void
settings_startup_complete_changed(NMSettings *settings, GParamSpec *pspec, NMManager *self);

static void retry_connections_for_parent_device(NMManager *self, NMDevice *device);

static void
active_connection_state_changed(NMActiveConnection *active, GParamSpec *pspec, NMManager *self);
static void
active_connection_default_changed(NMActiveConnection *active, GParamSpec *pspec, NMManager *self);
static void active_connection_parent_active(NMActiveConnection *active,
                                            NMActiveConnection *parent_ac,
                                            NMManager          *self);

static NMActiveConnection *active_connection_find(NMManager              *self,
                                                  NMSettingsConnection   *sett_conn,
                                                  const char             *uuid,
                                                  NMActiveConnectionState max_state,
                                                  gboolean                also_waiting_auth,
                                                  GPtrArray             **out_all_matching);

static NMConnectivity *concheck_get_mgr(NMManager *self);

static void _internal_activation_auth_done(NMManager          *self,
                                           NMActiveConnection *active,
                                           gboolean            success,
                                           const char         *error_desc);
static void _add_and_activate_auth_done(NMManager                      *self,
                                        AsyncOpType                     async_op_type,
                                        NMActiveConnection             *active,
                                        NMConnection                   *connection,
                                        GDBusMethodInvocation          *invocation,
                                        NMSettingsConnectionPersistMode persist_mode,
                                        gboolean                        is_volatile,
                                        gboolean                        success,
                                        const char                     *error_desc);
static void _activation_auth_done(NMManager             *self,
                                  NMActiveConnection    *active,
                                  GDBusMethodInvocation *invocation,
                                  gboolean               success,
                                  const char            *error_desc);

static void _rfkill_update(NMManager *self, NMRfkillType rtype);

static DevConData *_devcon_lookup_data(NMManager            *self,
                                       NMDevice             *device,
                                       NMSettingsConnection *sett_conn,
                                       gboolean              create,
                                       gboolean              log_creation);

/*****************************************************************************/

static NM_CACHED_QUARK_FCN("autoconnect-root", autoconnect_root_quark);

/*****************************************************************************/

static GVariant *
_version_info_get(void)
{
    const guint32 arr[] = {
        /* The array contains as first element NM_VERSION, which can be
         * used to numerically compare the version (see also NM_ENCODE_VERSION,
         * nm_utils_version(), nm_encode_version() and nm_decode_version(). */
        NM_VERSION,

        /* The following elements of the array are a bitfield of capabilities.
         * These capabilities should only depend on compile-time abilities
         * (unlike NM_MANAGER_CAPABILITIES, NMCapability). The supported values
         * are from NMVersionInfoCapability enum. This way to expose capabilities
         * is more cumbersome but more efficient compared to NM_MANAGER_CAPABILITIES.
         * As such, it is cheap to add capabilities for something, where you would
         * avoid it as NM_MANAGER_CAPABILITIES due to the overhead.
         *
         * Each of the array's elements has 32 bits. This means that capabilities
         * with index 0-31 goes to element #1, with index 32-63 to element #2,
         * with index 64-95 to element #3 and so on. */
        (1 << NM_VERSION_INFO_CAPABILITY_SYNC_ROUTE_WITH_TABLE)
            | (1 << NM_VERSION_INFO_CAPABILITY_IP4_FORWARDING)
            | (1 << NM_VERSION_INFO_CAPABILITY_SRIOV_PRESERVE_ON_DOWN),
    };

    return nm_g_variant_new_au(arr, G_N_ELEMENTS(arr));
}

/*****************************************************************************/

static gboolean
_connection_is_vpn(NMConnection *connection)
{
    const char *type;

    type = nm_connection_get_connection_type(connection);
    if (type)
        return nm_streq(type, NM_SETTING_VPN_SETTING_NAME);

    /* we have an incomplete (invalid) connection at hand. That can only
     * happen during AddAndActivate. Determine whether it's VPN type based
     * on the existence of a [vpn] section. */
    return !!nm_connection_get_setting_vpn(connection);
}

/*****************************************************************************/

static gboolean
concheck_enabled(NMManager *self, gboolean *out_changed)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    guint             check_enabled;

    check_enabled = nm_connectivity_check_enabled(concheck_get_mgr(self)) ? 1 : 2;
    if (priv->connectivity_check_enabled_last == check_enabled)
        NM_SET_OUT(out_changed, FALSE);
    else {
        NM_SET_OUT(out_changed, TRUE);
        priv->connectivity_check_enabled_last = check_enabled;
    }
    return check_enabled == 1;
}

static void
concheck_config_changed_cb(NMConnectivity *connectivity, NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;
    gboolean          changed;

    concheck_enabled(self, &changed);
    if (changed)
        _notify(self, PROP_CONNECTIVITY_CHECK_ENABLED);

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst)
        nm_device_check_connectivity_update_interval(device);
}

static NMConnectivity *
concheck_get_mgr(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    if (G_UNLIKELY(!priv->concheck_mgr)) {
        priv->concheck_mgr = g_object_ref(nm_connectivity_get());
        g_signal_connect(priv->concheck_mgr,
                         NM_CONNECTIVITY_CONFIG_CHANGED,
                         G_CALLBACK(concheck_config_changed_cb),
                         self);
    }
    return priv->concheck_mgr;
}

/*****************************************************************************/

static AsyncOpData *
_async_op_data_new_authorize_activate_internal(NMManager *self, NMActiveConnection *active_take)
{
    AsyncOpData *async_op_data;

    async_op_data                 = g_slice_new0(AsyncOpData);
    async_op_data->async_op_type  = ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_INTERNAL;
    async_op_data->self           = g_object_ref(self);
    async_op_data->ac_auth.active = active_take;
    c_list_link_tail(&NM_MANAGER_GET_PRIVATE(self)->async_op_lst_head,
                     &async_op_data->async_op_lst);
    return async_op_data;
}

static AsyncOpData *
_async_op_data_new_ac_auth_activate_user(NMManager             *self,
                                         NMActiveConnection    *active_take,
                                         GDBusMethodInvocation *invocation_take)
{
    AsyncOpData *async_op_data;

    async_op_data                                   = g_slice_new0(AsyncOpData);
    async_op_data->async_op_type                    = ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_USER;
    async_op_data->self                             = g_object_ref(self);
    async_op_data->ac_auth.active                   = active_take;
    async_op_data->ac_auth.activate_user.invocation = invocation_take;
    c_list_link_tail(&NM_MANAGER_GET_PRIVATE(self)->async_op_lst_head,
                     &async_op_data->async_op_lst);
    return async_op_data;
}

static AsyncOpData *
_async_op_data_new_ac_auth_add_and_activate(NMManager                      *self,
                                            AsyncOpType                     async_op_type,
                                            NMActiveConnection             *active_take,
                                            GDBusMethodInvocation          *invocation_take,
                                            NMConnection                   *connection_take,
                                            NMSettingsConnectionPersistMode persist_mode,
                                            gboolean                        is_volatile)
{
    AsyncOpData *async_op_data;

    nm_assert(NM_IN_SET(async_op_type,
                        ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE,
                        ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE2));

    async_op_data                                        = g_slice_new0(AsyncOpData);
    async_op_data->async_op_type                         = async_op_type;
    async_op_data->self                                  = g_object_ref(self);
    async_op_data->ac_auth.active                        = active_take;
    async_op_data->ac_auth.add_and_activate.invocation   = invocation_take;
    async_op_data->ac_auth.add_and_activate.connection   = connection_take;
    async_op_data->ac_auth.add_and_activate.persist_mode = persist_mode;
    async_op_data->ac_auth.add_and_activate.is_volatile  = is_volatile;
    c_list_link_tail(&NM_MANAGER_GET_PRIVATE(self)->async_op_lst_head,
                     &async_op_data->async_op_lst);
    return async_op_data;
}

static void
_async_op_complete_ac_auth_cb(NMActiveConnection *active,
                              gboolean            success,
                              const char         *error_desc,
                              gpointer            user_data)
{
    AsyncOpData *async_op_data = user_data;

    nm_assert(async_op_data);
    nm_assert(NM_IS_MANAGER(async_op_data->self));
    nm_assert(
        nm_c_list_contains_entry(&NM_MANAGER_GET_PRIVATE(async_op_data->self)->async_op_lst_head,
                                 async_op_data,
                                 async_op_lst));
    nm_assert(NM_IS_ACTIVE_CONNECTION(active));
    nm_assert(active == async_op_data->ac_auth.active);

    c_list_unlink(&async_op_data->async_op_lst);

    switch (async_op_data->async_op_type) {
    case ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_INTERNAL:
        _internal_activation_auth_done(async_op_data->self,
                                       async_op_data->ac_auth.active,
                                       success,
                                       error_desc);
        break;
    case ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_USER:
        _activation_auth_done(async_op_data->self,
                              async_op_data->ac_auth.active,
                              async_op_data->ac_auth.activate_user.invocation,
                              success,
                              error_desc);
        break;
    case ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE:
    case ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE2:
        _add_and_activate_auth_done(async_op_data->self,
                                    async_op_data->async_op_type,
                                    async_op_data->ac_auth.active,
                                    async_op_data->ac_auth.add_and_activate.connection,
                                    async_op_data->ac_auth.add_and_activate.invocation,
                                    async_op_data->ac_auth.add_and_activate.persist_mode,
                                    async_op_data->ac_auth.add_and_activate.is_volatile,
                                    success,
                                    error_desc);
        g_object_unref(async_op_data->ac_auth.add_and_activate.connection);
        break;
    default:
        nm_assert_not_reached();
        break;
    }

    g_object_unref(async_op_data->ac_auth.active);
    g_object_unref(async_op_data->self);
    g_slice_free(AsyncOpData, async_op_data);
}

/*****************************************************************************/

typedef struct {
    int     ifindex;
    guint32 aspired_metric;
    guint32 effective_metric;
} DeviceRouteMetricData;

static DeviceRouteMetricData *
_device_route_metric_data_new(int ifindex, guint32 aspired_metric, guint32 effective_metric)
{
    DeviceRouteMetricData *data;

    nm_assert(ifindex > 0);

    /* For IPv4, metrics can use the entire uint32 bit range. For IPv6,
     * zero is treated like 1024. Since we handle IPv4 and IPv6 identically,
     * we cannot allow a zero metric here.
     */
    nm_assert(aspired_metric > 0);
    nm_assert(effective_metric == 0 || aspired_metric <= effective_metric);

    data                   = g_slice_new0(DeviceRouteMetricData);
    data->ifindex          = ifindex;
    data->aspired_metric   = aspired_metric;
    data->effective_metric = effective_metric ?: aspired_metric;
    return data;
}

static guint
_device_route_metric_data_by_ifindex_hash(gconstpointer p)
{
    const DeviceRouteMetricData *data = p;

    return nm_hash_val(1030338191, data->ifindex);
}

static gboolean
_device_route_metric_data_by_ifindex_equal(gconstpointer pa, gconstpointer pb)
{
    const DeviceRouteMetricData *a = pa;
    const DeviceRouteMetricData *b = pb;

    return a->ifindex == b->ifindex;
}

static guint32
_device_route_metric_get(NMManager   *self,
                         int          ifindex,
                         NMDeviceType device_type,
                         gboolean     lookup_only,
                         guint32     *out_aspired_metric)
{
    NMManagerPrivate            *priv;
    const DeviceRouteMetricData *d2;
    DeviceRouteMetricData       *data;
    DeviceRouteMetricData        data_lookup;
    const NMDedupMultiHeadEntry *all_links_head;
    NMPObject                    links_needle;
    guint                        n_links;
    gboolean                     cleaned = FALSE;
    GHashTableIter               h_iter;
    guint32                      metric;

    g_return_val_if_fail(NM_IS_MANAGER(self), 0);

    NM_SET_OUT(out_aspired_metric, 0);

    if (ifindex <= 0) {
        if (lookup_only)
            return 0;
        metric = nm_device_get_route_metric_default(device_type);
        NM_SET_OUT(out_aspired_metric, metric);
        return metric;
    }

    priv = NM_MANAGER_GET_PRIVATE(self);

    if (lookup_only && !priv->device_route_metrics)
        return 0;

    if (G_UNLIKELY(!priv->device_route_metrics)) {
        const GHashTable              *h;
        const NMConfigDeviceStateData *device_state;

        priv->device_route_metrics =
            g_hash_table_new_full(_device_route_metric_data_by_ifindex_hash,
                                  _device_route_metric_data_by_ifindex_equal,
                                  NULL,
                                  nm_g_slice_free_fcn(DeviceRouteMetricData));
        cleaned = TRUE;

        /* we need to pre-populate the cache for all (still existing) devices from the state-file */
        h = nm_config_device_state_get_all(priv->config);
        if (!h)
            goto initited;

        g_hash_table_iter_init(&h_iter, (GHashTable *) h);
        while (g_hash_table_iter_next(&h_iter, NULL, (gpointer *) &device_state)) {
            if (!device_state->route_metric_default_effective)
                continue;
            if (!nm_platform_link_get(priv->platform, device_state->ifindex)) {
                /* we have the entry in the state file, but (currently) no such
                 * ifindex exists in platform. Most likely the entry is obsolete,
                 * hence we skip it. */
                continue;
            }
            if (!g_hash_table_add(
                    priv->device_route_metrics,
                    _device_route_metric_data_new(device_state->ifindex,
                                                  device_state->route_metric_default_aspired,
                                                  device_state->route_metric_default_effective)))
                nm_assert_not_reached();
        }
    }

initited:
    data_lookup.ifindex = ifindex;

    data = g_hash_table_lookup(priv->device_route_metrics, &data_lookup);
    if (data)
        goto out;
    if (lookup_only)
        return 0;

    if (!cleaned) {
        /* get the number of all links in the platform cache. */
        all_links_head = nm_platform_lookup_all(priv->platform,
                                                NMP_CACHE_ID_TYPE_OBJECT_TYPE,
                                                nmp_object_stackinit_id_link(&links_needle, 1));
        n_links        = all_links_head ? all_links_head->len : 0;

        /* on systems where a lot of devices are created and go away, the index contains
         * a lot of stale entries. We must from time to time clean them up.
         *
         * Do do this cleanup, whenever we have more entries then 2 times the number of links. */
        if (G_UNLIKELY(g_hash_table_size(priv->device_route_metrics) > NM_MAX(20u, n_links * 2u))) {
            /* from time to time, we need to do some house-keeping and prune stale entries.
             * Otherwise, on a system where interfaces frequently come and go (docker), we
             * keep growing this cache for ifindexes that no longer exist. */
            g_hash_table_iter_init(&h_iter, priv->device_route_metrics);
            while (g_hash_table_iter_next(&h_iter, NULL, (gpointer *) &d2)) {
                if (!nm_platform_link_get(priv->platform, d2->ifindex))
                    g_hash_table_iter_remove(&h_iter);
            }
            cleaned = TRUE;
        }
    }

    data =
        _device_route_metric_data_new(ifindex, nm_device_get_route_metric_default(device_type), 0);

    /* unfortunately, there is no stright forward way to lookup all reserved metrics.
     * Note, that we don't only have to know which metrics are currently reserved,
     * but also, which metrics are now seemingly un-used but caused another reserved
     * metric to be bumped. Hence, the naive O(n^2) search :(
     *
     * Well, technically, since we limit bumping the metric to 50, this entire
     * loop runs at most 50 times, so it's still O(n). Let's just say, it's not
     * very efficient. */
again:
    g_hash_table_iter_init(&h_iter, priv->device_route_metrics);
    while (g_hash_table_iter_next(&h_iter, NULL, (gpointer *) &d2)) {
        if (data->effective_metric < d2->aspired_metric
            || data->effective_metric > d2->effective_metric) {
            /* no overlap. Skip. */
            continue;
        }
        if (!cleaned && !nm_platform_link_get(priv->platform, d2->ifindex)) {
            /* the metric seems taken, but there is no such interface. This entry
             * is stale, forget about it. */
            g_hash_table_iter_remove(&h_iter);
            continue;
        }

        if (d2->effective_metric == G_MAXUINT32) {
            /* we cannot bump the metric any further. Done.
             *
             * Actually, this can currently not happen because the aspired_metric
             * are small numbers and we limit the bumping to 50. Still, for
             * completeness... */
            data->effective_metric = G_MAXUINT32;
            break;
        }

        if (d2->effective_metric - data->aspired_metric >= 50) {
            /* as one active interface reserves an entire range of metrics
             * (from aspired_metric to effective_metric), that means if you
             * alternatingly activate two interfaces, their metric will
             * bump each other.
             *
             * Limit this, bump the metric at most 50 points. */
            data->effective_metric = data->aspired_metric + 50;
            break;
        }

        /* bump the metric, and search again. */
        data->effective_metric = d2->effective_metric + 1;
        goto again;
    }

    _LOGT(LOGD_DEVICE,
          "default-route-metric: ifindex %d reserves metric %u (aspired %u)",
          data->ifindex,
          data->effective_metric,
          data->aspired_metric);

    if (!g_hash_table_add(priv->device_route_metrics, data))
        nm_assert_not_reached();

out:
    NM_SET_OUT(out_aspired_metric, data->aspired_metric);
    return data->effective_metric;
}

guint32
nm_manager_device_route_metric_reserve(NMManager *self, int ifindex, NMDeviceType device_type)
{
    guint32 metric;

    metric = _device_route_metric_get(self, ifindex, device_type, FALSE, NULL);
    nm_assert(metric != 0);
    return metric;
}

void
nm_manager_device_route_metric_clear(NMManager *self, int ifindex)
{
    NMManagerPrivate     *priv;
    DeviceRouteMetricData data_lookup;

    priv = NM_MANAGER_GET_PRIVATE(self);

    if (!priv->device_route_metrics)
        return;
    data_lookup.ifindex = ifindex;
    if (g_hash_table_remove(priv->device_route_metrics, &data_lookup)) {
        _LOGT(LOGD_DEVICE, "default-route-metric: ifindex %d released", ifindex);
    }
}

/*****************************************************************************/

static void
_delete_volatile_connection_do(NMManager *self, NMSettingsConnection *connection)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    if (!NM_FLAGS_ANY(nm_settings_connection_get_flags(connection),
                      NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                          | NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL))
        return;
    if (!nm_settings_has_connection(priv->settings, connection))
        return;
    if (active_connection_find(self,
                               connection,
                               NULL,
                               NM_ACTIVE_CONNECTION_STATE_DEACTIVATED,
                               TRUE,
                               NULL))
        return;

    _LOGD(LOGD_DEVICE,
          "volatile connection disconnected. Deleting connection '%s' (%s)",
          nm_settings_connection_get_id(connection),
          nm_settings_connection_get_uuid(connection));
    nm_settings_connection_delete(connection, FALSE);
}

/* Returns: whether to notify D-Bus of the removal or not */
static gboolean
active_connection_remove(NMManager *self, NMActiveConnection *active)
{
    NMManagerPrivate                     *priv       = NM_MANAGER_GET_PRIVATE(self);
    gs_unref_object NMSettingsConnection *connection = NULL;
    gboolean                              notify;

    nm_assert(NM_IS_ACTIVE_CONNECTION(active));
    nm_assert(c_list_contains(&priv->active_connections_lst_head, &active->active_connections_lst));

    notify = nm_dbus_object_is_exported(NM_DBUS_OBJECT(active));

    c_list_unlink(&active->active_connections_lst);
    g_signal_emit(self, signals[ACTIVE_CONNECTION_REMOVED], 0, active);
    g_signal_handlers_disconnect_by_func(active, active_connection_state_changed, self);
    g_signal_handlers_disconnect_by_func(active, active_connection_default_changed, self);
    g_signal_handlers_disconnect_by_func(active, active_connection_parent_active, self);

    connection = nm_g_object_ref(nm_active_connection_get_settings_connection(active));

    nm_dbus_object_clear_and_unexport(&active);

    if (connection)
        _delete_volatile_connection_do(self, connection);

    return notify;
}

static gboolean
_active_connection_cleanup(gpointer user_data)
{
    NMManager          *self = NM_MANAGER(user_data);
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *ac, *ac_safe;

    priv->ac_cleanup_id = 0;

    g_object_freeze_notify(G_OBJECT(self));
    c_list_for_each_entry_safe (ac,
                                ac_safe,
                                &priv->active_connections_lst_head,
                                active_connections_lst) {
        if (nm_active_connection_get_state(ac) == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
            if (active_connection_remove(self, ac))
                _notify(self, PROP_ACTIVE_CONNECTIONS);
        }
    }
    g_object_thaw_notify(G_OBJECT(self));

    return FALSE;
}

static void
active_connection_state_changed(NMActiveConnection *active, GParamSpec *pspec, NMManager *self)
{
    NMManagerPrivate       *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnectionState state;
    NMSettingsConnection   *con;

    state = nm_active_connection_get_state(active);
    if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
        /* Destroy active connections from an idle handler to ensure that
         * their last property change notifications go out, which wouldn't
         * happen if we destroyed them immediately when their state was set
         * to DEACTIVATED.
         */
        if (!priv->ac_cleanup_id)
            priv->ac_cleanup_id = g_idle_add(_active_connection_cleanup, self);

        con = nm_active_connection_get_settings_connection(active);
        if (con)
            g_object_set_qdata(G_OBJECT(con), autoconnect_root_quark(), NULL);
    }

    nm_manager_update_state(self);
}

static void
active_connection_default_changed(NMActiveConnection *active, GParamSpec *pspec, NMManager *self)
{
    nm_manager_update_state(self);
}

/**
 * active_connection_add():
 * @self: the #NMManager
 * @active: the #NMActiveConnection to manage
 *
 * Begins to track and manage @active.  Increases the refcount of @active.
 */
static void
active_connection_add(NMManager *self, NMActiveConnection *active)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    nm_assert(NM_IS_ACTIVE_CONNECTION(active));
    nm_assert(!c_list_is_linked(&active->active_connections_lst));

    c_list_link_tail(&priv->active_connections_lst_head, &active->active_connections_lst);
    g_object_ref(active);

    g_signal_connect(active,
                     "notify::" NM_ACTIVE_CONNECTION_STATE,
                     G_CALLBACK(active_connection_state_changed),
                     self);
    g_signal_connect(active,
                     "notify::" NM_ACTIVE_CONNECTION_DEFAULT,
                     G_CALLBACK(active_connection_default_changed),
                     self);
    g_signal_connect(active,
                     "notify::" NM_ACTIVE_CONNECTION_DEFAULT6,
                     G_CALLBACK(active_connection_default_changed),
                     self);

    if (!nm_dbus_object_is_exported(NM_DBUS_OBJECT(active)))
        nm_dbus_object_export(NM_DBUS_OBJECT(active));

    g_signal_emit(self, signals[ACTIVE_CONNECTION_ADDED], 0, active);

    _notify(self, PROP_ACTIVE_CONNECTIONS);
}

const CList *
nm_manager_get_active_connections(NMManager *manager)
{
    return &NM_MANAGER_GET_PRIVATE(manager)->active_connections_lst_head;
}

static NMActiveConnection *
active_connection_find(
    NMManager              *self,
    NMSettingsConnection   *sett_conn,
    const char             *uuid,
    NMActiveConnectionState max_state /* candidates in state @max_state will be found */,
    gboolean                also_waiting_auth /* return also ACs waiting authorization */,
    GPtrArray             **out_all_matching)
{
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *ac;
    NMActiveConnection *best_ac = NULL;
    GPtrArray          *all     = NULL;

    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!out_all_matching || !*out_all_matching);

    c_list_for_each_entry_prev (ac, &priv->active_connections_lst_head, active_connections_lst) {
        NMSettingsConnection *ac_conn;

        ac_conn = nm_active_connection_get_settings_connection(ac);
        if (sett_conn && sett_conn != ac_conn)
            continue;
        if (uuid && !nm_streq0(uuid, nm_settings_connection_get_uuid(ac_conn)))
            continue;
        if (nm_active_connection_get_state(ac) > max_state)
            continue;

        if (!out_all_matching)
            return ac;

        if (!best_ac) {
            best_ac = ac;
            continue;
        }

        if (!all) {
            all = g_ptr_array_new_with_free_func(g_object_unref);
            g_ptr_array_add(all, g_object_ref(best_ac));
        }
        g_ptr_array_add(all, g_object_ref(ac));
    }

    if (!best_ac) {
        AsyncOpData *async_op_data;

        if (!also_waiting_auth)
            return NULL;

        c_list_for_each_entry (async_op_data, &priv->async_op_lst_head, async_op_lst) {
            NMSettingsConnection *ac_conn;

            ac      = async_op_data->ac_auth.active;
            ac_conn = _nm_active_connection_get_settings_connection(ac);
            if (sett_conn && sett_conn != ac_conn)
                continue;
            if (uuid && !nm_streq0(uuid, nm_settings_connection_get_uuid(ac_conn)))
                continue;

            if (!out_all_matching)
                return ac;

            if (!best_ac) {
                best_ac = ac;
                continue;
            }

            if (!all) {
                all = g_ptr_array_new_with_free_func(g_object_unref);
                g_ptr_array_add(all, g_object_ref(best_ac));
            }
            g_ptr_array_add(all, g_object_ref(ac));
        }

        if (!best_ac)
            return NULL;
    }

    /* as an optimization, we only allocate out_all_matching, if there are more
     * than one result. If there is only one result, we only return the single
     * element and don't bother allocating an array. That's the common case.
     *
     * Also, in case we have multiple results, we return the *first* one
     * as @best_ac. */
    nm_assert(!all || (all->len >= 2 && all->pdata[0] == best_ac));

    *out_all_matching = all;
    return best_ac;
}

static NMActiveConnection *
active_connection_find_by_connection(NMManager              *self,
                                     NMSettingsConnection   *sett_conn,
                                     NMConnection           *connection,
                                     NMActiveConnectionState max_state,
                                     GPtrArray             **out_all_matching)
{
    nm_assert(NM_IS_MANAGER(self));
    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!connection || NM_IS_CONNECTION(connection));
    nm_assert(sett_conn || connection);
    nm_assert(!connection || !sett_conn
              || connection == nm_settings_connection_get_connection(sett_conn));

    /* Depending on whether connection is a settings connection,
     * either lookup by object-identity of @connection, or compare the UUID */
    return active_connection_find(self,
                                  sett_conn,
                                  sett_conn ? NULL : nm_connection_get_uuid(connection),
                                  max_state,
                                  FALSE,
                                  out_all_matching);
}

typedef struct {
    NMManager *self;
    gboolean   for_auto_activation;
} GetActivatableConnectionsFilterData;

static gboolean
_get_activatable_connections_filter(NMSettings           *settings,
                                    NMSettingsConnection *sett_conn,
                                    gpointer              user_data)
{
    const GetActivatableConnectionsFilterData *d = user_data;
    NMConnectionMultiConnect                   multi_connect;

    if (NM_FLAGS_ANY(nm_settings_connection_get_flags(sett_conn),
                     NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                         | NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL))
        return FALSE;

    multi_connect =
        _nm_connection_get_multi_connect(nm_settings_connection_get_connection(sett_conn));
    if (multi_connect == NM_CONNECTION_MULTI_CONNECT_MULTIPLE
        || (multi_connect == NM_CONNECTION_MULTI_CONNECT_MANUAL_MULTIPLE
            && !d->for_auto_activation))
        return TRUE;

    /* the connection is activatable, if it has no active-connections that are in state
     * activated, activating, or waiting to be activated. */
    return !active_connection_find(d->self,
                                   sett_conn,
                                   NULL,
                                   NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
                                   FALSE,
                                   NULL);
}

NMSettingsConnection **
nm_manager_get_activatable_connections(NMManager *manager,
                                       gboolean   for_auto_activation,
                                       gboolean   sort,
                                       guint     *out_len)
{
    NMManagerPrivate                         *priv = NM_MANAGER_GET_PRIVATE(manager);
    const GetActivatableConnectionsFilterData d    = {
           .self                = manager,
           .for_auto_activation = for_auto_activation,
    };

    return nm_settings_get_connections_clone(
        priv->settings,
        out_len,
        _get_activatable_connections_filter,
        (gpointer) &d,
        sort ? nm_settings_connection_cmp_autoconnect_priority_p_with_data : NULL,
        NULL);
}

static NMActiveConnection *
active_connection_get_by_path(NMManager *self, const char *path)
{
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *ac;

    ac = nm_dbus_manager_lookup_object_with_type(nm_dbus_object_get_manager(NM_DBUS_OBJECT(self)),
                                                 NM_TYPE_ACTIVE_CONNECTION,
                                                 path);
    if (!ac || c_list_is_empty(&ac->active_connections_lst))
        return NULL;

    nm_assert(c_list_contains(&priv->active_connections_lst_head, &ac->active_connections_lst));
    return ac;
}

/*****************************************************************************/

static guint32
_autoconnect_retries_initial(NMSettingsConnection *sett_conn)
{
    NMSettingConnection *s_con;
    int                  retries = -1;

    s_con = nm_connection_get_setting_connection(nm_settings_connection_get_connection(sett_conn));
    if (s_con)
        retries = nm_setting_connection_get_autoconnect_retries(s_con);

    if (retries == -1)
        retries = nm_config_data_get_autoconnect_retries_default(NM_CONFIG_GET_DATA);

    nm_assert(retries >= 0 && retries <= G_MAXINT32);

    if (retries == 0)
        return NM_AUTOCONNECT_RETRIES_FOREVER;
    return (guint32) retries;
}

static gboolean
_autoconnect_retries_set(NMManager *self, DevConData *data, guint32 retries, gboolean is_reset)
{
    gboolean changed = FALSE;
    gint32   blocked_until_sec;

    nm_assert(data);

    if (!data->autoconnect.initialized || data->autoconnect.retries != retries) {
        data->autoconnect.initialized = TRUE;
        data->autoconnect.retries     = retries;
        changed                       = TRUE;
    }

    if (retries != 0) {
        blocked_until_sec = 0;
    } else {
        /* NOTE: the blocked time must be identical for all connections, otherwise
         * the tracking of resetting the retry count in NMPolicy needs adjustment
         * in _connection_autoconnect_retries_set() (as it would need to re-evaluate
         * the next-timeout every time a connection gets blocked). */
        blocked_until_sec =
            nm_utils_get_monotonic_timestamp_sec() + AUTOCONNECT_RESET_RETRIES_TIMER_SEC;
    }

    if (data->autoconnect.blocked_until_sec != blocked_until_sec) {
        data->autoconnect.blocked_until_sec = blocked_until_sec;
        changed                             = TRUE;
    }

    if (changed) {
        char sbuf[200];

        _LOGT(LOGD_SETTINGS,
              "block-autoconnect: " DEV_CON_DATA_LOG_FMT ": retries set %u%s%s",
              DEV_CON_DATA_LOG_ARGS_DATA(data),
              retries,
              is_reset ? " (is-reset)" : "",
              blocked_until_sec == 0 ? ""
                                     : nm_sprintf_buf(sbuf,
                                                      " (blocked for %d sec)",
                                                      AUTOCONNECT_RESET_RETRIES_TIMER_SEC));
    }

    return changed;
}

/**
 * nm_manager_devcon_autoconnect_retries_get:
 * @self: the #NMManager
 * @device: the #NMDevice
 * @sett_conn: the #NMSettingsConnection
 *
 * Returns the number of autoconnect retries left for the (device, connection)
 * tuple. If the value is not yet set, initialize it with the value from the
 * connection or with the global default.
 */
guint32
nm_manager_devcon_autoconnect_retries_get(NMManager            *self,
                                          NMDevice             *device,
                                          NMSettingsConnection *sett_conn)
{
    DevConData *data;

    nm_assert(NM_IS_MANAGER(self));
    nm_assert(NM_IS_DEVICE(device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(self == nm_device_get_manager(device));
    nm_assert(self == nm_settings_connection_get_manager(sett_conn));

    data = _devcon_lookup_data(self, device, sett_conn, TRUE, FALSE);

    if (G_UNLIKELY(!data->autoconnect.initialized))
        _autoconnect_retries_set(self, data, _autoconnect_retries_initial(sett_conn), FALSE);

    return data->autoconnect.retries;
}

void
nm_manager_devcon_autoconnect_retries_set(NMManager            *self,
                                          NMDevice             *device,
                                          NMSettingsConnection *sett_conn,
                                          guint32               retries)
{
    _autoconnect_retries_set(self,
                             _devcon_lookup_data(self, device, sett_conn, TRUE, FALSE),
                             retries,
                             FALSE);
}

gboolean
nm_manager_devcon_autoconnect_retries_reset(NMManager            *self,
                                            NMDevice             *device,
                                            NMSettingsConnection *sett_conn)
{
    DevConData *data;
    guint32     retries_initial;
    gboolean    changed = FALSE;

    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));

    retries_initial = _autoconnect_retries_initial(sett_conn);

    if (device) {
        return _autoconnect_retries_set(self,
                                        _devcon_lookup_data(self, device, sett_conn, TRUE, FALSE),
                                        retries_initial,
                                        TRUE);
    }

    c_list_for_each_entry (data, &sett_conn->devcon_con_lst_head, con_lst) {
        if (_autoconnect_retries_set(self, data, retries_initial, TRUE))
            changed = TRUE;
    }

    return changed;
}

/**
 * nm_manager_devcon_autoconnect_reset_reconnect_all:
 * @self: the #NMManager
 * @device: the #NMDevice
 * @sett_conn: the #NMSettingsConnection
 * @only_no_secrets: boolean to reset all reasons or only no secrets.
 *
 * Returns a boolean indicating if something changed or not when resetting the
 * blocked reasons. If a #NMDevice is present then we also reset the reasons
 * for the (device, connection) tuple.
 */
gboolean
nm_manager_devcon_autoconnect_reset_reconnect_all(NMManager            *self,
                                                  NMDevice             *device,
                                                  NMSettingsConnection *sett_conn,
                                                  gboolean              only_no_secrets)
{
    gboolean changed = FALSE;

    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));

    if (only_no_secrets) {
        /* we only reset the no-secrets blocked flag. */
        if (nm_settings_connection_autoconnect_blocked_reason_set(
                sett_conn,
                NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NO_SECRETS,
                FALSE)) {
            /* maybe the connection is still blocked afterwards for other reasons
             * and in the larger picture nothing changed. Check if the connection
             * is still blocked or not. */
            if (!nm_settings_connection_autoconnect_is_blocked(sett_conn))
                changed = TRUE;
        }

        return changed;
    }

    /* we reset the tries-count and any blocked-reason... */

    nm_manager_devcon_autoconnect_retries_reset(self, NULL, sett_conn);

    if (device) {
        if (nm_manager_devcon_autoconnect_blocked_reason_set(
                self,
                device,
                sett_conn,
                NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_FAILED,
                FALSE))
            changed = TRUE;
    }

    /* we remove all the blocked reason from the connection, if something
     * happened, then it means the status changed */
    if (nm_settings_connection_autoconnect_blocked_reason_set(
            sett_conn,
            NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NO_SECRETS
                | NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_USER_REQUEST,
            FALSE))
        changed = TRUE;

    return changed;
}

gint32
nm_manager_devcon_autoconnect_retries_blocked_until(NMManager            *self,
                                                    NMDevice             *device,
                                                    NMSettingsConnection *sett_conn)
{
    DevConData *data;
    gint32      min_stamp;

    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));

    if (device) {
        data = _devcon_lookup_data(self, device, sett_conn, FALSE, FALSE);

        if (!data)
            return 0;

        return data->autoconnect.blocked_until_sec;
    }

    min_stamp = 0;
    c_list_for_each_entry (data, &sett_conn->devcon_con_lst_head, con_lst) {
        gint32 condev_stamp = data->autoconnect.blocked_until_sec;

        if (condev_stamp == 0)
            continue;

        if (min_stamp == 0 || min_stamp > condev_stamp)
            min_stamp = condev_stamp;
    }

    return min_stamp;
}

gboolean
nm_manager_devcon_autoconnect_is_blocked(NMManager            *self,
                                         NMDevice             *device,
                                         NMSettingsConnection *sett_conn)
{
    DevConData *data;

    nm_assert(NM_IS_DEVICE(device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));

    if (nm_settings_connection_autoconnect_is_blocked(sett_conn))
        return TRUE;

    data = _devcon_lookup_data(self, device, sett_conn, FALSE, FALSE);

    if (!data)
        return FALSE;

    if (data->autoconnect.blocked_reason != NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NONE)
        return TRUE;

    if (data->autoconnect.initialized && data->autoconnect.retries == 0)
        return TRUE;

    return FALSE;
}

gboolean
nm_manager_devcon_autoconnect_blocked_reason_set(NMManager                         *self,
                                                 NMDevice                          *device,
                                                 NMSettingsConnection              *sett_conn,
                                                 NMSettingsAutoconnectBlockedReason value,
                                                 gboolean                           set)
{
    NMSettingsAutoconnectBlockedReason v;
    DevConData                        *data;
    gboolean                           changed = FALSE;
    char                               buf[100];

    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!device || NM_IS_DEVICE(device));
    nm_assert(value != NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NONE);
    nm_assert(!NM_FLAGS_ANY(value, ~(NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_FAILED)));

    if (!sett_conn) {
        if (!device)
            g_return_val_if_reached(FALSE);
        c_list_for_each_entry (data, &device->devcon_dev_lst_head, dev_lst) {
            v = data->autoconnect.blocked_reason;
            v = NM_FLAGS_ASSIGN(v, value, set);

            if (data->autoconnect.blocked_reason == v)
                continue;

            _LOGT(LOGD_SETTINGS,
                  "block-autoconnect: " DEV_CON_DATA_LOG_FMT ": set blocked reason %s",
                  DEV_CON_DATA_LOG_ARGS_DATA(data),
                  nm_settings_autoconnect_blocked_reason_to_string(v, buf, sizeof(buf)));
            data->autoconnect.blocked_reason = v;
            changed                          = TRUE;
        }
        return changed;
    }

    if (device) {
        data = _devcon_lookup_data(self, device, sett_conn, TRUE, TRUE);
        v    = data->autoconnect.blocked_reason;
        v    = NM_FLAGS_ASSIGN(v, value, set);

        if (data->autoconnect.blocked_reason == v)
            return FALSE;

        data->autoconnect.blocked_reason = v;
        _LOGT(LOGD_SETTINGS,
              "block-autoconnect: " DEV_CON_DATA_LOG_FMT ": set blocked reason %s",
              DEV_CON_DATA_LOG_ARGS_DATA(data),
              nm_settings_autoconnect_blocked_reason_to_string(v, buf, sizeof(buf)));
        return TRUE;
    }

    c_list_for_each_entry (data, &sett_conn->devcon_con_lst_head, con_lst) {
        v = data->autoconnect.blocked_reason;
        v = NM_FLAGS_ASSIGN(v, value, set);

        if (data->autoconnect.blocked_reason == v)
            continue;

        _LOGT(LOGD_SETTINGS,
              "block-autoconnect: " DEV_CON_DATA_LOG_FMT ": set blocked reason %s",
              DEV_CON_DATA_LOG_ARGS_DATA(data),
              nm_settings_autoconnect_blocked_reason_to_string(v, buf, sizeof(buf)));
        data->autoconnect.blocked_reason = v;
        changed                          = TRUE;
    }

    return changed;
}

/*****************************************************************************/

static guint
_devcon_data_hash(gconstpointer ptr)
{
    const DevConData *data = ptr;

    nm_assert(NM_IS_DEVICE(data->device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(data->sett_conn));

    return nm_hash_vals(1832112199u, data->device, data->sett_conn);
}

static gboolean
_devcon_data_equal(gconstpointer ptr_a, gconstpointer ptr_b)
{
    const DevConData *data_a = ptr_a;
    const DevConData *data_b = ptr_b;

    nm_assert(NM_IS_DEVICE(data_a->device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(data_a->sett_conn));
    nm_assert(NM_IS_DEVICE(data_b->device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(data_b->sett_conn));

    return data_a->device == data_b->device && data_a->sett_conn == data_b->sett_conn;
}

static DevConData *
_devcon_lookup_data(NMManager            *self,
                    NMDevice             *device,
                    NMSettingsConnection *sett_conn,
                    gboolean              create,
                    gboolean              log_creation)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    DevConData       *data;
    DevConData        needle;

    nm_assert(NM_IS_DEVICE(device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(self == nm_device_get_manager(device));
    nm_assert(self == nm_settings_connection_get_manager(sett_conn));

    needle.device    = device;
    needle.sett_conn = sett_conn;

    data = g_hash_table_lookup(priv->devcon_data_dict, &needle);

    if (data)
        return data;
    if (!create)
        return NULL;

    data  = g_slice_new(DevConData);
    *data = (DevConData) {
        .device    = device,
        .sett_conn = sett_conn,
        .autoconnect =
            {
                .initialized       = FALSE,
                .retries           = 0,
                .blocked_until_sec = 0,
                .blocked_reason    = NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NONE,
            },
    };
    c_list_link_tail(&device->devcon_dev_lst_head, &data->dev_lst);
    c_list_link_tail(&sett_conn->devcon_con_lst_head, &data->con_lst);

    g_hash_table_add(priv->devcon_data_dict, data);

    if (log_creation) {
        _LOGT(LOGD_SETTINGS,
              "block-autoconnect: " DEV_CON_DATA_LOG_FMT ": entry created (not initialized)",
              DEV_CON_DATA_LOG_ARGS_DATA(data));
    }

    return data;
}

static void
_devcon_remove_data(NMManager *self, DevConData *data)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    nm_assert(data);
    nm_assert(NM_IS_DEVICE(data->device));
    nm_assert(NM_IS_SETTINGS_CONNECTION(data->sett_conn));
    nm_assert(data == _devcon_lookup_data(self, data->device, data->sett_conn, FALSE, FALSE));

    c_list_unlink_stale(&data->dev_lst);
    c_list_unlink_stale(&data->con_lst);
    g_hash_table_remove(priv->devcon_data_dict, data);
    nm_g_slice_free(data);
}

static gboolean
_devcon_remove_device_all(NMManager *self, NMDevice *device)
{
    DevConData *data;
    gboolean    changed;

    nm_assert(NM_IS_DEVICE(device));

    while ((data = c_list_first_entry(&device->devcon_dev_lst_head, DevConData, dev_lst))) {
        changed = TRUE;
        _devcon_remove_data(self, data);
    }

    return changed;
}

static gboolean
_devcon_remove_sett_conn_all(NMManager *self, NMSettingsConnection *sett_conn)
{
    DevConData *data;
    gboolean    changed = FALSE;

    nm_assert(NM_IS_SETTINGS_CONNECTION(sett_conn));

    while ((data = c_list_first_entry(&sett_conn->devcon_con_lst_head, DevConData, con_lst))) {
        changed = TRUE;
        _devcon_remove_data(self, data);
    }

    return changed;
}

void
nm_manager_notify_delete_settings_connections(NMManager *self, NMSettingsConnection *sett_conn)
{
    _devcon_remove_sett_conn_all(self, sett_conn);
}

/*****************************************************************************/

static void
_config_changed_cb(NMConfig           *config,
                   NMConfigData       *config_data,
                   NMConfigChangeFlags changes,
                   NMConfigData       *old_data,
                   NMManager          *self)
{
    g_object_freeze_notify(G_OBJECT(self));

    if (NM_FLAGS_HAS(changes, NM_CONFIG_CHANGE_GLOBAL_DNS_CONFIG))
        _notify(self, PROP_GLOBAL_DNS_CONFIGURATION);

    if (!nm_streq0(nm_config_data_get_connectivity_uri(config_data),
                   nm_config_data_get_connectivity_uri(old_data))) {
        if ((!nm_config_data_get_connectivity_uri(config_data))
            != (!nm_config_data_get_connectivity_uri(old_data)))
            _notify(self, PROP_CONNECTIVITY_CHECK_AVAILABLE);
        _notify(self, PROP_CONNECTIVITY_CHECK_URI);
    }

    g_object_thaw_notify(G_OBJECT(self));
}

static void
_reload_auth_cb(NMAuthChain *chain, GDBusMethodInvocation *context, gpointer user_data)
{
    NMManager          *self      = NM_MANAGER(user_data);
    NMManagerPrivate   *priv      = NM_MANAGER_GET_PRIVATE(self);
    GError             *ret_error = NULL;
    NMAuthCallResult    result;
    guint32             flags;
    NMAuthSubject      *subject;
    char                s_buf[60];
    NMConfigChangeFlags reload_type = NM_CONFIG_CHANGE_NONE;

    nm_assert(G_IS_DBUS_METHOD_INVOCATION(context));

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));
    flags = GPOINTER_TO_UINT(nm_auth_chain_get_data(chain, "flags"));

    subject = nm_auth_chain_get_subject(chain);

    result = nm_auth_chain_get_result(chain, NM_AUTH_PERMISSION_RELOAD);
    if (result != NM_AUTH_CALL_RESULT_YES) {
        ret_error = g_error_new_literal(NM_MANAGER_ERROR,
                                        NM_MANAGER_ERROR_PERMISSION_DENIED,
                                        "Not authorized to reload configuration");
    } else {
        if (NM_FLAGS_ANY(flags, ~NM_MANAGER_RELOAD_FLAG_ALL)) {
            /* invalid flags */
        } else if (flags == 0)
            reload_type = NM_CONFIG_CHANGE_CAUSE_SIGHUP;
        else {
            if (NM_FLAGS_HAS(flags, NM_MANAGER_RELOAD_FLAG_CONF))
                reload_type |= NM_CONFIG_CHANGE_CAUSE_CONF;
            if (NM_FLAGS_HAS(flags, NM_MANAGER_RELOAD_FLAG_DNS_RC))
                reload_type |= NM_CONFIG_CHANGE_CAUSE_DNS_RC;
            if (NM_FLAGS_HAS(flags, NM_MANAGER_RELOAD_FLAG_DNS_FULL))
                reload_type |= NM_CONFIG_CHANGE_CAUSE_DNS_FULL;
        }

        if (reload_type == NM_CONFIG_CHANGE_NONE) {
            ret_error = g_error_new_literal(NM_MANAGER_ERROR,
                                            NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                                            "Invalid flags for reload");
        }
    }

    nm_audit_log_control_op(NM_AUDIT_OP_RELOAD,
                            nm_sprintf_buf(s_buf, "%u", flags),
                            ret_error == NULL,
                            subject,
                            ret_error ? ret_error->message : NULL);

    if (ret_error) {
        g_dbus_method_invocation_take_error(context, ret_error);
        return;
    }

    nm_config_reload(priv->config, reload_type, TRUE);
    g_dbus_method_invocation_return_value(context, NULL);
}

static void
impl_manager_reload(NMDBusObject                      *obj,
                    const NMDBusInterfaceInfoExtended *interface_info,
                    const NMDBusMethodInfoExtended    *method_info,
                    GDBusConnection                   *connection,
                    const char                        *sender,
                    GDBusMethodInvocation             *invocation,
                    GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;
    guint32           flags;

    g_variant_get(parameters, "(u)", &flags);

    chain = nm_auth_chain_new_context(invocation, _reload_auth_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "flags", GUINT_TO_POINTER(flags), NULL);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_RELOAD, TRUE);
}

/*****************************************************************************/

NMDevice *
nm_manager_get_device_by_path(NMManager *self, const char *path)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    g_return_val_if_fail(path, NULL);

    device =
        nm_dbus_manager_lookup_object_with_type(nm_dbus_object_get_manager(NM_DBUS_OBJECT(self)),
                                                NM_TYPE_DEVICE,
                                                path);
    if (!device || c_list_is_empty(&device->devices_lst))
        return NULL;

    nm_assert(c_list_contains(&priv->devices_lst_head, &device->devices_lst));
    return device;
}

NMDevice *
nm_manager_get_device_by_ifindex(NMManager *self, int ifindex)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    if (ifindex > 0) {
        c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
            if (nm_device_get_ifindex(device) == ifindex)
                return device;
        }
    }

    return NULL;
}

static NMDevice *
find_device_by_permanent_hw_addr(NMManager *self, const char *hwaddr)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;
    const char       *device_addr;
    guint8            hwaddr_bin[_NM_UTILS_HWADDR_LEN_MAX];
    gsize             hwaddr_len;

    g_return_val_if_fail(hwaddr != NULL, NULL);

    if (!_nm_utils_hwaddr_aton(hwaddr, hwaddr_bin, sizeof(hwaddr_bin), &hwaddr_len))
        return NULL;

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        device_addr = nm_device_get_permanent_hw_address(device);
        if (device_addr && nm_utils_hwaddr_matches(hwaddr_bin, hwaddr_len, device_addr, -1))
            return device;
    }
    return NULL;
}

static NMDevice *
find_device_by_ip_iface(NMManager *self, const char *iface)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    g_return_val_if_fail(iface, NULL);

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        if (nm_device_is_real(device) && nm_streq0(nm_device_get_ip_iface(device), iface))
            return device;
    }
    return NULL;
}

/**
 * find_device_by_iface:
 * @self: the #NMManager
 * @iface: the device interface to find
 * @connection: a connection to ensure the returned device is compatible with
 * @port: a port connection to ensure a controller is compatible with
 *
 * Finds a device by interface name, preferring realized devices.  If @port
 * is given, this function will only return controller devices and will ensure
 * @port, when activated, can be a port of the returned controller device.  If
 * @connection is given, this function will only consider devices that are
 * compatible with @connection. If @child is given, this function will only
 * return parent device.
 *
 * Returns: the matching #NMDevice
 */
static NMDevice *
find_device_by_iface(NMManager    *self,
                     const char   *iface,
                     NMConnection *connection,
                     NMConnection *port,
                     NMConnection *child)
{
    NMManagerPrivate *priv     = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *fallback = NULL;
    NMDevice         *candidate;

    g_return_val_if_fail(iface != NULL, NULL);

    c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
        if (!nm_streq(nm_device_get_iface(candidate), iface))
            continue;
        if (connection && !nm_device_check_connection_compatible(candidate, connection, TRUE, NULL))
            continue;
        if (port) {
            if (!nm_device_is_controller(candidate))
                continue;
            if (!nm_device_check_port_connection_compatible(candidate, port))
                continue;
        }
        if (child && !nm_device_can_be_parent(candidate))
            continue;

        if (nm_device_is_real(candidate))
            return candidate;
        else if (!fallback)
            fallback = candidate;
    }
    return fallback;
}

static gboolean
manager_sleeping(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    if (priv->sleeping || !priv->net_enabled)
        return TRUE;
    return FALSE;
}

static const char *
_nm_state_to_string(NMState state)
{
    switch (state) {
    case NM_STATE_ASLEEP:
        return "ASLEEP";
    case NM_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case NM_STATE_DISCONNECTING:
        return "DISCONNECTING";
    case NM_STATE_CONNECTING:
        return "CONNECTING";
    case NM_STATE_CONNECTED_LOCAL:
        return "CONNECTED_LOCAL";
    case NM_STATE_CONNECTED_SITE:
        return "CONNECTED_SITE";
    case NM_STATE_CONNECTED_GLOBAL:
        return "CONNECTED_GLOBAL";
    case NM_STATE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static NMState
find_best_device_state(NMManager *manager)
{
    NMManagerPrivate   *priv       = NM_MANAGER_GET_PRIVATE(manager);
    NMState             best_state = NM_STATE_DISCONNECTED;
    NMActiveConnection *ac;

    c_list_for_each_entry (ac, &priv->active_connections_lst_head, active_connections_lst) {
        NMActiveConnectionState ac_state;

        if (NM_IS_DEVICE_LOOPBACK(nm_active_connection_get_device(ac)))
            continue;
        ac_state = nm_active_connection_get_state(ac);
        switch (ac_state) {
        case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
            if (nm_active_connection_get_default(ac, AF_UNSPEC)) {
                if (priv->connectivity_state == NM_CONNECTIVITY_FULL)
                    return NM_STATE_CONNECTED_GLOBAL;

                best_state = NM_STATE_CONNECTED_SITE;
            } else {
                if (best_state < NM_STATE_CONNECTING)
                    best_state = NM_STATE_CONNECTED_LOCAL;
            }
            break;
        case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
            if (!NM_IN_SET(nm_active_connection_get_activation_type(ac),
                           NM_ACTIVATION_TYPE_EXTERNAL,
                           NM_ACTIVATION_TYPE_ASSUME)) {
                if (best_state != NM_STATE_CONNECTED_GLOBAL)
                    best_state = NM_STATE_CONNECTING;
            }
            break;
        case NM_ACTIVE_CONNECTION_STATE_DEACTIVATING:
            if (!NM_IN_SET(nm_active_connection_get_activation_type(ac),
                           NM_ACTIVATION_TYPE_EXTERNAL,
                           NM_ACTIVATION_TYPE_ASSUME)) {
                if (best_state < NM_STATE_DISCONNECTING)
                    best_state = NM_STATE_DISCONNECTING;
            }
            break;
        default:
            break;
        }
    }

    return best_state;
}

static void
nm_manager_update_metered(NMManager *self)
{
    NMManagerPrivate *priv;
    NMDevice         *device;
    NMMetered         value = NM_METERED_UNKNOWN;

    g_return_if_fail(NM_IS_MANAGER(self));
    priv = NM_MANAGER_GET_PRIVATE(self);

    if (priv->primary_connection) {
        device = nm_active_connection_get_device(priv->primary_connection);
        if (device)
            value = nm_device_get_metered(device);
    }

    if (value != priv->metered) {
        priv->metered = value;
        _LOGD(LOGD_CORE, "new metered value: %d", (int) priv->metered);
        _notify(self, PROP_METERED);
    }
}

NMMetered
nm_manager_get_metered(NMManager *self)
{
    g_return_val_if_fail(NM_IS_MANAGER(self), NM_METERED_UNKNOWN);

    return NM_MANAGER_GET_PRIVATE(self)->metered;
}

static void
nm_manager_update_state(NMManager *self)
{
    NMManagerPrivate *priv;
    NMState           new_state = NM_STATE_DISCONNECTED;

    g_return_if_fail(NM_IS_MANAGER(self));

    priv = NM_MANAGER_GET_PRIVATE(self);

    if (manager_sleeping(self))
        new_state = NM_STATE_ASLEEP;
    else
        new_state = find_best_device_state(self);

    if (new_state >= NM_STATE_CONNECTED_LOCAL && priv->connectivity_state == NM_CONNECTIVITY_FULL) {
        new_state = NM_STATE_CONNECTED_GLOBAL;
    }

    if (priv->state == new_state)
        return;

    priv->state = new_state;

    _LOGI(LOGD_CORE, "NetworkManager state is now %s", _nm_state_to_string(new_state));

    _notify(self, PROP_STATE);
    nm_dbus_object_emit_signal(NM_DBUS_OBJECT(self),
                               &interface_info_manager,
                               &signal_info_state_changed,
                               "(u)",
                               (guint32) priv->state);
}

static void
manager_device_state_changed(NMDevice           *device,
                             NMDeviceState       new_state,
                             NMDeviceState       old_state,
                             NMDeviceStateReason reason,
                             gpointer            user_data)
{
    NMManager        *self = NM_MANAGER(user_data);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    if (old_state == NM_DEVICE_STATE_UNMANAGED && new_state > NM_DEVICE_STATE_UNMANAGED)
        retry_connections_for_parent_device(self, device);

    if (NM_IN_SET(new_state,
                  NM_DEVICE_STATE_UNMANAGED,
                  NM_DEVICE_STATE_UNAVAILABLE,
                  NM_DEVICE_STATE_DISCONNECTED,
                  NM_DEVICE_STATE_PREPARE,
                  NM_DEVICE_STATE_FAILED))
        _notify(self, PROP_ACTIVE_CONNECTIONS);

    if (NM_IN_SET(new_state,
                  NM_DEVICE_STATE_UNMANAGED,
                  NM_DEVICE_STATE_DISCONNECTED,
                  NM_DEVICE_STATE_ACTIVATED)) {
        nm_manager_write_device_state(self, device, NULL);

        G_STATIC_ASSERT_EXPR(DEVICE_STATE_PRUNE_RATELIMIT_MAX < G_MAXUINT8);
        if (priv->device_state_prune_ratelimit_count++ > DEVICE_STATE_PRUNE_RATELIMIT_MAX) {
            /* We write the device state to /run. The state files are named after the
             * ifindex (which is assumed to be unique and not repeat -- in practice
             * it may repeat). So from time to time, we prune device state files
             * for interfaces that no longer exist.
             *
             * Otherwise, the files might pile up if you create (and destroy) a large
             * number of software devices. */
            priv->device_state_prune_ratelimit_count = 0;
            nm_config_device_state_prune_stale(NULL, priv->platform);
        }
    }

    if (NM_IN_SET(new_state, NM_DEVICE_STATE_UNAVAILABLE, NM_DEVICE_STATE_DISCONNECTED))
        nm_settings_device_added(priv->settings, device);
}

static void
_dns_mgr_update_pending_cb(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    check_if_startup_complete(self);
}

static void
check_if_startup_complete(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;
    const char       *reason;

    if (!priv->startup)
        return;

    if (!priv->devices_inited)
        return;

    if (nm_dns_manager_get_update_pending(nm_manager_get_dns_manager(self))) {
        if (priv->dns_mgr_update_pending_signal_id == 0) {
            priv->dns_mgr_update_pending_signal_id =
                g_signal_connect(nm_manager_get_dns_manager(self),
                                 "notify::" NM_DNS_MANAGER_UPDATE_PENDING,
                                 G_CALLBACK(_dns_mgr_update_pending_cb),
                                 self);
        }
        return;
    }

    nm_clear_g_signal_handler(nm_manager_get_dns_manager(self),
                              &priv->dns_mgr_update_pending_signal_id);

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        reason = nm_device_has_pending_action_reason(device);
        if (reason) {
            _LOGD(LOGD_CORE,
                  "startup complete is waiting for device '%s' (%s)",
                  nm_device_get_iface(device),
                  reason);
            return;
        }
    }

    /* All NMDevice must be ready. But also NMSettings tracks profiles that wait for
     * ready devices via "connection.wait-device-timeout".
     *
     * Note that we only re-check nm_settings_get_startup_complete_blocked_reason() when
     * all of the devices become ready (again).
     *
     * For example, assume we have device "eth1" and "profile-eth2" which waits for "eth2".
     * If "eth1" is ready (no pending action), we only need to re-evaluate "profile-eth2"
     * if we have another device ("eth2"), that becomes non-ready (had pending actions)
     * and again become ready. We don't need to check "profile-eth2" until "eth2" becomes
     * non-ready.
     * That is why nm_settings_get_startup_complete_blocked_reason() only has any significance
     * if all devices are ready too. It allows us to cut down the number of checks whether
     * NMSettings is ready. That's because we don't need to re-evaluate on minor changes of
     * a device, only when all devices become managed and ready. */

    g_signal_handlers_block_by_func(priv->settings, settings_startup_complete_changed, self);
    reason = nm_settings_get_startup_complete_blocked_reason(priv->settings, TRUE);
    g_signal_handlers_unblock_by_func(priv->settings, settings_startup_complete_changed, self);
    if (reason) {
        _LOGD(LOGD_CORE, "startup complete is waiting for connection (%s)", reason);
        return;
    }

    /* Most of our logging is not API/stable, but this line is kinda important and
     * what people will look for when debugging NetworkManager-wait-online.service.
     * Take care before rewording this message. */
    _LOGI(LOGD_CORE, "startup complete");

    priv->startup = FALSE;

    /* we no longer care about these signals. Startup-complete only
     * happens once. */
    g_signal_handlers_disconnect_by_func(priv->settings,
                                         G_CALLBACK(settings_startup_complete_changed),
                                         self);
    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        g_signal_handlers_disconnect_by_func(device,
                                             G_CALLBACK(device_has_pending_action_changed),
                                             self);
    }

    _notify(self, PROP_STARTUP);

    if (nm_config_get_configure_and_quit(priv->config))
        g_signal_emit(self, signals[CONFIGURE_QUIT], 0);
}

static void
device_has_pending_action_changed(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    check_if_startup_complete(self);
}

static void
settings_startup_complete_changed(NMSettings *settings, GParamSpec *pspec, NMManager *self)
{
    check_if_startup_complete(self);
}

static void
_parent_notify_changed(NMManager *self, NMDevice *device, gboolean device_removed)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *candidate;

    nm_assert(NM_IS_DEVICE(device));

again:
    c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
        if (nm_device_parent_notify_changed(candidate, device, device_removed)) {
            /* in the unlikely event that this changes anything, we start iterating
             * again, to be sure that the device list is up-to-date. */
            goto again;
        }
    }
}

static gboolean
device_is_wake_on_lan(NMPlatform *platform, NMDevice *device)
{
    int ifindex;

    ifindex = nm_device_get_ip_ifindex(device);
    if (ifindex <= 0)
        return FALSE;
    return nm_platform_link_get_wake_on_lan(platform, ifindex);
}

static void
remove_device(NMManager *self, NMDevice *device, gboolean quitting)
{
    NMManagerPrivate *priv     = NM_MANAGER_GET_PRIVATE(self);
    gboolean          unmanage = FALSE;
    NMRfkillType      rtype;

    _LOG2D(LOGD_DEVICE,
           device,
           "removing device (managed %d, wol %d)",
           nm_device_get_managed(device, FALSE),
           device_is_wake_on_lan(priv->platform, device));

    if (nm_device_get_managed(device, FALSE)) {
        if (quitting) {
            /* Leave configured if wo(w)lan and quitting */
            if (device_is_wake_on_lan(priv->platform, device))
                unmanage = FALSE;
            else
                unmanage = nm_device_unmanage_on_quit(device);
        } else {
            /* the device is already gone. Unmanage it. */
            unmanage = TRUE;
        }

        if (unmanage) {
            if (quitting)
                nm_device_set_unmanaged_by_quitting(device);
            else {
                nm_device_managed_type_set(device, NM_DEVICE_MANAGED_TYPE_REMOVED);
                nm_device_set_unmanaged_by_flags(device,
                                                 NM_UNMANAGED_PLATFORM_INIT,
                                                 NM_UNMAN_FLAG_OP_SET_UNMANAGED,
                                                 NM_DEVICE_STATE_REASON_REMOVED);
            }
        }
    }

    g_signal_handlers_disconnect_matched(device, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

    nm_settings_device_removed(priv->settings, device, quitting);

    _devcon_remove_device_all(self, device);

    c_list_unlink(&device->devices_lst);

    _parent_notify_changed(self, device, TRUE);

    rtype = nm_device_get_rfkill_type(device);
    if (rtype != NM_RFKILL_TYPE_UNKNOWN)
        _rfkill_update(self, rtype);

    if (nm_device_is_real(device)) {
        gboolean unconfigure_ip_config = !quitting || unmanage;

        /* When we don't unmanage the device on shutdown, we want to preserve the DNS
         * configuration in resolv.conf. For that, we must leak the configuration
         * in NMPolicy/NMDnsManager. We do that, by emitting the device-removed signal
         * with device's ip-config object still uncleared. In that case, NMPolicy
         * never learns to unconfigure the ip-config objects and does not remove them
         * from DNS on shutdown (which is ugly, because we don't cleanup the memory
         * properly).
         *
         * Control that by passing @unconfigure_ip_config.  */
        nm_device_removed(device, unconfigure_ip_config);

        _emit_device_added_removed(self, device, FALSE);
    } else {
        /* unrealize() does not release a port device from controller and
         * clear IP configurations, do it here */
        nm_device_removed(device, TRUE);
    }

    g_signal_emit(self, signals[INTERNAL_DEVICE_REMOVED], 0, device);
    _notify(self, PROP_ALL_DEVICES);

    update_connectivity_value(self);

    nm_dbus_object_clear_and_unexport(&device);

    check_if_startup_complete(self);
}

static void
device_removed_cb(NMDevice *device, gpointer user_data)
{
    remove_device(NM_MANAGER(user_data), device, FALSE);
}

NMState
nm_manager_get_state(NMManager *manager)
{
    g_return_val_if_fail(NM_IS_MANAGER(manager), NM_STATE_UNKNOWN);

    return NM_MANAGER_GET_PRIVATE(manager)->state;
}

/*****************************************************************************/

static NMDevice *
find_parent_device_for_connection(NMManager       *self,
                                  NMConnection    *connection,
                                  NMDeviceFactory *cached_factory,
                                  const char     **out_parent_spec)
{
    NMManagerPrivate     *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDeviceFactory      *factory;
    const char           *parent_name = NULL;
    NMSettingsConnection *parent_connection;
    NMDevice             *parent, *first_compatible = NULL;
    NMDevice             *candidate;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);
    NM_SET_OUT(out_parent_spec, NULL);

    if (!cached_factory) {
        factory = nm_device_factory_manager_find_factory_for_connection(connection);
        if (!factory)
            return NULL;
    } else
        factory = cached_factory;

    parent_name = nm_device_factory_get_connection_parent(factory, connection);
    if (!parent_name)
        return NULL;

    NM_SET_OUT(out_parent_spec, parent_name);

    /* Try as an interface name of a parent device */
    parent = find_device_by_iface(self, parent_name, NULL, NULL, connection);
    if (parent)
        return parent;

    /* Maybe a hardware address */
    parent = find_device_by_permanent_hw_addr(self, parent_name);
    if (parent)
        return parent;

    /* Maybe a connection UUID */
    parent_connection = nm_settings_get_connection_by_uuid(priv->settings, parent_name);
    if (!parent_connection)
        return NULL;

    /* Check if the parent connection is currently activated or is compatible
     * with some known device.
     */
    c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
        /* For a realized device, check that it's managed; otherwise it's not
         * compatible with any connection. If the device is unrealized then
         * the managed state is meaningless.
         */
        if (nm_device_is_real(candidate) && !nm_device_get_managed(candidate, FALSE))
            continue;

        if (nm_device_get_settings_connection(candidate) == parent_connection)
            return candidate;

        if (!first_compatible
            && nm_device_check_connection_compatible(
                candidate,
                nm_settings_connection_get_connection(parent_connection),
                TRUE,
                NULL))
            first_compatible = candidate;
    }

    return first_compatible;
}

/**
 * nm_manager_get_connection_iface:
 * @self: the #NMManager
 * @connection: the #NMConnection to get the interface for
 * @out_parent: on success, the parent device if any
 * @out_parent_spec: on return, a string specifying the parent device
 *   in the connection. This can be a device name, a MAC address or a
 *   connection UUID.
 * @error: an error if determining the virtual interface name failed
 *
 * Given @connection, returns the interface name that the connection
 * would need to use when activated. %NULL is returned if the name
 * is not specified in connection or a the name for a virtual device
 * could not be generated.
 *
 * Returns: the expected interface name (caller takes ownership), or %NULL
 */
char *
nm_manager_get_connection_iface(NMManager    *self,
                                NMConnection *connection,
                                NMDevice    **out_parent,
                                const char  **out_parent_spec,
                                GError      **error)
{
    NMDeviceFactory *factory;
    char            *iface  = NULL;
    NMDevice        *parent = NULL;

    NM_SET_OUT(out_parent, NULL);
    NM_SET_OUT(out_parent_spec, NULL);

    factory = nm_device_factory_manager_find_factory_for_connection(connection);
    if (!factory) {
        if (nm_streq0(nm_connection_get_connection_type(connection),
                      NM_SETTING_GENERIC_SETTING_NAME)) {
            /* the generic type doesn't have a factory. */
            goto return_ifname_fom_connection;
        }

        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_MISSING_PLUGIN,
                    "NetworkManager plugin for '%s' unavailable",
                    nm_connection_get_connection_type(connection));
        return NULL;
    }

    if (!out_parent && !NM_DEVICE_FACTORY_GET_CLASS(factory)->get_connection_iface) {
        /* optimization. Shortcut lookup of the partent device. */
        goto return_ifname_fom_connection;
    }

    parent = find_parent_device_for_connection(self, connection, factory, out_parent_spec);
    iface  = nm_device_factory_get_connection_iface(factory,
                                                   connection,
                                                   parent ? nm_device_get_ip_iface(parent) : NULL,
                                                   error);
    if (!iface)
        return NULL;

    if (out_parent)
        *out_parent = parent;
    return iface;

return_ifname_fom_connection:
    iface = g_strdup(nm_connection_get_interface_name(connection));
    if (!iface) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "failed to determine interface name for a %s",
                    nm_connection_get_connection_type(connection));
    }
    return iface;
}

/**
 * nm_manager_iface_for_uuid:
 * @self: the #NMManager
 * @uuid: the connection uuid
 *
 * Gets a link name for the given UUID. Useful for the settings plugins that
 * wish to write configuration files compatible with tooling that can't
 * interpret our UUIDs.
 *
 * Returns: An interface name; %NULL if none matches
 */
const char *
nm_manager_iface_for_uuid(NMManager *self, const char *uuid)
{
    NMManagerPrivate     *priv = NM_MANAGER_GET_PRIVATE(self);
    NMSettingsConnection *sett_conn;

    sett_conn = nm_settings_get_connection_by_uuid(priv->settings, uuid);
    if (!sett_conn)
        return NULL;

    return nm_connection_get_interface_name(nm_settings_connection_get_connection(sett_conn));
}

NMDevice *
nm_manager_get_device(NMManager *self, const char *ifname, NMDeviceType device_type)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    g_return_val_if_fail(ifname, NULL);
    g_return_val_if_fail(device_type != NM_DEVICE_TYPE_UNKNOWN, NULL);

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        if (nm_device_get_device_type(device) == device_type
            && nm_streq0(nm_device_get_iface(device), ifname))
            return device;
    }

    return NULL;
}

gboolean
nm_manager_remove_device(NMManager *self, const char *ifname, NMDeviceType device_type)
{
    NMDevice *d;

    d = nm_manager_get_device(self, ifname, device_type);
    if (!d)
        return FALSE;

    remove_device(self, d, FALSE);
    return TRUE;
}

/**
 * system_create_virtual_device:
 * @self: the #NMManager
 * @connection: the connection which might require a virtual device
 *
 * If @connection requires a virtual device and one does not yet exist for it,
 * creates that device.
 *
 * Returns: A #NMDevice that was just realized; %NULL if none
 */
static NMDevice *
system_create_virtual_device(NMManager *self, NMConnection *connection, GError **error)
{
    NMManagerPrivate            *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDeviceFactory             *factory;
    NMSettingsConnection *const *connections;
    guint                        i;
    gs_free char                *iface = NULL;
    const char                  *parent_spec;
    NMDevice                    *device = NULL;
    NMDevice                    *parent = NULL;
    NMDevice                    *dev_candidate;

    g_return_val_if_fail(NM_IS_MANAGER(self), NULL);
    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    iface = nm_manager_get_connection_iface(self, connection, &parent, &parent_spec, error);
    if (!iface)
        return NULL;

    if (parent_spec && !parent) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                    "Parent device for '%s' not available",
                    iface);
        return NULL;
    }

    /* See if there's a device that is already compatible with this connection */
    c_list_for_each_entry (dev_candidate, &priv->devices_lst_head, devices_lst) {
        if (nm_device_check_connection_compatible(dev_candidate, connection, FALSE, NULL)) {
            if (nm_device_is_real(dev_candidate)) {
                g_set_error(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                            "Device named '%s' already exists",
                            iface);
                return NULL;
            }

            device = dev_candidate;
            break;
        }
    }

    if (!device) {
        /* No matching device found. Proceed creating a new one. */

        factory = nm_device_factory_manager_find_factory_for_connection(connection);
        if (!factory) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                        "'%s' plugin for '%s' unavailable",
                        nm_connection_get_connection_type(connection),
                        iface);
            return NULL;
        }

        device = nm_device_factory_create_device(factory, iface, NULL, connection, NULL, error);
        if (!device)
            return NULL;

        _LOG3D(LOGD_DEVICE, connection, "create virtual device %s", nm_device_get_iface(device));

        if (!add_device(self, device, error)) {
            g_object_unref(device);
            return NULL;
        }

        /* Add device takes a reference that NMManager still owns, so it's
         * safe to unref here and still return @device.
         */
        g_object_unref(device);
    }

    if (!nm_device_check_unrealized_device_managed(device)) {
        _LOG3D(LOGD_DEVICE,
               connection,
               "skip activation because virtual device '%s' is unmanaged",
               nm_device_get_iface(device));
        return device;
    }

    if (!find_controller(self, connection, device, NULL, NULL, NULL, error))
        return device;

    /* Create backing resources if the device has any autoconnect connections */
    connections = nm_settings_get_connections_sorted_by_autoconnect_priority(priv->settings, NULL);
    for (i = 0; connections[i]; i++) {
        NMConnection        *candidate = nm_settings_connection_get_connection(connections[i]);
        NMSettingConnection *s_con;

        if (!nm_device_check_connection_compatible(device, candidate, TRUE, NULL))
            continue;

        s_con = nm_connection_get_setting_connection(candidate);
        if (!nm_setting_connection_get_autoconnect(s_con)
            || nm_settings_connection_autoconnect_is_blocked(connections[i]))
            continue;

        /* Create any backing resources the device needs */
        if (!nm_device_create_and_realize(device, connection, parent, error))
            return NULL;

        retry_connections_for_parent_device(self, device);
        break;
    }

    return device;
}

static void
retry_connections_for_parent_device(NMManager *self, NMDevice *device)
{
    NMManagerPrivate            *priv = NM_MANAGER_GET_PRIVATE(self);
    NMSettingsConnection *const *connections;
    guint                        i;

    g_return_if_fail(device);

    connections = nm_settings_get_connections_sorted_by_autoconnect_priority(priv->settings, NULL);
    for (i = 0; connections[i]; i++) {
        NMSettingsConnection *sett_conn  = connections[i];
        NMConnection         *connection = nm_settings_connection_get_connection(sett_conn);
        gs_free_error GError *error      = NULL;
        gs_free char         *ifname     = NULL;
        NMDevice             *parent;

        parent = find_parent_device_for_connection(self, connection, NULL, NULL);
        if (parent == device) {
            /* Only try to activate devices that don't already exist */
            ifname = nm_manager_get_connection_iface(self, connection, &parent, NULL, &error);
            if (ifname) {
                if (!nm_platform_link_get_by_ifname(NM_PLATFORM_GET, ifname))
                    connection_changed(self, sett_conn);
            }
        }
    }
}

static void
connection_changed(NMManager *self, NMSettingsConnection *sett_conn)
{
    NMConnection         *connection;
    NMDevice             *device;
    gs_free_error GError *error = NULL;

    if (NM_FLAGS_ANY(nm_settings_connection_get_flags(sett_conn),
                     NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                         | NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL))
        return;

    connection = nm_settings_connection_get_connection(sett_conn);

    if (!nm_connection_is_virtual(connection))
        return;

    device = system_create_virtual_device(self, connection, &error);
    if (!device) {
        _LOG3D(LOGD_DEVICE, connection, "Can't create a virtual device: %s", error->message);
        return;
    }

    /* Maybe the device that was created was needed by some other
     * connection's device (parent of a VLAN). Let the connections
     * can use the newly created device as a parent know. */
    retry_connections_for_parent_device(self, device);
}

static void
connection_added_cb(NMSettings *settings, NMSettingsConnection *sett_conn, NMManager *self)
{
    connection_changed(self, sett_conn);
}

static void
connection_updated_cb(NMSettings           *settings,
                      NMSettingsConnection *sett_conn,
                      guint                 update_reason_u,
                      NMManager            *self)
{
    connection_changed(self, sett_conn);
}

static void
connections_changed(NMManager *self)
{
    NMManagerPrivate            *priv = NM_MANAGER_GET_PRIVATE(self);
    NMSettingsConnection *const *connections;
    guint                        i;

    connections = nm_settings_get_connections_sorted_by_autoconnect_priority(priv->settings, NULL);
    for (i = 0; connections[i]; i++)
        connection_changed(self, connections[i]);
}

/*****************************************************************************/

static void
_delete_volatile_connection_all(NMManager *self, gboolean do_delete)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMCListElem      *elem;

    while (
        (elem = c_list_first_entry(&priv->delete_volatile_connection_lst_head, NMCListElem, lst))) {
        gs_unref_object NMSettingsConnection *connection = NULL;

        connection = nm_c_list_elem_free_steal(elem);
        if (do_delete)
            _delete_volatile_connection_do(self, connection);
    }
}

static gboolean
_delete_volatile_connection_cb(gpointer user_data)
{
    NMManager        *self = user_data;
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    priv->delete_volatile_connection_idle_id = 0;
    _delete_volatile_connection_all(self, TRUE);
    return G_SOURCE_REMOVE;
}

static void
connection_flags_changed(NMSettings *settings, NMSettingsConnection *connection, gpointer user_data)
{
    NMManager        *self = user_data;
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    if (!NM_FLAGS_ANY(nm_settings_connection_get_flags(connection),
                      NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                          | NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL))
        return;

    if (active_connection_find(self,
                               connection,
                               NULL,
                               NM_ACTIVE_CONNECTION_STATE_DEACTIVATED,
                               FALSE,
                               NULL)) {
        /* the connection still has an active-connection. It will be purged
         * when the active connection(s) get(s) removed. */
        return;
    }

    c_list_link_tail(&priv->delete_volatile_connection_lst_head,
                     &nm_c_list_elem_new_stale(g_object_ref(connection))->lst);
    if (!priv->delete_volatile_connection_idle_id)
        priv->delete_volatile_connection_idle_id = g_idle_add(_delete_volatile_connection_cb, self);
}

/*****************************************************************************/

static void
system_unmanaged_devices_changed_cb(NMSettings *settings, GParamSpec *pspec, gpointer user_data)
{
    NMManager        *self = NM_MANAGER(user_data);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst)
        nm_device_set_unmanaged_by_user_settings(device, TRUE);
}

static void
_static_hostname_changed_cb(NMHostnameManager *hostname_manager, GParamSpec *pspec, NMManager *self)
{
    nm_dispatcher_call_hostname(NULL, NULL, NULL);
}

/*****************************************************************************/

static const RfkillTypeDesc _rfkill_type_desc[NM_RFKILL_TYPE_MAX] = {
    [NM_RFKILL_TYPE_WLAN] =
        {
            .prop_id    = PROP_WIRELESS_ENABLED,
            .hw_prop_id = PROP_WIRELESS_HARDWARE_ENABLED,
            .key        = NM_CONFIG_STATE_PROPERTY_WIFI_ENABLED,
        },
    [NM_RFKILL_TYPE_WWAN] =
        {
            .prop_id    = PROP_WWAN_ENABLED,
            .hw_prop_id = PROP_WWAN_HARDWARE_ENABLED,
            .key        = NM_CONFIG_STATE_PROPERTY_WWAN_ENABLED,
        },
};

static gboolean
_rfkill_radio_state_get_enabled(const RfkillRadioState *rstate, gboolean check_changeable)
{
    gboolean enabled;

    enabled = rstate->user_enabled && rstate->hw_enabled;
    if (check_changeable)
        enabled &= rstate->sw_enabled;
    return enabled;
}

static void
_rfkill_radio_state_set_from_manager(NMManager *self, NMRfkillType rtype, RfkillRadioState *rstate)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    switch (nm_rfkill_manager_get_rfkill_state(priv->rfkill_mgr, rtype)) {
    case NM_RFKILL_STATE_UNAVAILABLE:
        rstate->sw_enabled = TRUE;
        rstate->hw_enabled = TRUE;

        /* A rfkill-type is available when there is a compatible
         * killswitch or a compatible device. */
        c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
            if (nm_device_get_rfkill_type(device) == rtype) {
                rstate->available = TRUE;
                return;
            }
        }
        rstate->available = FALSE;
        return;
    case NM_RFKILL_STATE_UNBLOCKED:
        rstate->available  = TRUE;
        rstate->sw_enabled = TRUE;
        rstate->hw_enabled = TRUE;
        return;
    case NM_RFKILL_STATE_SOFT_BLOCKED:
        rstate->available  = TRUE;
        rstate->sw_enabled = FALSE;
        rstate->hw_enabled = TRUE;
        return;
    case NM_RFKILL_STATE_HARD_BLOCKED:
        rstate->available  = TRUE;
        rstate->sw_enabled = FALSE;
        rstate->hw_enabled = FALSE;
        return;
    }
    nm_assert_not_reached();
}

static gboolean
_rfkill_radio_state_get(NMManager *self, NMRfkillType rtype)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    nm_assert(_NM_INT_NOT_NEGATIVE(rtype) && rtype < G_N_ELEMENTS(priv->radio_states));

    return _rfkill_radio_state_get_enabled(&priv->radio_states[rtype], TRUE);
}

static void
_rfkill_update_devices(NMManager *self, NMRfkillType rtype, gboolean enabled)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    _notify(self, _rfkill_type_desc[rtype].prop_id);

    /* Don't touch devices if asleep/networking disabled */
    if (manager_sleeping(self))
        return;

    /* enable/disable wireless devices as required */
    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        if (nm_device_get_rfkill_type(device) == rtype) {
            _LOG2D(LOGD_RFKILL,
                   device,
                   "rfkill: setting radio %s",
                   enabled ? "enabled" : "disabled");
            nm_device_set_enabled(device, enabled);
        }
    }
}

static void
_rfkill_update_one_type(NMManager *self, NMRfkillType rtype)
{
    NMManagerPrivate *priv   = NM_MANAGER_GET_PRIVATE(self);
    RfkillRadioState *rstate = &priv->radio_states[rtype];
    gboolean          old_enabled;
    gboolean          new_enabled;
    gboolean          old_rfkilled;
    gboolean          new_rfkilled;
    gboolean          old_hwe;
    guint             old_radio_flags;

    nm_assert(_NM_INT_NOT_NEGATIVE(rtype) && rtype < G_N_ELEMENTS(priv->radio_states));

    old_enabled     = _rfkill_radio_state_get_enabled(rstate, TRUE);
    old_rfkilled    = rstate->hw_enabled && rstate->sw_enabled;
    old_hwe         = rstate->hw_enabled;
    old_radio_flags = priv->radio_flags;

    /* recheck kernel rfkill state */
    _rfkill_radio_state_set_from_manager(self, rtype, rstate);

    /* Print out all states affecting device enablement */
    _LOGD(LOGD_RFKILL,
          "rfkill: %s available %d hw-enabled %d sw-enabled %d",
          nm_rfkill_type_to_string(rtype),
          rstate->available,
          rstate->hw_enabled,
          rstate->sw_enabled);

    /* Log new killswitch state */
    new_rfkilled = rstate->hw_enabled && rstate->sw_enabled;
    if (old_rfkilled != new_rfkilled) {
        _LOGI(LOGD_RFKILL,
              "rfkill: %s now %s by radio killswitch",
              nm_rfkill_type_to_string(rtype),
              new_rfkilled ? "enabled" : "disabled");
    }

    priv->radio_flags = NM_FLAGS_ASSIGN(priv->radio_flags,
                                        (guint) nm_rfkill_type_to_radio_available_flag(rtype),
                                        rstate->available);

    /* Send out property changed signal for HW available and enabled */
    nm_gobject_notify_together(self,
                               rstate->hw_enabled != old_hwe ? _rfkill_type_desc[rtype].hw_prop_id
                                                             : PROP_0,
                               priv->radio_flags != old_radio_flags ? PROP_RADIO_FLAGS : PROP_0);

    /* And finally update the actual device radio state itself; respect the
     * daemon state here because this is never called from user-triggered
     * radio changes and we only want to ignore the daemon enabled state when
     * handling user radio change requests.
     */
    new_enabled = _rfkill_radio_state_get_enabled(rstate, TRUE);
    if (new_enabled != old_enabled)
        _rfkill_update_devices(self, rtype, new_enabled);
}

static void
_rfkill_update(NMManager *self, NMRfkillType rtype)
{
    guint i;

    if (rtype != NM_RFKILL_TYPE_UNKNOWN)
        _rfkill_update_one_type(self, rtype);
    else {
        /* Otherwise, sync all radio types */
        for (i = 0; i < NM_RFKILL_TYPE_MAX; i++)
            _rfkill_update_one_type(self, i);
    }
}

/*****************************************************************************/

#define KERN_RFKILL_OP_CHANGE_ALL 3
#define KERN_RFKILL_TYPE_WLAN     1
#define KERN_RFKILL_TYPE_WWAN     5

struct rfkill_event {
    uint32_t idx;
    uint8_t  type;
    uint8_t  op;
    uint8_t  soft;
    uint8_t  hard;
} _nm_packed;

static void
_rfkill_update_system(NMManager *self, NMRfkillType rtype, gboolean enabled)
{
    nm_auto_close int   fd = -1;
    struct rfkill_event event;
    ssize_t             len;
    int                 errsv;

    nm_assert(NM_IN_SET(rtype, NM_RFKILL_TYPE_WLAN, NM_RFKILL_TYPE_WWAN));

    fd = open("/dev/rfkill", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES)
            _LOGW(LOGD_RFKILL,
                  "rfkill: (%s): failed to open killswitch device",
                  nm_rfkill_type_to_string(rtype));
        return;
    }

    memset(&event, 0, sizeof(event));
    event.op = KERN_RFKILL_OP_CHANGE_ALL;
    switch (rtype) {
    case NM_RFKILL_TYPE_WLAN:
        event.type = KERN_RFKILL_TYPE_WLAN;
        break;
    case NM_RFKILL_TYPE_WWAN:
        event.type = KERN_RFKILL_TYPE_WWAN;
        break;
    default:
        nm_assert_not_reached();
    }
    event.soft = enabled ? 0 : 1;

    len = write(fd, &event, sizeof(event));
    if (len < 0) {
        errsv = errno;
        _LOGW(LOGD_RFKILL,
              "rfkill: (%s): failed to change Wi-Fi killswitch state: (%d) %s",
              nm_rfkill_type_to_string(rtype),
              errsv,
              nm_strerror_native(errsv));
    } else if (len == sizeof(event)) {
        _LOGI(LOGD_RFKILL,
              "rfkill: %s hardware radio set %s",
              nm_rfkill_type_to_string(rtype),
              enabled ? "enabled" : "disabled");
    } else {
        /* Failed to write full structure */
        _LOGW(LOGD_RFKILL,
              "rfkill: (%s): failed to change Wi-Fi killswitch state",
              nm_rfkill_type_to_string(rtype));
    }
}

static void
_rfkill_update_from_user(NMManager *self, NMRfkillType rtype, gboolean enabled)
{
    NMManagerPrivate *priv   = NM_MANAGER_GET_PRIVATE(self);
    RfkillRadioState *rstate = &priv->radio_states[rtype];
    gboolean          old_enabled, new_enabled;

    /* Don't touch devices if asleep/networking disabled */
    if (manager_sleeping(self))
        return;

    _LOGD(LOGD_RFKILL,
          "rfkill: (%s): setting radio %s by user",
          nm_rfkill_type_to_string(rtype),
          enabled ? "enabled" : "disabled");

    /* Update enabled key in state file */
    nm_config_state_set(priv->config, TRUE, FALSE, _rfkill_type_desc[rtype].key, enabled);

    /* When the user toggles the radio, their request should override any
     * daemon (like ModemManager) enabled state that can be changed.  For WWAN
     * for example, we want the WwanEnabled property to reflect the daemon state
     * too so that users can toggle the modem powered, but we don't want that
     * daemon state to affect whether or not the user *can* turn it on, which is
     * what the kernel rfkill state does.  So we ignore daemon enabled state
     * when determining what the new state should be since it shouldn't block
     * the user's request.
     */
    old_enabled          = _rfkill_radio_state_get_enabled(rstate, TRUE);
    rstate->user_enabled = enabled;
    new_enabled          = _rfkill_radio_state_get_enabled(rstate, FALSE);
    if (new_enabled != old_enabled) {
        /* Try to change the kernel rfkill state */
        _rfkill_update_system(self, rtype, new_enabled);
        _rfkill_update_devices(self, rtype, new_enabled);
    }
}

/*****************************************************************************/

void
nm_manager_device_recheck_auto_activate_schedule(NMManager *self, NMDevice *device)
{
    g_return_if_fail(NM_IS_MANAGER(self));

    nm_policy_device_recheck_auto_activate_schedule(NM_MANAGER_GET_PRIVATE(self)->policy, device);
}

/*****************************************************************************/

static void
device_auth_done_cb(NMAuthChain *chain, GDBusMethodInvocation *context, gpointer user_data)
{
    NMManager                     *self  = NM_MANAGER(user_data);
    gs_free_error GError          *error = NULL;
    NMAuthCallResult               result;
    NMDevice                      *device;
    GCancellable                  *cancellable;
    const char                    *permission;
    NMManagerDeviceAuthRequestFunc callback;
    NMAuthSubject                 *subject;

    nm_assert(G_IS_DBUS_METHOD_INVOCATION(context));

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));

    permission = nm_auth_chain_get_data(chain, "perm");
    nm_assert(permission);
    callback = nm_auth_chain_get_data(chain, "callback");
    nm_assert(callback);
    device = nm_auth_chain_get_data(chain, "device");
    nm_assert(NM_IS_DEVICE(device));

    cancellable = nm_auth_chain_get_cancellable(chain);
    nm_assert(!cancellable || G_IS_CANCELLABLE(cancellable));

    result  = nm_auth_chain_get_result(chain, permission);
    subject = nm_auth_chain_get_subject(chain);

    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        /* pass. */
    } else {
        if (result != NM_AUTH_CALL_RESULT_YES) {
            _LOGD(LOGD_CORE, "%s request failed: not authorized", permission);
            error = g_error_new(NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_PERMISSION_DENIED,
                                "%s request failed: not authorized",
                                permission);
        }

        nm_assert(error || (result == NM_AUTH_CALL_RESULT_YES));
    }

    if (!error && !nm_dbus_object_is_exported(NM_DBUS_OBJECT(device))) {
        g_set_error(&error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                    "device no longer exists");
    }

    callback(device, context, subject, error, nm_auth_chain_get_data(chain, "user-data"));
}

static void
_device_auth_done_fail_on_idle(gpointer user_data, GCancellable *cancellable)
{
    gs_unref_object NMManager             *self            = NULL;
    gs_unref_object NMDevice              *device          = NULL;
    gs_unref_object GDBusMethodInvocation *context         = NULL;
    gs_unref_object NMAuthSubject         *subject         = NULL;
    gs_free_error GError                  *error_original  = NULL;
    gs_free_error GError                  *error_cancelled = NULL;
    NMManagerDeviceAuthRequestFunc         callback;
    gpointer                               callback_user_data;

    nm_utils_user_data_unpack(user_data,
                              &self,
                              &device,
                              &context,
                              &subject,
                              &error_original,
                              &callback,
                              &callback_user_data);

    g_cancellable_set_error_if_cancelled(cancellable, &error_cancelled);

    callback(device, context, subject, error_cancelled ?: error_original, callback_user_data);
}

void
nm_manager_device_auth_request(NMManager                     *self,
                               NMDevice                      *device,
                               GDBusMethodInvocation         *context,
                               NMConnection                  *connection,
                               const char                    *permission,
                               gboolean                       allow_interaction,
                               GCancellable                  *cancellable,
                               NMManagerDeviceAuthRequestFunc callback,
                               gpointer                       user_data)
{
    NMManagerPrivate              *priv    = NM_MANAGER_GET_PRIVATE(self);
    gs_free_error GError          *error   = NULL;
    gs_unref_object NMAuthSubject *subject = NULL;
    NMAuthChain                   *chain;
    char                          *permission_dup;

    /* Validate the caller */
    subject = nm_dbus_manager_new_auth_subject_from_context(context);
    if (!subject) {
        g_set_error_literal(&error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_PERMISSION_DENIED,
                            NM_UTILS_ERROR_MSG_REQ_UID_UKNOWN);
        goto fail_on_idle;
    }

    /* Ensure the subject has permissions for this connection */
    if (connection
        && !nm_auth_is_subject_in_acl_set_error(connection,
                                                subject,
                                                NM_MANAGER_ERROR,
                                                NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                &error))
        goto fail_on_idle;

    if (!nm_dbus_object_is_exported(NM_DBUS_OBJECT(device))) {
        g_set_error(&error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                    "device no longer exists");
        goto fail_on_idle;
    }

    chain = nm_auth_chain_new_subject(subject, context, device_auth_done_cb, self);
    if (cancellable)
        nm_auth_chain_set_cancellable(chain, cancellable);

    permission_dup = g_strdup(permission);

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "device", g_object_ref(device), g_object_unref);
    nm_auth_chain_set_data(chain, "callback", callback, NULL);
    nm_auth_chain_set_data(chain, "user-data", user_data, NULL);
    nm_auth_chain_set_data(chain, "perm", permission_dup /* transfer ownership */, g_free);
    nm_auth_chain_add_call_unsafe(chain, permission_dup, allow_interaction);
    return;

fail_on_idle:
    nm_utils_invoke_on_idle(cancellable,
                            _device_auth_done_fail_on_idle,
                            nm_utils_user_data_pack(g_object_ref(self),
                                                    g_object_ref(device),
                                                    g_object_ref(context),
                                                    g_steal_pointer(&subject),
                                                    g_steal_pointer(&error),
                                                    callback,
                                                    user_data));
}

static gboolean
new_activation_allowed_for_connection(NMManager *self, NMSettingsConnection *connection)
{
    if (NM_IN_SET(
            _nm_connection_get_multi_connect(nm_settings_connection_get_connection(connection)),
            NM_CONNECTION_MULTI_CONNECT_MANUAL_MULTIPLE,
            NM_CONNECTION_MULTI_CONNECT_MULTIPLE))
        return TRUE;

    return !active_connection_find(self,
                                   connection,
                                   NULL,
                                   NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
                                   FALSE,
                                   NULL);
}

/**
 * get_existing_connection:
 * @manager: #NMManager instance
 * @device: #NMDevice instance
 * @out_generated: (out) (optional): return TRUE, if the connection was generated.
 *
 * Returns: a #NMSettingsConnection to be assumed by the device, or %NULL if
 *   the device does not support assuming existing connections.
 */
static NMSettingsConnection *
get_existing_connection(NMManager *self, NMDevice *device, gboolean *out_generated)
{
    NMManagerPrivate             *priv       = NM_MANAGER_GET_PRIVATE(self);
    gs_unref_object NMConnection *connection = NULL;
    NMSettingsConnection         *added;
    GError                       *error                        = NULL;
    gs_free_error GError         *gen_error                    = NULL;
    NMDevice                     *controller                   = NULL;
    int                           ifindex                      = nm_device_get_ifindex(device);
    NMSettingsConnection         *matched                      = NULL;
    NMSettingsConnection         *connection_checked           = NULL;
    gboolean                      assume_state_guess_assume    = FALSE;
    const char                   *assume_state_connection_uuid = NULL;
    gboolean                      maybe_later, only_by_uuid = FALSE;

    if (out_generated)
        *out_generated = FALSE;

    if (!nm_device_can_assume_connections(device)) {
        nm_device_assume_state_reset(device);
        _LOG2D(LOGD_DEVICE, device, "assume: device cannot assume connection");
        return NULL;
    }

    if (ifindex) {
        int controller_ifindex = nm_platform_link_get_controller(priv->platform, ifindex);

        /* Check that the controller is activating before assuming a
         * port connection. However, ignore ovs-system/ovs-netdev controller as
         * we never manage it.
         */
        if (controller_ifindex
            && nm_platform_link_get_type(priv->platform, controller_ifindex)
                   != NM_LINK_TYPE_OPENVSWITCH) {
            controller = nm_manager_get_device_by_ifindex(self, controller_ifindex);
            if (!controller) {
                _LOG2D(LOGD_DEVICE,
                       device,
                       "assume: don't assume because "
                       "cannot generate connection for port before its controller (%s/%d)",
                       nm_platform_link_get_name(priv->platform, controller_ifindex),
                       controller_ifindex);
                return NULL;
            }
            if (!nm_device_get_act_request(controller)) {
                _LOG2D(LOGD_DEVICE,
                       device,
                       "assume: don't assume because "
                       "cannot generate connection for port before controller %s activates",
                       nm_device_get_iface(controller));
                return NULL;
            }
        }
    }

    if (nm_config_data_get_device_config_boolean_by_device(
            NM_CONFIG_GET_DATA,
            NM_CONFIG_KEYFILE_KEY_DEVICE_KEEP_CONFIGURATION,
            device,
            TRUE,
            TRUE)) {
        /* The core of the API is nm_device_generate_connection() function, based on
         * update_connection() virtual method and the @connection_type_supported
         * class attribute. Devices that support assuming existing connections must
         * have update_connection() implemented, otherwise
         * nm_device_generate_connection() returns NULL. */
        connection = nm_device_generate_connection(device, controller, &maybe_later, &gen_error);
        if (!connection) {
            if (maybe_later) {
                /* The device can potentially assume connections, but at this
                 * time we can't generate a connection because no address is
                 * configured. Allow the device to assume a connection indicated
                 * in the state file by UUID. */
                only_by_uuid = TRUE;
            } else {
                nm_device_assume_state_reset(device);
                _LOG2D(LOGD_DEVICE,
                       device,
                       "assume: cannot generate connection: %s",
                       gen_error->message);
                return NULL;
            }
        }
    } else {
        connection   = NULL;
        only_by_uuid = TRUE;
        g_set_error(&gen_error,
                    NM_DEVICE_ERROR,
                    NM_DEVICE_ERROR_FAILED,
                    "device %s has 'keep-configuration=no'",
                    nm_device_get_iface(device));
    }

    nm_device_assume_state_get(device, &assume_state_guess_assume, &assume_state_connection_uuid);

    /* If the device state file indicates a connection that was active before NM
     * restarted, perform basic sanity checks on it. */
    if (assume_state_connection_uuid
        && (connection_checked =
                nm_settings_get_connection_by_uuid(priv->settings, assume_state_connection_uuid))
        && new_activation_allowed_for_connection(self, connection_checked)
        && nm_device_check_connection_compatible(
            device,
            nm_settings_connection_get_connection(connection_checked),
            TRUE,
            NULL)) {
        if (connection) {
            NMConnection *con = nm_settings_connection_get_connection(connection_checked);

            if (nm_utils_match_connection((NMConnection *[]) {con, NULL},
                                          connection,
                                          TRUE,
                                          nm_device_has_carrier(device),
                                          nm_device_get_route_metric(device, AF_INET),
                                          nm_device_get_route_metric(device, AF_INET6),
                                          NULL,
                                          NULL))
                matched = connection_checked;
        } else
            matched = connection_checked;
    }

    if (!matched && only_by_uuid) {
        _LOG2D(LOGD_DEVICE, device, "assume: cannot generate connection: %s", gen_error->message);
        return NULL;
    }

    if (!matched && assume_state_guess_assume) {
        gs_free NMSettingsConnection **sett_conns = NULL;
        guint                          len, i, j;

        /* @assume_state_guess_assume=TRUE means this is the first start of NM
         * and the state file contains no UUID. Search persistent connections
         * for a matching candidate. */
        sett_conns = nm_manager_get_activatable_connections(self, FALSE, FALSE, &len);
        if (len > 0) {
            for (i = 0, j = 0; i < len; i++) {
                NMSettingsConnection *sett_conn = sett_conns[i];

                if (sett_conn != connection_checked
                    && nm_device_check_connection_compatible(
                        device,
                        nm_settings_connection_get_connection(sett_conn),
                        TRUE,
                        NULL))
                    sett_conns[j++] = sett_conn;
            }
            sett_conns[j] = NULL;
            len           = j;
            if (len > 0) {
                gs_free NMConnection **conns = NULL;
                NMConnection          *con;

                g_qsort_with_data(sett_conns,
                                  len,
                                  sizeof(sett_conns[0]),
                                  nm_settings_connection_cmp_timestamp_p_with_data,
                                  NULL);

                conns = nm_settings_connections_array_to_connections(sett_conns, len);

                con = nm_utils_match_connection(conns,
                                                connection,
                                                FALSE,
                                                nm_device_has_carrier(device),
                                                nm_device_get_route_metric(device, AF_INET),
                                                nm_device_get_route_metric(device, AF_INET6),
                                                NULL,
                                                NULL);
                if (con) {
                    for (i = 0; i < len; i++) {
                        if (conns[i] == con) {
                            matched = sett_conns[i];
                            break;
                        }
                    }
                    nm_assert(matched);
                }
            }
        }
    }

    if (matched) {
        if (NM_FLAGS_HAS(nm_settings_connection_get_flags(matched),
                         NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL)) {
            _LOG2D(LOGD_DEVICE,
                   device,
                   "assume: take over previous connection '%s' (%s)",
                   nm_settings_connection_get_id(matched),
                   nm_settings_connection_get_uuid(matched));
            NM_SET_OUT(out_generated, TRUE);
        } else {
            _LOG2I(LOGD_DEVICE,
                   device,
                   "assume: will attempt to assume matching connection '%s' (%s)%s",
                   nm_settings_connection_get_id(matched),
                   nm_settings_connection_get_uuid(matched),
                   assume_state_connection_uuid
                           && nm_streq(assume_state_connection_uuid,
                                       nm_settings_connection_get_uuid(matched))
                       ? " (indicated)"
                       : " (guessed)");
        }
        nm_device_assume_state_reset(device);
        return matched;
    }

    /* When no configured connection matches the generated connection, we keep
     * the generated connection instead. */
    _LOG2D(LOGD_DEVICE,
           device,
           "assume: generated connection '%s' (%s)",
           nm_connection_get_id(connection),
           nm_connection_get_uuid(connection));

    nm_device_assume_state_reset(device);

    if (!nm_settings_add_connection(priv->settings,
                                    NULL,
                                    connection,
                                    NM_SETTINGS_CONNECTION_PERSIST_MODE_IN_MEMORY_ONLY,
                                    NM_SETTINGS_CONNECTION_ADD_REASON_NONE,
                                    NM_SETTINGS_CONNECTION_INT_FLAGS_NM_GENERATED
                                        | NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                                        | NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL,
                                    &added,
                                    &error)) {
        _LOG2W(LOGD_SETTINGS,
               device,
               "assume: failure to save generated connection '%s': %s",
               nm_connection_get_id(connection),
               error->message);
        g_error_free(error);
        return NULL;
    }

    NM_SET_OUT(out_generated, TRUE);
    return added;
}

static gboolean
copy_lease(const char *src, const char *dst)
{
    nm_auto_close int src_fd = -1;
    int               dst_fd;
    ssize_t           res, size = SSIZE_MAX;

    src_fd = open(src, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0)
        return FALSE;

    dst_fd = open(dst, O_CREAT | O_EXCL | O_CLOEXEC | O_WRONLY, 0644);
    if (dst_fd < 0)
        return FALSE;

    while ((res = sendfile(dst_fd, src_fd, NULL, size)) > 0)
        size -= res;

    nm_close(dst_fd);

    if (res != 0) {
        unlink(dst);
        return FALSE;
    }

    return TRUE;
}

static gboolean
recheck_assume_connection(NMManager *self, NMDevice *device)
{
    NMSettingsConnection *sett_conn;
    gboolean              was_unmanaged = FALSE;
    gboolean              generated     = FALSE;
    NMDeviceState         state;
    gboolean              activation_type_assume;

    g_return_val_if_fail(NM_IS_MANAGER(self), FALSE);
    g_return_val_if_fail(NM_IS_DEVICE(device), FALSE);

    if (!nm_device_get_managed(device, FALSE)) {
        /* If the device is unmanaged by NM_UNMANAGED_PLATFORM_INIT,
         * don't reset the state now but wait until it becomes managed. */
        if (nm_device_get_unmanaged_flags(device, NM_UNMANAGED_ALL) & ~NM_UNMANAGED_PLATFORM_INIT)
            nm_device_assume_state_reset(device);
        _LOG2D(LOGD_DEVICE, device, "assume: don't assume because device is not managed");
        return FALSE;
    }

    state = nm_device_get_state(device);
    if (state > NM_DEVICE_STATE_DISCONNECTED) {
        _LOG2D(LOGD_DEVICE,
               device,
               "assume: don't assume due to device state %s",
               nm_device_state_to_string(state));
        return FALSE;
    }

    sett_conn = get_existing_connection(self, device, &generated);
    /* log  no reason. get_existing_connection() already does it. */
    if (!sett_conn)
        return FALSE;

    activation_type_assume = !generated;

    if (state == NM_DEVICE_STATE_UNMANAGED) {
        gs_free char *initramfs_lease =
            g_strdup_printf(RUNSTATEDIR "/initramfs/net.%s.lease", nm_device_get_iface(device));
        gs_free char *connection_lease = g_strdup_printf(NMRUNDIR "/dhclient-%s-%s.lease",
                                                         nm_settings_connection_get_uuid(sett_conn),
                                                         nm_device_get_iface(device));

        if (copy_lease(initramfs_lease, connection_lease)) {
            unlink(initramfs_lease);
            /*
             * We've managed to steal the lease used by initramfs before it
             * killed off the dhclient. We need to take ownership of the configured
             * connection and act like the device was configured by us.
             * Otherwise, the address would just expire.
             */
            _LOG2I(LOGD_DEVICE, device, "assume: taking over an initramfs-configured connection");
            activation_type_assume = TRUE;

            if (generated) {
                gs_unref_object NMConnection *con2 = NULL;

                con2 = nm_simple_connection_new_clone(
                    nm_settings_connection_get_connection(sett_conn));

                /* Reset the IPv4 setting to empty method=auto, regardless of what assumption guessed. */
                nm_connection_add_setting(con2,
                                          g_object_new(NM_TYPE_SETTING_IP4_CONFIG,
                                                       NM_SETTING_IP_CONFIG_METHOD,
                                                       NM_SETTING_IP4_CONFIG_METHOD_AUTO,
                                                       NULL));

                nm_settings_connection_update(
                    sett_conn,
                    NULL,
                    con2,
                    NM_SETTINGS_CONNECTION_PERSIST_MODE_KEEP,
                    NM_SETTINGS_CONNECTION_INT_FLAGS_NONE,
                    NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                        | NM_SETTINGS_CONNECTION_INT_FLAGS_EXTERNAL,
                    NM_SETTINGS_CONNECTION_UPDATE_REASON_UPDATE_NON_SECRET,
                    "assume-initrd",
                    NULL);
            }
        }
    }

    nm_device_managed_type_set(device,
                               activation_type_assume ? NM_DEVICE_MANAGED_TYPE_ASSUME
                                                      : NM_DEVICE_MANAGED_TYPE_EXTERNAL);

    /* Move device to DISCONNECTED to activate the connection */
    if (state == NM_DEVICE_STATE_UNMANAGED) {
        was_unmanaged = TRUE;
        nm_device_state_changed(device,
                                NM_DEVICE_STATE_UNAVAILABLE,
                                NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED);
    }
    if (nm_device_get_state(device) == NM_DEVICE_STATE_UNAVAILABLE) {
        nm_device_state_changed(device,
                                NM_DEVICE_STATE_DISCONNECTED,
                                NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED);
    }

    g_return_val_if_fail(nm_device_get_state(device) >= NM_DEVICE_STATE_DISCONNECTED, FALSE);

    {
        gs_unref_object NMActiveConnection *active  = NULL;
        gs_unref_object NMAuthSubject      *subject = NULL;
        NMActiveConnection                 *controller_ac;
        GError                             *error = NULL;

        subject = nm_auth_subject_new_internal();

        /* Note: the lifetime of the activation connection is always bound to the profiles visibility
         * via NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY.
         *
         * This only makes a difference, if the profile actually has "connection.permissions"
         * set to limit visibility (which is not the case for externally managed, generated profiles).
         *
         * If we assume a previously active connection whose lifetime was unbound, we now bind it
         * after restart. That is not correct, and can mean that the profile becomes subject to
         * deactivation after restart (if the user logs out).
         *
         * This should be improved, but it's unclear how. */
        active = _new_active_connection(
            self,
            FALSE,
            sett_conn,
            NULL,
            NULL,
            NULL,
            device,
            subject,
            activation_type_assume ? NM_ACTIVATION_TYPE_ASSUME : NM_ACTIVATION_TYPE_EXTERNAL,
            activation_type_assume ? NM_ACTIVATION_REASON_ASSUME : NM_ACTIVATION_REASON_EXTERNAL,
            NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY,
            &error);

        if (!active) {
            _LOGW(LOGD_DEVICE,
                  "assume: assumed connection %s failed to activate: %s",
                  nm_dbus_object_get_path(NM_DBUS_OBJECT(sett_conn)),
                  error->message);
            g_error_free(error);

            if (was_unmanaged) {
                nm_device_state_changed(device,
                                        NM_DEVICE_STATE_UNAVAILABLE,
                                        NM_DEVICE_STATE_REASON_CONFIG_FAILED);
            }

            if (generated && !activation_type_assume) {
                _LOG2D(LOGD_DEVICE,
                       device,
                       "assume: deleting generated connection after assuming failed");
                nm_settings_connection_delete(sett_conn, FALSE);
            } else {
                if (nm_device_managed_type_get(device) == NM_DEVICE_MANAGED_TYPE_ASSUME)
                    nm_device_managed_type_set(device, NM_DEVICE_MANAGED_TYPE_EXTERNAL);
            }
            return FALSE;
        }

        /* If the device is a port or VLAN, find the controller ActiveConnection */
        controller_ac = NULL;
        if (find_controller(self,
                            nm_settings_connection_get_connection(sett_conn),
                            device,
                            NULL,
                            NULL,
                            &controller_ac,
                            NULL)
            && controller_ac)
            nm_active_connection_set_controller(active, controller_ac);

        active_connection_add(self, active);
        nm_device_queue_activation(device, NM_ACT_REQUEST(active));
    }

    return TRUE;
}

static void
recheck_assume_connection_cb(NMManager *self, NMDevice *device)
{
    recheck_assume_connection(self, device);
}

static void
device_ifindex_changed(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    _parent_notify_changed(self, device, FALSE);
}

static void
device_ip_iface_changed(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    NMManagerPrivate *priv        = NM_MANAGER_GET_PRIVATE(self);
    const char       *ip_iface    = nm_device_get_ip_iface(device);
    NMDeviceType      device_type = nm_device_get_device_type(device);
    NMDevice         *candidate;

    /* Remove NMDevice objects that are actually child devices of others,
     * when the other device finally knows its IP interface name.  For example,
     * remove the PPP interface that's a child of a WWAN device, since it's
     * not really a standalone NMDevice.
     */
    c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
        if (candidate != device && nm_streq0(nm_device_get_iface(candidate), ip_iface)
            && nm_device_get_device_type(candidate) == device_type
            && nm_device_is_real(candidate)) {
            remove_device(self, candidate, FALSE);
            break;
        }
    }
}

static void
device_iface_changed(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    /* Virtual connections may refer to the new device name as
     * parent device, retry to activate them.
     */
    retry_connections_for_parent_device(self, device);
}

static void
_emit_device_added_removed(NMManager *self, NMDevice *device, gboolean is_added)
{
    nm_dbus_object_emit_signal(NM_DBUS_OBJECT(self),
                               &interface_info_manager,
                               is_added ? &signal_info_device_added : &signal_info_device_removed,
                               "(o)",
                               nm_dbus_object_get_path(NM_DBUS_OBJECT(device)));
    g_signal_emit(self, signals[is_added ? DEVICE_ADDED : DEVICE_REMOVED], 0, device);
    _notify(self, PROP_DEVICES);
}

static void
device_realized(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    _emit_device_added_removed(self, device, nm_device_is_real(device));
}

static NMConnectivityState
_get_best_connectivity(NMManager *self, int addr_family)
{
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMConnectivityState best_state;
    NMDevice           *dev;
    gint64              best_metric;

    if (addr_family == AF_UNSPEC) {
        best_state = _get_best_connectivity(self, AF_INET);
        if (nm_connectivity_state_cmp(best_state, NM_CONNECTIVITY_FULL) >= 0) {
            /* already FULL IPv4 connectivity. No need to check IPv6, it doesn't get
             * better. */
            return best_state;
        }
        return NM_MAX_WITH_CMP(nm_connectivity_state_cmp,
                               best_state,
                               _get_best_connectivity(self, AF_INET6));
    }

    nm_assert_addr_family(addr_family);

    best_state  = NM_CONNECTIVITY_UNKNOWN;
    best_metric = G_MAXINT64;
    c_list_for_each_entry (dev, &priv->devices_lst_head, devices_lst) {
        const NMPObject    *r;
        NMConnectivityState state;
        gint64              metric;

        if (NM_IS_DEVICE_LOOPBACK(dev))
            continue;

        r = nm_device_get_best_default_route(dev, addr_family);
        if (r)
            metric = NMP_OBJECT_CAST_IP_ROUTE(r)->metric;
        else {
            /* if all devices have no default-route, we still include the best
             * of all connectivity state of all the devices. */
            metric = G_MAXINT64;
        }

        if (metric > best_metric) {
            /* we already have a default route with better metric. The connectivity state
             * of this device is irreleavnt. */
            continue;
        }

        state = nm_device_get_connectivity_state(dev, addr_family);
        if (metric < best_metric) {
            /* this device has a better default route. It wins. */
            best_metric = metric;
            best_state  = state;
        } else {
            best_state = NM_MAX_WITH_CMP(nm_connectivity_state_cmp, best_state, state);
        }

        if (nm_connectivity_state_cmp(best_state, NM_CONNECTIVITY_FULL) >= 0) {
            /* it doesn't get better than FULL. We are done. */
            break;
        }
    }

    return best_state;
}

static void
device_connectivity_changed(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    update_connectivity_value(self);
}

static void
update_connectivity_value(NMManager *self)
{
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMConnectivityState best_state;

    best_state = _get_best_connectivity(self, AF_UNSPEC);
    if (best_state == priv->connectivity_state)
        return;

    priv->connectivity_state = best_state;

    _LOGD(LOGD_CORE,
          "connectivity checking indicates %s",
          nm_connectivity_state_to_string(priv->connectivity_state));

    nm_manager_update_state(self);
    _notify(self, PROP_CONNECTIVITY);
    nm_dispatcher_call_connectivity(priv->connectivity_state, NULL, NULL, NULL);
}

static void
_device_realize_finish(NMManager *self, NMDevice *device, const NMPlatformLink *plink)
{
    g_return_if_fail(NM_IS_MANAGER(self));
    g_return_if_fail(NM_IS_DEVICE(device));

    nm_device_realize_finish(device, plink);

    if (!nm_device_get_managed(device, FALSE)) {
        /* If the device is unmanaged by NM_UNMANAGED_PLATFORM_INIT,
         * don't reset the state now but wait until it becomes managed. */
        if (nm_device_get_unmanaged_flags(device, NM_UNMANAGED_ALL) & ~NM_UNMANAGED_PLATFORM_INIT)
            nm_device_assume_state_reset(device);
        return;
    }

    if (recheck_assume_connection(self, device))
        return;

    /* if we failed to assume a connection for the managed device, but the device
     * is still unavailable. Set UNAVAILABLE state again, this time with NOW_MANAGED. */
    nm_device_state_changed(device,
                            NM_DEVICE_STATE_UNAVAILABLE,
                            nm_device_get_manage_reason_external(device));
    nm_manager_device_recheck_auto_activate_schedule(self, device);
}

/**
 * add_device:
 * @self: the #NMManager
 * @device: the #NMDevice to add
 * @error: the #GError
 *
 * If successful, this function will increase the references count of @device.
 * Callers should decrease the reference count.
 */
static gboolean
add_device(NMManager *self, NMDevice *device, GError **error)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    const char       *iface, *type_desc;
    NMRfkillType      rtype;
    GSList           *iter, *remove = NULL;
    int               ifindex;
    const char       *dbus_path;
    NMDevice         *candidate;

    /* No duplicates */
    ifindex = nm_device_get_ifindex(device);
    if (ifindex > 0 && nm_manager_get_device_by_ifindex(self, ifindex)) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "A device with ifindex %d already exists",
                    ifindex);
        return FALSE;
    }

    /* Remove existing devices owned by the new device; eg remove ethernet
     * ports that are owned by a WWAN modem, since udev may announce them
     * before the modem is fully discovered.
     *
     * FIXME: use parent/child device relationships instead of removing
     * the child NMDevice entirely
     */
    c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
        if (nm_device_is_real(candidate) && (iface = nm_device_get_ip_iface(candidate))
            && nm_device_owns_iface(device, iface))
            remove = g_slist_prepend(remove, candidate);
    }
    for (iter = remove; iter; iter = iter->next)
        remove_device(self, NM_DEVICE(iter->data), FALSE);
    g_slist_free(remove);

    g_object_ref(device);

    nm_assert(c_list_is_empty(&device->devices_lst));
    c_list_link_tail(&priv->devices_lst_head, &device->devices_lst);

    g_signal_connect(device,
                     NM_DEVICE_STATE_CHANGED,
                     G_CALLBACK(manager_device_state_changed),
                     self);

    g_signal_connect(device, NM_DEVICE_REMOVED, G_CALLBACK(device_removed_cb), self);

    g_signal_connect_data(device,
                          NM_DEVICE_RECHECK_ASSUME,
                          G_CALLBACK(recheck_assume_connection_cb),
                          self,
                          NULL,
                          G_CONNECT_SWAPPED);

    g_signal_connect(device,
                     "notify::" NM_DEVICE_IP_IFACE,
                     G_CALLBACK(device_ip_iface_changed),
                     self);

    g_signal_connect(device,
                     "notify::" NM_DEVICE_IFINDEX,
                     G_CALLBACK(device_ifindex_changed),
                     self);

    g_signal_connect(device, "notify::" NM_DEVICE_IFACE, G_CALLBACK(device_iface_changed), self);

    g_signal_connect(device, "notify::" NM_DEVICE_REAL, G_CALLBACK(device_realized), self);

    g_signal_connect(device,
                     "notify::" NM_DEVICE_IP4_CONNECTIVITY,
                     G_CALLBACK(device_connectivity_changed),
                     self);
    g_signal_connect(device,
                     "notify::" NM_DEVICE_IP6_CONNECTIVITY,
                     G_CALLBACK(device_connectivity_changed),
                     self);

    if (priv->startup) {
        g_signal_connect(device,
                         "notify::" NM_DEVICE_HAS_PENDING_ACTION,
                         G_CALLBACK(device_has_pending_action_changed),
                         self);
    }

    /* Update global rfkill state for this device type with the device's
     * rfkill state, and then set this device's rfkill state based on the
     * global state.
     */
    rtype = nm_device_get_rfkill_type(device);
    if (rtype != NM_RFKILL_TYPE_UNKNOWN) {
        _rfkill_update(self, rtype);
        nm_device_set_enabled(device, _rfkill_radio_state_get(self, rtype));
    }

    iface = nm_device_get_iface(device);
    g_assert(iface);
    type_desc = nm_device_get_type_desc(device);
    g_assert(type_desc);

    nm_device_set_unmanaged_by_user_settings(device, TRUE);

    nm_device_set_unmanaged_flags(device, NM_UNMANAGED_SLEEPING, manager_sleeping(self));

    dbus_path = nm_dbus_object_export(NM_DBUS_OBJECT(device));
    _LOG2I(LOGD_DEVICE, device, "new %s device (%s)", type_desc, dbus_path);

    nm_settings_device_added(priv->settings, device);
    g_signal_emit(self, signals[INTERNAL_DEVICE_ADDED], 0, device);
    _notify(self, PROP_ALL_DEVICES);

    _parent_notify_changed(self, device, FALSE);

    return TRUE;
}

/*****************************************************************************/

static void
factory_device_added_cb(NMDeviceFactory *factory, NMDevice *device, gpointer user_data)
{
    NMManager *self  = user_data;
    GError    *error = NULL;

    g_return_if_fail(NM_IS_MANAGER(self));

    if (nm_device_realize_start(device,
                                NULL,
                                FALSE, /* assume_state_guess_assume */
                                NULL,  /* assume_state_connection_uuid */
                                FALSE, /* set_nm_owned */
                                NM_UNMAN_FLAG_OP_FORGET,
                                NULL,
                                &error)) {
        add_device(self, device, NULL);
        _device_realize_finish(self, device, NULL);
        retry_connections_for_parent_device(self, device);
    } else {
        _LOG2W(LOGD_DEVICE, device, "failed to realize device: %s", error->message);
        g_error_free(error);
    }
}

static void
_register_device_factory(NMDeviceFactory *factory, gpointer user_data)
{
    NMManager *self = NM_MANAGER(user_data);

    g_signal_connect(factory,
                     NM_DEVICE_FACTORY_DEVICE_ADDED,
                     G_CALLBACK(factory_device_added_cb),
                     self);
}

/*****************************************************************************/

void
nm_manager_notify_device_availability_maybe_changed(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst)
        nm_device_notify_availability_maybe_changed(device);
}

/*****************************************************************************/

static void
platform_link_added(NMManager                     *self,
                    int                            ifindex,
                    const NMPlatformLink          *plink,
                    gboolean                       guess_assume,
                    const NMConfigDeviceStateData *dev_state)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDeviceFactory  *factory;
    NMDevice         *device = NULL;
    NMDevice         *candidate;

    g_return_if_fail(ifindex > 0);

    if (nm_manager_get_device_by_ifindex(self, ifindex))
        return;

    /* Let unrealized devices try to realize themselves with the link */
    c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
        gboolean              compatible = TRUE;
        gs_free_error GError *error      = NULL;

        if (nm_device_get_device_type(candidate) == NM_DEVICE_TYPE_GENERIC) {
            /* generic devices are compatible with all link types */
        } else if (nm_device_get_link_type(candidate) != plink->type) {
            continue;
        }

        if (!nm_streq(nm_device_get_iface(candidate), plink->name))
            continue;

        if (nm_device_is_real(candidate)) {
            /* There's already a realized device with the link's name
             * and a different ifindex.
             */
            if (nm_device_get_ifindex(candidate) <= 0)
                nm_device_update_from_platform_link(candidate, plink);
            else {
                /* The ifindex of a device can't be changed after
                 * initialization because it is used as a key by
                 * the dns-manager.
                 */
                _LOGD(LOGD_DEVICE,
                      "(%s): removing old device %p after ifindex change from %d to %d",
                      plink->name,
                      candidate,
                      nm_device_get_ifindex(candidate),
                      ifindex);
                remove_device(self, candidate, FALSE);
                goto add;
            }
            return;
        } else if (nm_device_realize_start(candidate,
                                           plink,
                                           FALSE, /* assume_state_guess_assume */
                                           NULL,  /* assume_state_connection_uuid */
                                           FALSE, /* set_nm_owned */
                                           NM_UNMAN_FLAG_OP_FORGET,
                                           &compatible,
                                           &error)) {
            _device_realize_finish(self, candidate, plink);
            return;
        }

        _LOGD(LOGD_DEVICE, "(%s): failed to realize from plink: '%s'", plink->name, error->message);

        /* Try next unrealized device */
    }

add:
    if (dev_state && dev_state->generic_sw) {
        factory = nm_device_factory_get_generic_factory();
    } else {
        /* Try registered device factories */
        factory = nm_device_factory_manager_find_factory_for_link_type(plink->type);
    }
    if (factory) {
        gboolean              ignore = FALSE;
        gs_free_error GError *error  = NULL;

        device =
            nm_device_factory_create_device(factory, plink->name, plink, NULL, &ignore, &error);
        if (!device) {
            if (!ignore) {
                _LOGW(LOGD_PLATFORM,
                      "%s: factory failed to create device: %s",
                      plink->name,
                      error->message);
            } else {
                _LOGD(LOGD_PLATFORM,
                      "%s: factory failed to create device: %s",
                      plink->name,
                      error->message);
            }
            return;
        }
    }

    if (device == NULL) {
        gboolean nm_plugin_missing = FALSE;

        switch (plink->type) {
        case NM_LINK_TYPE_WWAN_NET:
        case NM_LINK_TYPE_BNEP:
        case NM_LINK_TYPE_OLPC_MESH:
        case NM_LINK_TYPE_TEAM:
        case NM_LINK_TYPE_WIFI:
        case NM_LINK_TYPE_OPENVSWITCH:
            _LOGI(LOGD_PLATFORM,
                  "(%s): '%s' plugin not available; creating generic device",
                  plink->name,
                  nm_link_type_to_string(plink->type));
            nm_plugin_missing = TRUE;
            /* fall-through */
        default:
            device = nm_device_generic_new(plink, nm_plugin_missing);
            break;
        }
    }

    if (device) {
        gs_free_error GError *error                   = NULL;
        NMUnmanFlagOp         unmanaged_user_explicit = NM_UNMAN_FLAG_OP_FORGET;

        if (dev_state) {
            switch (dev_state->managed) {
            case NM_CONFIG_DEVICE_STATE_MANAGED_TYPE_MANAGED:
                unmanaged_user_explicit = NM_UNMAN_FLAG_OP_SET_MANAGED;
                break;
            case NM_CONFIG_DEVICE_STATE_MANAGED_TYPE_UNMANAGED:
                unmanaged_user_explicit = NM_UNMAN_FLAG_OP_SET_UNMANAGED;
                break;
            case NM_CONFIG_DEVICE_STATE_MANAGED_TYPE_UNKNOWN:
                break;
            }
        }

        if (nm_device_realize_start(device,
                                    plink,
                                    guess_assume,
                                    dev_state ? dev_state->connection_uuid : NULL,
                                    dev_state ? (dev_state->nm_owned == 1) : FALSE,
                                    unmanaged_user_explicit,
                                    NULL,
                                    &error)) {
            add_device(self, device, NULL);
            _device_realize_finish(self, device, plink);
            retry_connections_for_parent_device(self, device);
        } else {
            _LOGW(LOGD_DEVICE, "%s: failed to realize device: %s", plink->name, error->message);
        }
        g_object_unref(device);
    }
}

typedef struct {
    CList      lst;
    NMManager *self;
    int        ifindex;
    guint      idle_id;
} PlatformLinkCbData;

static gboolean
_check_remove_dev_on_link_deleted(NMManager *self, NMDevice *device)
{
    NMManagerPrivate            *priv  = NM_MANAGER_GET_PRIVATE(self);
    NMSettingsConnection *const *scons = NULL;
    NMConnection                *con;
    guint                        i;

    nm_assert(nm_device_is_software(device));

    /* In general, software devices stick around as unrealized
     * until their connection is removed. However, we don't want
     * that a NM-generated connection keeps the device alive.
     * If there are no other compatible connections, the device
     * should be also removed.
     */

    scons = nm_settings_get_connections(priv->settings, NULL);

    for (i = 0; scons[i]; i++) {
        con = nm_settings_connection_get_connection(scons[i]);
        if (!nm_connection_is_virtual(con))
            continue;

        if (NM_FLAGS_HAS(nm_settings_connection_get_flags(scons[i]),
                         NM_SETTINGS_CONNECTION_INT_FLAGS_NM_GENERATED))
            continue;

        if (!nm_device_check_connection_compatible(device, con, TRUE, NULL))
            continue;

        /* Found a virtual connection compatible, the device must
         * stay around unrealized. */
        return FALSE;
    }

    return TRUE;
}

static gboolean
_platform_link_cb_idle(PlatformLinkCbData *data)
{
    int                   ifindex = data->ifindex;
    NMManager            *self    = data->self;
    NMManagerPrivate     *priv    = NM_MANAGER_GET_PRIVATE(self);
    const NMPlatformLink *plink;

    c_list_unlink_stale(&data->lst);
    g_slice_free(PlatformLinkCbData, data);

    plink = nm_platform_link_get(priv->platform, ifindex);
    if (plink) {
        const NMPObject *plink_keep_alive = nmp_object_ref(NMP_OBJECT_UP_CAST(plink));

        platform_link_added(self, ifindex, plink, FALSE, NULL);
        nmp_object_unref(plink_keep_alive);
    } else {
        NMDevice *device;
        GError   *error = NULL;

        device = nm_manager_get_device_by_ifindex(self, ifindex);
        if (device) {
            if (nm_device_is_software(device)) {
                nm_device_managed_type_set(device, NM_DEVICE_MANAGED_TYPE_REMOVED);
                if (!nm_device_unrealize(device, FALSE, &error)) {
                    _LOG2W(LOGD_DEVICE, device, "failed to unrealize: %s", error->message);
                    g_clear_error(&error);
                    remove_device(self, device, FALSE);
                } else {
                    if (_check_remove_dev_on_link_deleted(self, device))
                        remove_device(self, device, FALSE);
                    else
                        nm_device_update_from_platform_link(device, NULL);
                }
            } else {
                /* Hardware and external devices always get removed when their kernel link is gone */
                remove_device(self, device, FALSE);
            }
        }
    }

    return G_SOURCE_REMOVE;
}

static void
platform_link_cb(NMPlatform     *platform,
                 int             obj_type_i,
                 int             ifindex,
                 NMPlatformLink *plink,
                 int             change_type_i,
                 gpointer        user_data)
{
    NMManager                       *self;
    NMManagerPrivate                *priv;
    const NMPlatformSignalChangeType change_type = change_type_i;
    PlatformLinkCbData              *data;

    switch (change_type) {
    case NM_PLATFORM_SIGNAL_ADDED:
    case NM_PLATFORM_SIGNAL_REMOVED:
        self = NM_MANAGER(user_data);
        priv = NM_MANAGER_GET_PRIVATE(self);

        data          = g_slice_new(PlatformLinkCbData);
        data->self    = self;
        data->ifindex = ifindex;
        c_list_link_tail(&priv->link_cb_lst, &data->lst);
        data->idle_id = g_idle_add((GSourceFunc) _platform_link_cb_idle, data);
        break;
    default:
        break;
    }
}

static void
platform_query_devices(NMManager *self)
{
    NMManagerPrivate            *priv  = NM_MANAGER_GET_PRIVATE(self);
    gs_unref_ptrarray GPtrArray *links = NULL;
    int                          i;
    gboolean                     guess_assume;
    gs_free char                *order = NULL;

    guess_assume = nm_config_get_first_start(nm_config_get());
    links        = nm_platform_link_get_all(priv->platform);
    if (!links)
        return;

    for (i = 0; i < links->len; i++) {
        const NMPlatformLink          *elem = NMP_OBJECT_CAST_LINK(links->pdata[i]);
        const NMPlatformLink          *link;
        const NMConfigDeviceStateData *dev_state;

        /*
         * @links is an immutable snapshot of the platform links captured before
         * the loop was started. It's possible that in the meantime, while
         * processing netlink events in platform_link_added(), a link was
         * renamed.  If that happens, we have 2 different views of the same
         * ifindex: the one from @links and the one from platform. This can
         * cause race conditions; make sure to use the latest known version of
         * the link.
         */
        link = nm_platform_link_get(priv->platform, elem->ifindex);
        if (!link)
            continue;

        dev_state = nm_config_device_state_get(priv->config, link->ifindex);
        platform_link_added(self,
                            link->ifindex,
                            link,
                            guess_assume && (!dev_state || !dev_state->connection_uuid),
                            dev_state);
    }
}

static void
rfkill_manager_rfkill_changed_cb(NMRfkillManager          *rfkill_mgr,
                                 /* NMRfkillType */ guint  rtype,
                                 /* NMRfkillState */ guint udev_state,
                                 gpointer                  user_data)
{
    nm_assert(rtype < NM_RFKILL_TYPE_MAX);

    _rfkill_update(NM_MANAGER(user_data), rtype);
}

const CList *
nm_manager_get_devices(NMManager *manager)
{
    g_return_val_if_fail(NM_IS_MANAGER(manager), NULL);

    return &NM_MANAGER_GET_PRIVATE(manager)->devices_lst_head;
}

typedef enum {
    DEVICE_ACTIVATION_PRIO_NONE,
    DEVICE_ACTIVATION_PRIO_UNMANAGED,
    DEVICE_ACTIVATION_PRIO_UNAVAILABLE,
    DEVICE_ACTIVATION_PRIO_DEACTIVATING,
    DEVICE_ACTIVATION_PRIO_ACTIVATING,
    DEVICE_ACTIVATION_PRIO_ACTIVATED,
    DEVICE_ACTIVATION_PRIO_DISCONNECTED,

    _DEVICE_ACTIVATION_PRIO_BEST = DEVICE_ACTIVATION_PRIO_DISCONNECTED,
} DeviceActivationPrio;

static DeviceActivationPrio
_device_get_activation_prio(NMDevice *device)
{
    if (!nm_device_get_managed(device, TRUE))
        return DEVICE_ACTIVATION_PRIO_NONE;

    switch (nm_device_get_state(device)) {
    case NM_DEVICE_STATE_DISCONNECTED:
        return DEVICE_ACTIVATION_PRIO_DISCONNECTED;
    case NM_DEVICE_STATE_ACTIVATED:
        return DEVICE_ACTIVATION_PRIO_ACTIVATED;
    case NM_DEVICE_STATE_PREPARE:
    case NM_DEVICE_STATE_CONFIG:
    case NM_DEVICE_STATE_NEED_AUTH:
    case NM_DEVICE_STATE_IP_CONFIG:
    case NM_DEVICE_STATE_IP_CHECK:
    case NM_DEVICE_STATE_SECONDARIES:
        return DEVICE_ACTIVATION_PRIO_ACTIVATING;
    case NM_DEVICE_STATE_DEACTIVATING:
    case NM_DEVICE_STATE_FAILED:
        return DEVICE_ACTIVATION_PRIO_DEACTIVATING;
    case NM_DEVICE_STATE_UNAVAILABLE:
        return DEVICE_ACTIVATION_PRIO_UNAVAILABLE;
    case NM_DEVICE_STATE_UNKNOWN:
    case NM_DEVICE_STATE_UNMANAGED:
        return DEVICE_ACTIVATION_PRIO_UNMANAGED;
    }

    g_return_val_if_reached(DEVICE_ACTIVATION_PRIO_UNAVAILABLE);
}

static NMDevice *
nm_manager_get_best_device_for_connection(NMManager            *self,
                                          NMSettingsConnection *sett_conn,
                                          NMConnection         *connection,
                                          gboolean              for_user_request,
                                          GHashTable           *exclude_devices,
                                          GError              **error)
{
    NMManagerPrivate       *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnectionState ac_state;
    NMActiveConnection     *ac;
    NMDevice               *ac_device;
    NMDevice               *device;
    struct {
        NMDevice            *device;
        DeviceActivationPrio prio;
    } best = {
        .device = NULL,
        .prio   = DEVICE_ACTIVATION_PRIO_NONE,
    };
    NMDeviceCheckConAvailableFlags flags;
    gs_unref_ptrarray GPtrArray   *all_ac_arr = NULL;
    gs_free_error GError          *local_best = NULL;
    NMConnectionMultiConnect       multi_connect;

    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!connection || NM_IS_CONNECTION(connection));
    nm_assert(sett_conn || connection);
    nm_assert(!connection || !sett_conn
              || connection == nm_settings_connection_get_connection(sett_conn));

    if (!connection)
        connection = nm_settings_connection_get_connection(sett_conn);

    multi_connect = _nm_connection_get_multi_connect(connection);

    if (!for_user_request)
        flags = NM_DEVICE_CHECK_CON_AVAILABLE_NONE;
    else {
        /* if the profile is multi-connect=single, we also consider devices which
         * are marked as unmanaged. And explicit user-request shows sufficient user
         * intent to make the device managed.
         * That is also, because we expect that such profile is suitably tied
         * to the intended device. So when an unmanaged device matches, the user's
         * intent is clear.
         *
         * For multi-connect != single devices that is different. The profile
         * is not restricted to a particular device.
         * For that reason, plain `nmcli connection up "$MULIT_PROFILE"` seems
         * less suitable for multi-connect profiles, because the target device is
         * left unspecified. Anyway, if a user issues
         *
         *   $ nmcli device set "$DEVICE" managed no
         *   $ nmcli connection up "$MULIT_PROFILE"
         *
         * then it is reasonable for multi-connect profiles to not consider
         * the device a suitable candidate.
         *
         * This may be seen inconsistent, but I think that it makes a lot of
         * sense. Also note that "connection.multi-connect" work quite differently
         * in aspects like activation. E.g. `nmcli connection up` of multi-connect
         * "single" profile, will deactivate the profile if it is active already.
         * That is different from multi-connect profiles, where it will aim to
         * activate the profile one more time on an hitherto disconnected device.
         */
        if (multi_connect == NM_CONNECTION_MULTI_CONNECT_SINGLE)
            flags = NM_DEVICE_CHECK_CON_AVAILABLE_FOR_USER_REQUEST;
        else
            flags = NM_DEVICE_CHECK_CON_AVAILABLE_FOR_USER_REQUEST
                    & ~_NM_DEVICE_CHECK_CON_AVAILABLE_FOR_USER_REQUEST_OVERRULE_UNMANAGED;
    }

    if (multi_connect == NM_CONNECTION_MULTI_CONNECT_SINGLE
        && (ac = active_connection_find_by_connection(self,
                                                      sett_conn,
                                                      connection,
                                                      NM_ACTIVE_CONNECTION_STATE_DEACTIVATING,
                                                      &all_ac_arr))) {
        /* if we have a profile which may activate on only one device (multi-connect single), then
         * we prefer the device on which the profile is already active. It means to reactivate
         * the profile on the same device.
         *
         * If the profile can be activated on multiple devices, we don't do this. In fact, the
         * check below for the DeviceActivationPrio will prefer devices which are not already
         * activated (with this or another) profile. */

        ac_device = nm_active_connection_get_device(ac);
        if (ac_device
            && (nm_g_hash_table_contains(exclude_devices, ac_device)
                || !nm_device_check_connection_available(ac_device, connection, flags, NULL, NULL)))
            ac_device = NULL;

        if (all_ac_arr) {
            guint i;

            ac_state = nm_active_connection_get_state(ac);

            /* we found several active connections. See which one is the most suitable... */
            nm_assert(ac == all_ac_arr->pdata[0]);
            for (i = 1; i < all_ac_arr->len; i++) {
                NMActiveConnection     *ac2        = all_ac_arr->pdata[i];
                NMDevice               *ac_device2 = nm_active_connection_get_device(ac2);
                NMActiveConnectionState ac_state2;

                if (!ac_device2 || nm_g_hash_table_contains(exclude_devices, ac_device2)
                    || !nm_device_check_connection_available(ac_device2,
                                                             connection,
                                                             flags,
                                                             NULL,
                                                             NULL))
                    continue;

                ac_state2 = nm_active_connection_get_state(ac2);

                if (!ac_device)
                    goto found_better;

                if (ac_state == ac_state2) {
                    /* active-connections are in their list in the order in which they are connected.
                     * If we have two with same state, the later (newer) one is preferred. */
                    goto found_better;
                }

                switch (ac_state) {
                case NM_ACTIVE_CONNECTION_STATE_UNKNOWN:
                    if (NM_IN_SET(ac_state2,
                                  NM_ACTIVE_CONNECTION_STATE_ACTIVATING,
                                  NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
                                  NM_ACTIVE_CONNECTION_STATE_DEACTIVATING))
                        goto found_better;
                    break;
                case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
                    if (NM_IN_SET(ac_state2, NM_ACTIVE_CONNECTION_STATE_ACTIVATED))
                        goto found_better;
                    break;
                case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
                    break;
                case NM_ACTIVE_CONNECTION_STATE_DEACTIVATING:
                    if (NM_IN_SET(ac_state2,
                                  NM_ACTIVE_CONNECTION_STATE_ACTIVATING,
                                  NM_ACTIVE_CONNECTION_STATE_ACTIVATED))
                        goto found_better;
                    break;
                default:
                    nm_assert_not_reached();
                    goto found_better;
                }

                continue;
found_better:
                ac        = ac2;
                ac_state  = ac_state2;
                ac_device = ac_device2;
            }
        }

        if (ac_device)
            return ac_device;
    }

    /* Pick the first device that's compatible with the connection. */
    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        GError              *local = NULL;
        DeviceActivationPrio prio;

        if (nm_g_hash_table_contains(exclude_devices, device))
            continue;

        if (!nm_device_is_available(device,
                                    for_user_request
                                        ? NM_DEVICE_CHECK_DEV_AVAILABLE_FOR_USER_REQUEST
                                        : NM_DEVICE_CHECK_DEV_AVAILABLE_NONE))
            continue;

        /* determine the priority of this device. Currently, this priority is independent
         * of the profile (connection) and the device's details (aside the state).
         *
         * Maybe nm_device_check_connection_available() should instead return a priority,
         * as it has more information available.
         *
         * For example, if you have multiple Wi-Fi devices, currently a user-request would
         * also select the device if the AP is not visible. Optimally, if one of the two
         * devices sees the AP and the other one doesn't, the former would be preferred.
         * For that, the priority would need to be determined by nm_device_check_connection_available(). */
        prio = _device_get_activation_prio(device);
        if (prio <= best.prio && best.device) {
            /* we already have a matching device with a better priority. This candidate
             * cannot be better. Skip the check.
             *
             * Also note, that below we collect the best error message @local_best.
             * Since we already have best.device, the error message does not matter
             * either, and we can skip nm_device_check_connection_available() altogether. */
            continue;
        }

        if (nm_device_check_connection_available(device,
                                                 connection,
                                                 flags,
                                                 NULL,
                                                 error ? &local : NULL)) {
            if (prio == _DEVICE_ACTIVATION_PRIO_BEST) {
                /* this device already has the best priority. It cannot get better
                 * and finish the search. */
                return device;
            }
            best.prio   = prio;
            best.device = device;
            continue;
        }

        if (error) {
            gboolean reset_error;

            if (!local_best)
                reset_error = TRUE;
            else if (local_best->domain != NM_UTILS_ERROR)
                reset_error = (local->domain == NM_UTILS_ERROR);
            else {
                reset_error = (local->domain == NM_UTILS_ERROR && local_best->code < local->code);
            }

            if (reset_error) {
                g_clear_error(&local_best);
                g_set_error(&local_best,
                            local->domain,
                            local->code,
                            "device %s not available because %s",
                            nm_device_get_iface(device),
                            local->message);
            }
            g_error_free(local);
        }
    }

    if (best.device)
        return best.device;

    if (error) {
        if (local_best)
            g_propagate_error(error, g_steal_pointer(&local_best));
        else {
            nm_utils_error_set_literal(error, NM_UTILS_ERROR_UNKNOWN, "no suitable device found");
        }
    }
    return NULL;
}

static const char **
_get_devices_paths(NMManager *self, gboolean all_devices)
{
    NMManagerPrivate *priv  = NM_MANAGER_GET_PRIVATE(self);
    const char      **paths = NULL;
    guint             i;
    NMDevice         *device;

    paths = g_new(const char *, c_list_length(&priv->devices_lst_head) + 1);

    i = 0;
    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        const char *path;

        path = nm_dbus_object_get_path(NM_DBUS_OBJECT(device));
        if (!path)
            continue;

        if (!all_devices && !nm_device_is_real(device))
            continue;

        paths[i++] = path;
    }
    paths[i++] = NULL;

    return paths;
}

static void
impl_manager_get_devices(NMDBusObject                      *obj,
                         const NMDBusInterfaceInfoExtended *interface_info,
                         const NMDBusMethodInfoExtended    *method_info,
                         GDBusConnection                   *connection,
                         const char                        *sender,
                         GDBusMethodInvocation             *invocation,
                         GVariant                          *parameters)
{
    NMManager           *self  = NM_MANAGER(obj);
    gs_free const char **paths = NULL;

    paths = _get_devices_paths(self, FALSE);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(^ao)", (char **) paths));
}

static void
impl_manager_get_all_devices(NMDBusObject                      *obj,
                             const NMDBusInterfaceInfoExtended *interface_info,
                             const NMDBusMethodInfoExtended    *method_info,
                             GDBusConnection                   *connection,
                             const char                        *sender,
                             GDBusMethodInvocation             *invocation,
                             GVariant                          *parameters)
{
    NMManager           *self  = NM_MANAGER(obj);
    gs_free const char **paths = NULL;

    paths = _get_devices_paths(self, TRUE);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(^ao)", (char **) paths));
}

static void
impl_manager_get_device_by_ip_iface(NMDBusObject                      *obj,
                                    const NMDBusInterfaceInfoExtended *interface_info,
                                    const NMDBusMethodInfoExtended    *method_info,
                                    GDBusConnection                   *connection,
                                    const char                        *sender,
                                    GDBusMethodInvocation             *invocation,
                                    GVariant                          *parameters)
{
    NMManager  *self = NM_MANAGER(obj);
    NMDevice   *device;
    const char *path = NULL;
    const char *iface;

    g_variant_get(parameters, "(&s)", &iface);

    device = find_device_by_ip_iface(self, iface);
    if (device)
        path = nm_dbus_object_get_path(NM_DBUS_OBJECT(device));

    if (!path) {
        g_dbus_method_invocation_return_error(invocation,
                                              NM_MANAGER_ERROR,
                                              NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                                              "No device found for the requested iface.");
        return;
    }

    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path));
}

static gboolean
is_compatible_with_port(NMConnection *controller, NMConnection *port)
{
    NMSettingConnection *s_con;

    g_return_val_if_fail(controller, FALSE);
    g_return_val_if_fail(port, FALSE);

    s_con = nm_connection_get_setting_connection(port);
    g_assert(s_con);

    return nm_connection_is_type(controller, nm_setting_connection_get_port_type(s_con));
}

/**
 * find_controller:
 * @self: #NMManager object
 * @connection: the #NMConnection to find the controller connection and device for
 * @device: the #NMDevice, if any, which will activate @connection
 * @out_controller_connection: on success, the controller connection of @connection if
 *   that controller connection was found
 * @out_controller_device: on success, the controller device of @connection if that
 *   controller device was found
 * @out_controller_ac: on success, the controller ActiveConnection of @connection if
 *   there already is one
 * @error: the error, if an error occurred
 *
 * Given an #NMConnection, attempts to find its controller. If @connection has
 * no controller, this will return %TRUE and @out_controller_connection and
 * @out_controller_device will be untouched.
 *
 * If @connection does have a controller, then the outputs depend on what is in its
 * #NMSettingConnection:controller property:
 *
 * If "controller" is the ifname of an existing #NMDevice, and that device has a
 * compatible controller connection activated or activating on it, then
 * @out_controller_device, @out_controller_connection, and @out_controller_ac will all be
 * set. If the device exists and is idle, only @out_controller_device will be set.
 * If the device exists and has an incompatible connection on it, an error
 * will be returned.
 *
 * If "controller" is the ifname of a non-existent device, then @out_controller_device
 * will be %NULL, and @out_controller_connection will be a connection whose
 * activation would cause the creation of that device. @out_controller_ac MAY be
 * set in this case as well (if the connection has started activating, but has
 * not yet created its device).
 *
 * If "controller" is the UUID of a compatible controller connection, then
 * @out_controller_connection will be the identified connection, and @out_controller_device
 * and/or @out_controller_ac will be set if the connection is currently activating.
 * (@out_controller_device will not be set if the device exists but does not have
 * @out_controller_connection active/activating on it.)
 *
 * Returns: %TRUE if the controller device and/or connection could be found or if
 *  the connection did not require a controller, %FALSE otherwise
 **/
static gboolean
find_controller(NMManager             *self,
                NMConnection          *connection,
                NMDevice              *device,
                NMSettingsConnection **out_controller_connection,
                NMDevice             **out_controller_device,
                NMActiveConnection   **out_controller_ac,
                GError               **error)
{
    NMManagerPrivate            *priv = NM_MANAGER_GET_PRIVATE(self);
    NMSettingConnection         *s_con;
    const char                  *controller;
    NMDevice                    *controller_device     = NULL;
    NMSettingsConnection        *controller_connection = NULL;
    NMSettingsConnection *const *connections;
    guint                        i;

    nm_assert(!out_controller_connection || !*out_controller_connection);
    nm_assert(!out_controller_device || !*out_controller_device);
    nm_assert(!out_controller_ac || !*out_controller_ac);

    s_con      = nm_connection_get_setting_connection(connection);
    controller = nm_setting_connection_get_controller(s_con);

    if (controller == NULL)
        return TRUE; /* success, but no controller */

    _LOGD(LOGD_CORE,
          "Looking for a controller '%s' for connection '%s' (%s)",
          controller,
          nm_connection_get_id(connection),
          nm_connection_get_uuid(connection));

    connections = nm_settings_get_connections_sorted_by_autoconnect_priority(priv->settings, NULL);
    for (i = 0; connections[i]; i++) {
        NMConnection *controller_candidate = nm_settings_connection_get_connection(connections[i]);
        NMDevice     *device_candidate;

        if (nm_streq(nm_connection_get_uuid(controller_candidate), controller)) {
            if (!is_compatible_with_port(controller_candidate, connection)) {
                g_set_error(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                            "The active connection on %s is not compatible",
                            nm_device_get_iface(controller_device));
                return FALSE;
            }

            _LOGD(LOGD_CORE,
                  "Will consider using connection '%s' (%s) as a controller for '%s' (%s) "
                  "because UUID matches",
                  nm_connection_get_id(controller_candidate),
                  nm_connection_get_uuid(controller_candidate),
                  nm_connection_get_id(connection),
                  nm_connection_get_uuid(connection));

            controller_connection = connections[i];
        } else if (nm_connection_get_interface_name(controller_candidate)
                   && nm_streq(nm_connection_get_interface_name(controller_candidate),
                               controller)) {
            if (!is_compatible_with_port(controller_candidate, connection))
                continue;

            /* This might be good enough unless we find a better one (already active or UUID match) */
            if (!controller_connection) {
                controller_connection = connections[i];
                _LOGD(LOGD_CORE,
                      "Will consider using connection '%s' (%s) as a controller for '%s' (%s) "
                      "because device matches",
                      nm_connection_get_id(controller_candidate),
                      nm_connection_get_uuid(controller_candidate),
                      nm_connection_get_id(connection),
                      nm_connection_get_uuid(connection));
            }
        } else {
            /* No match. */
            continue;
        }

        /* Check if the controller connection is activated on some device already */
        c_list_for_each_entry (device_candidate, &priv->devices_lst_head, devices_lst) {
            if (device_candidate == device)
                continue;

            if (nm_device_get_settings_connection(device_candidate) == connections[i]) {
                controller_device     = device_candidate;
                controller_connection = connections[i];
                break;
            }
        }

        if (controller_device) {
            /* Now we got a connection and also a device. Look no further. */
            _LOGD(LOGD_CORE,
                  "Will use connection '%s' (%s) as a controller for '%s' (%s)",
                  nm_connection_get_id(controller_candidate),
                  nm_connection_get_uuid(controller_candidate),
                  nm_connection_get_id(connection),
                  nm_connection_get_uuid(connection));

            break;
        }
    }

    if (!controller_connection) {
        controller_device = find_device_by_iface(self, controller, NULL, connection, NULL);
        if (!controller_device) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                        "Connection or device %s not found",
                        controller);
            return FALSE;
        }

        if (controller_device == device) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                                "Device cannot be its own controller");
            return FALSE;
        }

        _LOGD(LOGD_CORE,
              "Controller connection for '%s' (%s) not found, will use device '%s'",
              nm_connection_get_id(connection),
              nm_connection_get_uuid(connection),
              nm_device_get_iface(controller_device));
    }

    if (!controller_device && !controller_connection) {
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                            "Controller connection not found or invalid");
        return FALSE;
    }

    NM_SET_OUT(out_controller_connection, controller_connection);
    NM_SET_OUT(out_controller_device, controller_device);
    if (out_controller_ac && controller_connection) {
        *out_controller_ac = active_connection_find(self,
                                                    controller_connection,
                                                    NULL,
                                                    NM_ACTIVE_CONNECTION_STATE_DEACTIVATING,
                                                    FALSE,
                                                    NULL);
    }

    return TRUE;
}

/**
 * ensure_controller_active_connection:
 * @self: the #NMManager
 * @subject: the #NMAuthSubject representing the requestor of this activation
 * @connection: the connection that should depend on @controller_connection
 * @device: the #NMDevice, if any, which will activate @connection
 * @controller_connection: the controller connection, or %NULL
 * @controller_device: the controller device, or %NULL
 * @activation_reason: the reason for activation
 * @error: the error, if an error occurred
 *
 * Determines whether a given #NMConnection depends on another connection to
 * be activated, and if so, finds that controller connection or creates it.
 *
 * If @controller_device and @controller_connection are both set then @controller_connection
 * MUST already be activated or activating on @controller_device, and the function will
 * return the existing #NMActiveConnection.
 *
 * If only @controller_device is set, and it has an #NMActiveConnection, then the
 * function will return it if it is a compatible controller, or an error if not. If it
 * doesn't have an AC, then the function will create one if a compatible controller
 * connection exists, or return an error if not.
 *
 * If only @controller_connection is set, then this will try to find or create a compatible
 * #NMDevice, and either activate @controller_connection on that device or return an error.
 *
 * Returns: the controller #NMActiveConnection that the caller should depend on, or
 * %NULL if an error occurred
 */
static NMActiveConnection *
ensure_controller_active_connection(NMManager            *self,
                                    NMAuthSubject        *subject,
                                    NMConnection         *connection,
                                    NMDevice             *device,
                                    NMSettingsConnection *controller_connection,
                                    NMDevice             *controller_device,
                                    NMActivationReason    activation_reason,
                                    GError              **error)
{
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *ac;
    NMActiveConnection *controller_ac = NULL;
    NMDeviceState       controller_state;
    gboolean            bind_lifetime_to_profile_visibility;

    g_return_val_if_fail(connection, NULL);
    g_return_val_if_fail(controller_connection || controller_device, FALSE);

    bind_lifetime_to_profile_visibility =
        NM_FLAGS_HAS(nm_device_get_activation_state_flags(device),
                     NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY);

    /* If the controller device isn't activated then we need to activate it using
     * compatible connection.  If it's already activating we can just proceed.
     */
    if (controller_device) {
        NMSettingsConnection *device_connection =
            nm_device_get_settings_connection(controller_device);

        /* If we're passed a connection and a device, we require that connection
         * be already activated on the device, eg returned from find_controller().
         */
        g_assert(!controller_connection || controller_connection == device_connection);
        if (device_connection
            && !is_compatible_with_port(nm_settings_connection_get_connection(device_connection),
                                        connection)) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                        "The active connection %s is not compatible",
                        nm_connection_get_id(connection));
            return NULL;
        }

        controller_state = nm_device_get_state(controller_device);
        if ((controller_state == NM_DEVICE_STATE_ACTIVATED)
            || nm_device_is_activating(controller_device)) {
            /* Device already using controller_connection */
            ac = NM_ACTIVE_CONNECTION(nm_device_get_act_request(controller_device));
            g_return_val_if_fail(device_connection, ac);

            if (!bind_lifetime_to_profile_visibility) {
                /* unbind the lifetime. */
                nm_active_connection_set_state_flags_clear(
                    ac,
                    NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY);
            }

            return ac;
        }

        /* If the device is disconnected, find a compatible connection and
         * activate it on the device.
         */
        if (controller_state == NM_DEVICE_STATE_DISCONNECTED
            || !nm_device_is_real(controller_device)) {
            gs_free NMSettingsConnection **connections = NULL;
            guint                          i;

            g_assert(controller_connection == NULL);

            /* Find a compatible connection and activate this device using it */
            connections = nm_manager_get_activatable_connections(self, FALSE, TRUE, NULL);
            for (i = 0; connections[i]; i++) {
                NMSettingsConnection *candidate = connections[i];
                NMConnection         *cand_conn = nm_settings_connection_get_connection(candidate);

                /* Ensure eg bond/team port and the candidate controller is a
                 * bond/team controller
                 */
                if (!is_compatible_with_port(cand_conn, connection))
                    continue;

                if (nm_device_check_connection_available(
                        controller_device,
                        cand_conn,
                        NM_DEVICE_CHECK_CON_AVAILABLE_FOR_USER_REQUEST,
                        NULL,
                        NULL)) {
                    controller_ac = nm_manager_activate_connection(
                        self,
                        candidate,
                        NULL,
                        NULL,
                        controller_device,
                        subject,
                        NM_ACTIVATION_TYPE_MANAGED,
                        activation_reason,
                        bind_lifetime_to_profile_visibility
                            ? NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY
                            : NM_ACTIVATION_STATE_FLAG_NONE,
                        error);
                    return controller_ac;
                }
            }

            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_UNKNOWN_CONNECTION,
                        "No compatible connection found.");
            return NULL;
        }

        /* Otherwise, the device is unmanaged, unavailable, or disconnecting */
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                    "Device unmanaged or not available for activation");
    } else if (controller_connection) {
        NMDevice *candidate;

        /* Find a compatible device and activate it using this connection */
        c_list_for_each_entry (candidate, &priv->devices_lst_head, devices_lst) {
            if (candidate == device) {
                /* A device obviously can't be its own controller */
                continue;
            }

            if (!nm_device_check_connection_available(
                    candidate,
                    nm_settings_connection_get_connection(controller_connection),
                    NM_DEVICE_CHECK_CON_AVAILABLE_FOR_USER_REQUEST,
                    NULL,
                    NULL))
                continue;

            if (nm_device_is_real(candidate)
                && !NM_IN_SET(nm_device_get_state(candidate),
                              NM_DEVICE_STATE_DISCONNECTED,
                              NM_DEVICE_STATE_DEACTIVATING))
                continue;

            controller_ac = nm_manager_activate_connection(
                self,
                controller_connection,
                NULL,
                NULL,
                candidate,
                subject,
                NM_ACTIVATION_TYPE_MANAGED,
                activation_reason,
                bind_lifetime_to_profile_visibility
                    ? NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY
                    : NM_ACTIVATION_STATE_FLAG_NONE,
                error);
            return controller_ac;
        }

        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                    "No device available");
    } else
        g_assert_not_reached();

    return NULL;
}

typedef struct {
    NMSettingsConnection *connection;
    NMDevice             *device;
} PortConnectionInfo;

/**
 * find_ports:
 * @manager: #NMManager object
 * @sett_conn: the controller #NMSettingsConnection to find port connections for
 * @device: the controller #NMDevice for the @sett_conn
 * @out_n_ports: on return, the number of ports found
 *
 * Given an #NMSettingsConnection, attempts to find its ports. If @sett_conn is not
 * controller, or has not any ports, this will return %NULL.
 *
 * Returns: an array of #PortConnectionInfo for given controller @sett_conn, or %NULL
 **/
static PortConnectionInfo *
find_ports(NMManager            *manager,
           NMSettingsConnection *sett_conn,
           NMDevice             *device,
           guint                *out_n_ports,
           gboolean              for_user_request)
{
    NMManagerPrivate              *priv            = NM_MANAGER_GET_PRIVATE(manager);
    NMSettingsConnection *const   *all_connections = NULL;
    guint                          n_all_connections;
    guint                          i;
    PortConnectionInfo            *ports   = NULL;
    guint                          n_ports = 0;
    NMSettingConnection           *s_con;
    gs_unref_hashtable GHashTable *devices = NULL;

    nm_assert(out_n_ports);

    s_con = nm_connection_get_setting_connection(nm_settings_connection_get_connection(sett_conn));
    g_return_val_if_fail(s_con, NULL);

    devices = g_hash_table_new(nm_direct_hash, NULL);

    /* Search through all connections, not only inactive ones, because
     * even if a port was already active, it might be deactivated during
     * controller reactivation.
     */
    all_connections =
        nm_settings_get_connections_sorted_by_autoconnect_priority(priv->settings,
                                                                   &n_all_connections);
    for (i = 0; i < n_all_connections; i++) {
        NMSettingsConnection *controller_connection = NULL;
        NMDevice             *controller_device     = NULL;
        NMDevice             *port_device;
        NMSettingsConnection *candidate = all_connections[i];

        find_controller(manager,
                        nm_settings_connection_get_connection(candidate),
                        NULL,
                        &controller_connection,
                        &controller_device,
                        NULL,
                        NULL);
        if ((controller_connection && controller_connection == sett_conn)
            || (controller_device && controller_device == device)) {
            port_device = nm_manager_get_best_device_for_connection(manager,
                                                                    candidate,
                                                                    NULL,
                                                                    for_user_request,
                                                                    devices,
                                                                    NULL);

            if (!ports) {
                /* what we allocate is quite likely much too large. Don't bother, it is only
                 * a temporary buffer. */
                ports = g_new(PortConnectionInfo, n_all_connections);
            }

            nm_assert(n_ports < n_all_connections);
            ports[n_ports++] = (PortConnectionInfo) {
                .connection = candidate,
                .device     = port_device,
            };

            if (port_device)
                g_hash_table_add(devices, port_device);
        }
    }

    *out_n_ports = n_ports;

    /* Warning: returns NULL if n_ports is zero. */
    return ports;
}

static gboolean
should_connect_ports(NMConnection *connection, NMDevice *device)
{
    NMSettingConnection *s_con;
    NMTernary            val;

    s_con = nm_connection_get_setting_connection(connection);
    g_assert(s_con);

    val = nm_setting_connection_get_autoconnect_ports(s_con);
    if (val != NM_TERNARY_DEFAULT)
        goto out;

    val =
        nm_config_data_get_connection_default_int64(NM_CONFIG_GET_DATA,
                                                    NM_CON_DEFAULT("connection.autoconnect-ports"),
                                                    device,
                                                    0,
                                                    1,
                                                    -1);

    /* Trust "connection.autoconnect-ports" first, if set to default fallback to the deprecated term. */
    if (val == NM_TERNARY_DEFAULT)
        val = nm_config_data_get_connection_default_int64(
            NM_CONFIG_GET_DATA,
            NM_CON_DEFAULT("connection.autoconnect-slaves"),
            device,
            0,
            1,
            -1);

out:
    if (val == NM_TERNARY_FALSE)
        return FALSE;
    if (val == NM_TERNARY_TRUE)
        return TRUE;
    return FALSE;
}

static int
compare_ports(gconstpointer a, gconstpointer b)
{
    const PortConnectionInfo *a_info = a;
    const PortConnectionInfo *b_info = b;

    /* Ports without a device at the end */
    if (!a_info->device)
        return 1;
    if (!b_info->device)
        return -1;

    return nm_strcmp0(nm_device_get_iface(a_info->device), nm_device_get_iface(b_info->device));
}

static void
autoconnect_ports(NMManager            *self,
                  NMSettingsConnection *controller_connection,
                  NMDevice             *controller_device,
                  NMAuthSubject        *subject,
                  gboolean              for_user_request)
{
    GError *local_err = NULL;

    if (should_connect_ports(nm_settings_connection_get_connection(controller_connection),
                             controller_device)) {
        gs_free PortConnectionInfo *ports = NULL;
        guint                       i, n_ports = 0;
        gboolean                    bind_lifetime_to_profile_visibility;

        ports =
            find_ports(self, controller_connection, controller_device, &n_ports, for_user_request);
        if (n_ports > 1) {
            qsort(ports, n_ports, sizeof(ports[0]), compare_ports);
        }

        bind_lifetime_to_profile_visibility =
            n_ports > 0
            && NM_FLAGS_HAS(nm_device_get_activation_state_flags(controller_device),
                            NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY);

        for (i = 0; i < n_ports; i++) {
            PortConnectionInfo *port = &ports[i];
            const char         *uuid;

            /* To avoid loops when autoconnecting ports, we propagate
             * the UUID of the initial connection down to ports until
             * the same connection is found.
             */
            uuid = g_object_get_qdata(G_OBJECT(controller_connection), autoconnect_root_quark());
            if (nm_streq0(nm_settings_connection_get_uuid(port->connection), uuid)) {
                _LOGI(LOGD_CORE,
                      "will NOT activate port connection '%s' (%s) as a dependency for controller "
                      "'%s' (%s): "
                      "circular dependency detected",
                      nm_settings_connection_get_id(port->connection),
                      nm_settings_connection_get_uuid(port->connection),
                      nm_settings_connection_get_id(controller_connection),
                      nm_settings_connection_get_uuid(controller_connection));
                continue;
            }

            if (!uuid)
                uuid = nm_settings_connection_get_uuid(controller_connection);
            g_object_set_qdata_full(G_OBJECT(port->connection),
                                    autoconnect_root_quark(),
                                    g_strdup(uuid),
                                    g_free);

            if (!port->device) {
                _LOGD(LOGD_CORE,
                      "will NOT activate port connection '%s' (%s) as a dependency for controller "
                      "'%s' (%s): "
                      "no compatible device found",
                      nm_settings_connection_get_id(port->connection),
                      nm_settings_connection_get_uuid(port->connection),
                      nm_settings_connection_get_id(controller_connection),
                      nm_settings_connection_get_uuid(controller_connection));
                continue;
            }

            _LOGD(
                LOGD_CORE,
                "will activate port connection '%s' (%s) as a dependency for controller '%s' (%s)",
                nm_settings_connection_get_id(port->connection),
                nm_settings_connection_get_uuid(port->connection),
                nm_settings_connection_get_id(controller_connection),
                nm_settings_connection_get_uuid(controller_connection));

            /* Schedule port activation */
            nm_manager_activate_connection(
                self,
                port->connection,
                NULL,
                NULL,
                port->device,
                subject,
                NM_ACTIVATION_TYPE_MANAGED,
                NM_ACTIVATION_REASON_AUTOCONNECT_PORTS,
                bind_lifetime_to_profile_visibility
                    ? NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY
                    : NM_ACTIVATION_STATE_FLAG_NONE,
                &local_err);
            if (local_err) {
                _LOGW(LOGD_CORE, "Port connection activation failed: %s", local_err->message);
                g_clear_error(&local_err);
            }
        }
    }
}

static gboolean
_internal_activate_vpn(NMManager *self, NMActiveConnection *active, GError **error)
{
    nm_assert(NM_IS_VPN_CONNECTION(active));

    nm_dbus_object_export(NM_DBUS_OBJECT(active));
    if (!nm_vpn_manager_activate_connection(NM_MANAGER_GET_PRIVATE(self)->vpn_manager,
                                            NM_VPN_CONNECTION(active),
                                            error)) {
        nm_dbus_object_unexport(NM_DBUS_OBJECT(active));
        return FALSE;
    }

    active_connection_add(self, active);
    return TRUE;
}

/* Traverse the device to disconnected state. This means that the device is ready
 * for connection and will proceed activating if there's an activation request
 * enqueued.
 */
static void
unmanaged_to_disconnected(NMDevice *device)
{
    /* when creating the software device, it can happen that the device is
     * still unmanaged by NM_UNMANAGED_PLATFORM_INIT because we didn't yet
     * get the udev event. At this point, we can no longer delay the activation
     * and force the device to be managed. */
    nm_device_set_unmanaged_by_flags(device,
                                     NM_UNMANAGED_PLATFORM_INIT,
                                     NM_UNMAN_FLAG_OP_SET_MANAGED,
                                     NM_DEVICE_STATE_REASON_USER_REQUESTED);

    nm_device_set_unmanaged_by_flags(device,
                                     NM_UNMANAGED_USER_EXPLICIT,
                                     NM_UNMAN_FLAG_OP_SET_MANAGED,
                                     NM_DEVICE_STATE_REASON_USER_REQUESTED);

    if (!nm_device_get_managed(device, FALSE)) {
        /* the device is still marked as unmanaged. Nothing to do. */
        return;
    }

    if (nm_device_get_state(device) == NM_DEVICE_STATE_UNMANAGED) {
        nm_device_state_changed(device,
                                NM_DEVICE_STATE_UNAVAILABLE,
                                NM_DEVICE_STATE_REASON_USER_REQUESTED);
    }

    if (nm_device_get_state(device) == NM_DEVICE_STATE_UNAVAILABLE
        && nm_device_is_available(device, NM_DEVICE_CHECK_DEV_AVAILABLE_FOR_USER_REQUEST)) {
        nm_device_state_changed(device,
                                NM_DEVICE_STATE_DISCONNECTED,
                                NM_DEVICE_STATE_REASON_USER_REQUESTED);
    }
}

static NMActivationStateFlags
_activation_bind_lifetime_to_profile_visibility(NMAuthSubject *subject)
{
    if (nm_auth_subject_get_subject_type(subject) == NM_AUTH_SUBJECT_TYPE_INTERNAL
        || nm_auth_subject_get_unix_process_uid(subject) == 0) {
        /* internal requests and requests from root are always unbound. */
        return NM_ACTIVATION_STATE_FLAG_NONE;
    }

    /* if the activation was not done by internal decision nor root, there
     * are the following cases:
     *
     * - the connection has "connection.permissions" unset and the profile
     *   is not restricted to a user and commonly always visible. It does
     *   not hurt to bind the lifetime, because we expect the profile to be
     *   visible at the moment. If the profile changes (while still being active),
     *   we want to pick-up changes to the visibility and possibly disconnect.
     *
     * - the connection has "connection.permissions" set, and the current user
     *   is the owner:
     *
     *      - Usually, we would expect that the profile is visible at the moment,
     *        and of course we want to bind the lifetime. The moment the user
     *        logs out, the connection becomes invisible and disconnects.
     *
     *      - the profile at this time could already be invisible (e.g. if the
     *        user didn't create a proper session (sudo) and manually activates
     *        an invisible profile. In this case, we still want to bind the
     *        lifetime, and it will disconnect after the user logs in and logs
     *        out again. NMKeepAlive takes care of that.
     */
    return NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY;
}

/* The parent connection is ready; we can proceed realizing the device and
 * progressing the device to disconencted state.
 */
static void
active_connection_parent_active(NMActiveConnection *active,
                                NMActiveConnection *parent_ac,
                                NMManager          *self)
{
    NMDevice             *device = nm_active_connection_get_device(active);
    GError               *error  = NULL;
    NMSettingsConnection *sett_conn;
    NMDevice             *parent;

    g_signal_handlers_disconnect_by_func(active, G_CALLBACK(active_connection_parent_active), self);

    if (!parent_ac) {
        _LOGW(LOGD_CORE,
              "The parent connection device '%s' depended on disappeared.",
              nm_device_get_iface(device));
        nm_active_connection_set_state_fail(active,
                                            NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_REMOVED,
                                            "parent device disappeared");
        return;
    }

    sett_conn = nm_active_connection_get_settings_connection(active);
    parent    = nm_active_connection_get_device(parent_ac);

    if (!nm_device_create_and_realize(device,
                                      nm_settings_connection_get_connection(sett_conn),
                                      parent,
                                      &error)) {
        _LOGW(LOGD_CORE,
              "Could not realize device '%s': %s",
              nm_device_get_iface(device),
              error->message);
        nm_active_connection_set_state_fail(active,
                                            NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_REALIZE_FAILED,
                                            "failure to realize device");
        return;
    }

    /* We can now proceed to disconnected state so that activation proceeds. */
    unmanaged_to_disconnected(device);
}

static gboolean
_check_autoconnect_port(NMActiveConnection   *active,
                        NMSettingsConnection *controller_connection,
                        NMDevice             *controller_device,
                        NMActiveConnection   *controller_ac)
{
    NMSettingConnection *s_con;
    NMDevice            *device;

    if (nm_active_connection_get_activation_reason(active) != NM_ACTIVATION_REASON_AUTOCONNECT) {
        /* This is an explicit activation. Proceed. */
        return TRUE;
    }

    if (!controller_connection) {
        /* This is not a port. Proceed. */
        return TRUE;
    }

    device = nm_active_connection_get_device(active);

    if (!nm_device_is_real(device)) {
        /* The device is not real. We don't know about the carrier. Proceed. */
        return TRUE;
    }

    if (nm_device_get_ifindex(device) <= 0) {
        /* The device has no ifindex. It has no concept of carrier. Proceed. */
        return TRUE;
    }

    if (nm_device_has_carrier(device)) {
        /* The device has carrier. Proceed. */
        return TRUE;
    }

    s_con =
        nm_settings_connection_get_setting(controller_connection, NM_META_SETTING_TYPE_CONNECTION);

    if (nm_setting_connection_get_autoconnect(s_con)) {
        /* The controller profile has autoconnect enabled. Here we want to honor
         * "ignore-carrier=no", which -- as configuration -- only makes sense for
         * controllers that have autoconnect disable. Proceed. */
        return TRUE;
    }

    if (nm_config_data_get_ignore_carrier_for_port(
            NM_CONFIG_GET_DATA,
            nm_setting_connection_get_interface_name(s_con),
            nm_setting_connection_get_connection_type(s_con))) {
        /* We ignore carrier on the controller (as we would do by default). Proceed. */
        return TRUE;
    }

    return FALSE;
}

static gboolean
_internal_activate_device(NMManager *self, NMActiveConnection *active, GError **error)
{
    NMDevice                *device;
    NMDevice                *controller_device = NULL;
    NMConnection            *applied;
    NMSettingsConnection    *sett_conn;
    NMSettingsConnection    *controller_connection = NULL;
    NMConnection            *existing_connection   = NULL;
    NMActiveConnection      *controller_ac         = NULL;
    NMAuthSubject           *subject;
    GError                  *local = NULL;
    NMConnectionMultiConnect multi_connect;
    const char              *parent_spec;

    g_return_val_if_fail(NM_IS_MANAGER(self), FALSE);
    g_return_val_if_fail(NM_IS_ACTIVE_CONNECTION(active), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    g_assert(NM_IS_VPN_CONNECTION(active) == FALSE);

    device = nm_active_connection_get_device(active);
    g_return_val_if_fail(device != NULL, FALSE);

    sett_conn = nm_active_connection_get_settings_connection(active);
    nm_assert(sett_conn);

    applied = nm_active_connection_get_applied_connection(active);

    /* If the device is active and its connection is not visible to the
     * user that's requesting this new activation, fail, since other users
     * should not be allowed to implicitly deactivate private connections
     * by activating a connection of their own.
     */
    existing_connection = nm_device_get_applied_connection(device);
    subject             = nm_active_connection_get_subject(active);
    if (existing_connection
        && !nm_auth_is_subject_in_acl_set_error(existing_connection,
                                                subject,
                                                NM_MANAGER_ERROR,
                                                NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                error)) {
        g_prefix_error(error, "Private connection already active on the device: ");
        return FALSE;
    }

    /* Final connection must be available on device */
    if (!nm_device_check_connection_available(device,
                                              applied,
                                              NM_DEVICE_CHECK_CON_AVAILABLE_FOR_USER_REQUEST,
                                              NULL,
                                              &local)) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_UNKNOWN_CONNECTION,
                    "Connection '%s' is not available on device %s because %s",
                    nm_settings_connection_get_id(sett_conn),
                    nm_device_get_iface(device),
                    local->message);
        g_error_free(local);
        return FALSE;
    }

    if (nm_active_connection_get_activation_type(active) == NM_ACTIVATION_TYPE_MANAGED)
        nm_device_managed_type_set(device, NM_DEVICE_MANAGED_TYPE_FULL);

    /* Try to find the controller connection/device if the connection has a dependency */
    if (!find_controller(self,
                         applied,
                         device,
                         &controller_connection,
                         &controller_device,
                         &controller_ac,
                         error)) {
        g_prefix_error(error,
                       "Can not find a controller for %s: ",
                       nm_settings_connection_get_id(sett_conn));
        return FALSE;
    }

    /* FIXME: in _check_autoconnect_port() we decide on whether to abort to
     * activation based on the device's carrier state (and the controller's
     * ignore-carrier setting).
     *
     * At this stage, we might be activating a VLAN attached to a bond
     * interface. But the VLAN interface may not be created yet, and not have a
     * carrier state yet.
     *
     * We could fix this, by checking again (or exclusively) before attaching
     * the port to the controller, whether the conditions from
     * _check_autoconnect_port() hold.  And if they don't, abort activation at
     * a later stage.
     *
     * The problem is, that we already start activating the controller at this
     * point. Hence, aborting later is not good. What instead maybe should be
     * done, is that port profiles don't start activating the controller
     * before they have layer 2 set up.
     */

    if (!_check_autoconnect_port(active, controller_connection, controller_device, controller_ac)) {
        /* Usually, port and controller devices can (auto)connect without carrier. However,
         * the controller has "ignore-carrier=no" configured. If the port autoconnects,
         * has no carrier and the controller has ignore-carrier=no, then autoconnect
         * is going to fail. */
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                    "port has no carrier and controller does not ignore carrier");
        return FALSE;
    }

    /* Create any backing resources the device needs */
    if (!nm_device_is_real(device)) {
        NMDevice *parent;

        parent = find_parent_device_for_connection(self,
                                                   nm_settings_connection_get_connection(sett_conn),
                                                   NULL,
                                                   &parent_spec);

        if (parent_spec && !parent) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                        "parent device '%s' not found",
                        parent_spec);
            return FALSE;
        }

        if (parent && !nm_device_is_real(parent)) {
            NMSettingsConnection *parent_con;
            NMActiveConnection   *parent_ac;

            parent_con = nm_device_get_best_connection(parent, NULL, error);
            if (!parent_con) {
                g_prefix_error(error, "%s failed to create parent: ", nm_device_get_iface(device));
                return FALSE;
            }

            if (nm_active_connection_get_activation_reason(active)
                    == NM_ACTIVATION_REASON_AUTOCONNECT
                && NM_FLAGS_HAS(nm_settings_connection_autoconnect_blocked_reason_get(parent_con),
                                NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_USER_REQUEST)) {
                g_set_error(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                            "the parent connection of %s cannot autoactivate because it is blocked "
                            "due to user request",
                            nm_device_get_iface(device));
                return FALSE;
            }

            parent_ac = nm_manager_activate_connection(
                self,
                parent_con,
                NULL,
                NULL,
                parent,
                subject,
                NM_ACTIVATION_TYPE_MANAGED,
                nm_active_connection_get_activation_reason(active),
                nm_active_connection_get_state_flags(active)
                    & NM_ACTIVATION_STATE_FLAG_LIFETIME_BOUND_TO_PROFILE_VISIBILITY,
                error);
            if (!parent_ac) {
                g_prefix_error(error,
                               "%s failed to activate parent: ",
                               nm_device_get_iface(device));
                return FALSE;
            }

            /* We can't realize now; defer until the parent device is ready. */
            g_signal_connect(active,
                             NM_ACTIVE_CONNECTION_PARENT_ACTIVE,
                             G_CALLBACK(active_connection_parent_active),
                             self);
            nm_active_connection_set_parent(active, parent_ac);
        } else {
            /* We can realize now; no need to wait for a parent device. */
            if (!nm_device_create_and_realize(device,
                                              nm_settings_connection_get_connection(sett_conn),
                                              parent,
                                              error)) {
                g_prefix_error(error,
                               "%s failed to create resources: ",
                               nm_device_get_iface(device));
                return FALSE;
            }
        }
    }

    /* Ensure there's a controller active connection the new connection we're
     * activating can depend on.
     */
    if (controller_connection || controller_device) {
        if (controller_connection) {
            _LOGD(LOGD_CORE,
                  "Activation of '%s' requires controller connection '%s'",
                  nm_settings_connection_get_id(sett_conn),
                  nm_settings_connection_get_id(controller_connection));
        }
        if (controller_device) {
            _LOGD(LOGD_CORE,
                  "Activation of '%s' requires controller device '%s'",
                  nm_settings_connection_get_id(sett_conn),
                  nm_device_get_ip_iface(controller_device));
        }

        /* Ensure eg bond port and the candidate controller is a bond controller */
        if (controller_connection
            && !is_compatible_with_port(
                nm_settings_connection_get_connection(controller_connection),
                applied)) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                        "The controller connection '%s' is not compatible with '%s'",
                        nm_settings_connection_get_id(controller_connection),
                        nm_settings_connection_get_id(sett_conn));
            return FALSE;
        }

        if (!controller_ac) {
            controller_ac = ensure_controller_active_connection(
                self,
                nm_active_connection_get_subject(active),
                applied,
                device,
                controller_connection,
                controller_device,
                nm_active_connection_get_activation_reason(active),
                error);
            if (!controller_ac) {
                if (controller_device) {
                    g_prefix_error(error,
                                   "Controller device '%s' can't be activated: ",
                                   nm_device_get_ip_iface(controller_device));
                } else {
                    g_prefix_error(error,
                                   "Controller connection '%s' can't be activated: ",
                                   nm_settings_connection_get_id(controller_connection));
                }
                return FALSE;
            }
        }

        /* Now that we're activating a port for that controller, make sure the controller just
         * decides to go unmanaged while we're activating (perhaps because other ports
         * go away leaving him with no kids).
         */
        if (controller_device) {
            nm_device_set_unmanaged_by_flags(controller_device,
                                             NM_UNMANAGED_EXTERNAL_DOWN,
                                             NM_UNMAN_FLAG_OP_FORGET,
                                             NM_DEVICE_STATE_REASON_USER_REQUESTED);
        }

        /* If controller NMActiveConnection is deactivating, we should wait on
         * controller's NMDevice to have new NMActiveConnection after
         * controller device state change to between NM_DEVICE_STATE_PREPARE and
         * NM_DEVICE_STATE_ACTIVATED.
         */
        if ((nm_active_connection_get_state(controller_ac)
             >= NM_ACTIVE_CONNECTION_STATE_DEACTIVATING)
            && controller_device
            && (nm_device_get_state_reason(controller_device)
                == NM_DEVICE_STATE_REASON_NEW_ACTIVATION)) {
            nm_active_connection_set_controller_dev(active, controller_device);
            _LOGD(LOGD_CORE,
                  "Activation of '%s'(%s) depends on controller device %p %s",
                  nm_settings_connection_get_id(sett_conn),
                  nm_settings_connection_get_connection_type(sett_conn),
                  controller_device,
                  nm_dbus_object_get_path(NM_DBUS_OBJECT(controller_device)) ?: "");
        } else {
            nm_active_connection_set_controller(active, controller_ac);
            _LOGD(LOGD_CORE,
                  "Activation of '%s'(%s) depends on active connection %p %s",
                  nm_settings_connection_get_id(sett_conn),
                  nm_settings_connection_get_connection_type(sett_conn),
                  controller_ac,
                  nm_dbus_object_get_path(NM_DBUS_OBJECT(controller_ac)) ?: "");
        }
    }

    /* Check ports for controller connection and possibly activate them */
    autoconnect_ports(self,
                      sett_conn,
                      device,
                      nm_active_connection_get_subject(active),
                      nm_active_connection_get_activation_reason(active)
                          == NM_ACTIVATION_REASON_USER_REQUEST);

    multi_connect =
        _nm_connection_get_multi_connect(nm_settings_connection_get_connection(sett_conn));
    if (multi_connect == NM_CONNECTION_MULTI_CONNECT_MULTIPLE
        || (multi_connect == NM_CONNECTION_MULTI_CONNECT_MANUAL_MULTIPLE
            && NM_IN_SET(nm_active_connection_get_activation_reason(active),
                         NM_ACTIVATION_REASON_ASSUME,
                         NM_ACTIVATION_REASON_AUTOCONNECT_PORTS,
                         NM_ACTIVATION_REASON_USER_REQUEST))) {
        /* the profile can be activated multiple times. Proceed. */
    } else {
        gs_unref_ptrarray GPtrArray *all_ac_arr = NULL;
        NMActiveConnection          *ac;
        guint                        i, n_all;

        /* Disconnect the connection if already connected or queued for activation.
         * The connection cannot be active multiple times (at the same time).  */
        ac = active_connection_find(self,
                                    sett_conn,
                                    NULL,
                                    NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
                                    FALSE,
                                    &all_ac_arr);
        if (ac) {
            n_all = all_ac_arr ? all_ac_arr->len : ((guint) 1);
            for (i = 0; i < n_all; i++) {
                nm_device_disconnect_active_connection(all_ac_arr ? all_ac_arr->pdata[i] : ac,
                                                       NM_DEVICE_STATE_REASON_NEW_ACTIVATION,
                                                       NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN);
            }
        }
    }

    /* If the device is there, we can ready it for the activation. */
    if (nm_device_is_real(device)) {
        unmanaged_to_disconnected(device);

        if (!nm_device_get_managed(device, FALSE)) {
            /* Unexpectedly, the device is still unmanaged. That can happen for example,
             * if the device is forcibly unmanaged due to NM_UNMANAGED_USER_SETTINGS. */
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                                "Activation failed because the device is unmanaged");
            return FALSE;
        }
    }

    /* Export the new ActiveConnection to clients and start it on the device */
    active_connection_add(self, active);
    nm_device_queue_activation(device, NM_ACT_REQUEST(active));
    return TRUE;
}

static gboolean
_internal_activate_generic(NMManager *self, NMActiveConnection *active, GError **error)
{
    NMManagerPrivate *priv    = NM_MANAGER_GET_PRIVATE(self);
    gboolean          success = FALSE;

    /* Ensure activation request is still valid, eg that its device hasn't gone
     * away or that some other dependency has not failed.
     */
    if (nm_active_connection_get_state(active) >= NM_ACTIVE_CONNECTION_STATE_DEACTIVATING) {
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_DEPENDENCY_FAILED,
                            "Activation failed because dependencies failed.");
        return FALSE;
    }

    if (NM_IS_VPN_CONNECTION(active))
        success = _internal_activate_vpn(self, active, error);
    else
        success = _internal_activate_device(self, active, error);

    if (success) {
        /* Force an update of the Manager's activating-connection property.
         * The device changes state before the AC gets exported, which causes
         * the manager's 'activating-connection' property to be NULL since the
         * AC only gets a D-Bus path when it's exported.  So now that the AC
         * is exported, make sure the manager's activating-connection property
         * is up-to-date.
         */
        policy_activating_ac_changed(G_OBJECT(priv->policy), NULL, self);
    }

    return success;
}

static NMActiveConnection *
_new_active_connection(NMManager             *self,
                       gboolean               is_vpn,
                       NMSettingsConnection  *sett_conn,
                       NMConnection          *incompl_conn,
                       NMConnection          *applied,
                       const char            *specific_object,
                       NMDevice              *device,
                       NMAuthSubject         *subject,
                       NMActivationType       activation_type,
                       NMActivationReason     activation_reason,
                       NMActivationStateFlags initial_state_flags,
                       GError               **error)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *parent_device;

    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!incompl_conn || NM_IS_CONNECTION(incompl_conn));
    nm_assert((!incompl_conn) ^ (!sett_conn));
    nm_assert(NM_IS_AUTH_SUBJECT(subject));
    nm_assert(is_vpn
              == _connection_is_vpn(sett_conn ? nm_settings_connection_get_connection(sett_conn)
                                              : incompl_conn));
    nm_assert(is_vpn || NM_IS_DEVICE(device));
    nm_assert(!nm_streq0(specific_object, "/"));
    nm_assert(!applied || NM_IS_CONNECTION(applied));
    nm_assert(!is_vpn || !applied);

    if (is_vpn) {
        NMActiveConnection *parent;

        /* FIXME: for VPN connections, we don't allow re-activating an
         * already active connection. It's a bug, and should be fixed together
         * when reworking VPN handling. */
        if (active_connection_find_by_connection(self,
                                                 sett_conn,
                                                 incompl_conn,
                                                 NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
                                                 NULL)) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_CONNECTION_ALREADY_ACTIVE,
                        "Connection '%s' is already active",
                        sett_conn ? nm_settings_connection_get_id(sett_conn)
                                  : nm_connection_get_id(incompl_conn));
            return NULL;
        }

        if (activation_type != NM_ACTIVATION_TYPE_MANAGED)
            g_return_val_if_reached(NULL);

        if (specific_object) {
            /* Find the specific connection the client requested we use */
            parent = active_connection_get_by_path(self, specific_object);
            if (!parent) {
                g_set_error_literal(error,
                                    NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_CONNECTION_NOT_ACTIVE,
                                    "Base connection for VPN connection not active.");
                return NULL;
            }
        } else
            parent = priv->primary_connection;

        if (!parent) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_UNKNOWN_CONNECTION,
                                "Could not find source connection.");
            return NULL;
        }

        parent_device = nm_active_connection_get_device(parent);
        if (!parent_device) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                                "Source connection had no active device");
            return NULL;
        }

        if (device && device != parent_device) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                                "The device doesn't match the active connection.");
            return NULL;
        }

        return (NMActiveConnection *) nm_vpn_connection_new(
            sett_conn,
            parent_device,
            nm_dbus_object_get_path(NM_DBUS_OBJECT(parent)),
            activation_reason,
            initial_state_flags,
            subject);
    }

    return (NMActiveConnection *) nm_act_request_new(sett_conn,
                                                     applied,
                                                     specific_object,
                                                     subject,
                                                     activation_type,
                                                     activation_reason,
                                                     initial_state_flags,
                                                     device);
}

static void
_internal_activation_auth_done(NMManager          *self,
                               NMActiveConnection *active,
                               gboolean            success,
                               const char         *error_desc)
{
    NMManagerPrivate     *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection   *ac;
    gs_free_error GError *error = NULL;

    nm_assert(NM_IS_ACTIVE_CONNECTION(active));

    if (!success)
        goto fail;

    /* Don't continue with an autoconnect-activation if a more important activation
     * already exists.
     * We also check this earlier, but there we may fail to detect a duplicate
     * if the existing active connection was undergoing authorization.
     */
    if (NM_IN_SET(nm_active_connection_get_activation_reason(active),
                  NM_ACTIVATION_REASON_EXTERNAL,
                  NM_ACTIVATION_REASON_ASSUME,
                  NM_ACTIVATION_REASON_AUTOCONNECT)) {
        c_list_for_each_entry (ac, &priv->active_connections_lst_head, active_connections_lst) {
            if (nm_active_connection_get_device(ac) == nm_active_connection_get_device(active)
                && nm_active_connection_get_settings_connection(ac)
                       == nm_active_connection_get_settings_connection(active)
                && nm_active_connection_get_state(ac) <= NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
                g_set_error(&error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_CONNECTION_ALREADY_ACTIVE,
                            "Connection '%s' is already active",
                            nm_active_connection_get_settings_connection_id(active));
                goto fail;
            }
        }
    }

    if (_internal_activate_generic(self, active, &error))
        return;

fail:
    _delete_volatile_connection_do(self, nm_active_connection_get_settings_connection(active));
    nm_assert(error_desc || error);
    nm_active_connection_set_state_fail(active,
                                        NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN,
                                        error_desc ?: error->message);
}

void
nm_manager_deactivate_ac(NMManager *self, NMSettingsConnection *connection)
{
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *ac;
    const CList        *tmp_list, *tmp_safe;
    GError             *error = NULL;
    AsyncOpData        *async_op_data;
    AsyncOpData        *async_op_data_safe;

    nm_assert(NM_IS_SETTINGS_CONNECTION(connection));

    nm_manager_for_each_active_connection_safe (self, ac, tmp_list, tmp_safe) {
        if (nm_active_connection_get_settings_connection(ac) == connection
            && (nm_active_connection_get_state(ac) <= NM_ACTIVE_CONNECTION_STATE_ACTIVATED)) {
            if (!nm_manager_deactivate_connection(self,
                                                  ac,
                                                  NM_DEVICE_STATE_REASON_CONNECTION_REMOVED,
                                                  &error)) {
                _LOGW(LOGD_DEVICE,
                      "connection '%s' disappeared, but error deactivating it: (%d) %s",
                      nm_settings_connection_get_id(connection),
                      error ? error->code : -1,
                      error ? error->message : "(unknown)");
                g_clear_error(&error);
            }
        }
    }

    c_list_for_each_entry_safe (async_op_data,
                                async_op_data_safe,
                                &priv->async_op_lst_head,
                                async_op_lst) {
        if (!NM_IN_SET(async_op_data->async_op_type,
                       ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_INTERNAL,
                       ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_USER,
                       ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE,
                       ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE2))
            continue;

        ac = async_op_data->ac_auth.active;
        if (nm_active_connection_get_settings_connection(ac) == connection) {
            nm_active_connection_set_state(ac,
                                           NM_ACTIVE_CONNECTION_STATE_DEACTIVATED,
                                           NM_ACTIVE_CONNECTION_STATE_REASON_CONNECTION_REMOVED);
        }
    }
}

/**
 * nm_manager_activate_connection():
 * @self: the #NMManager
 * @sett_conn: the #NMSettingsConnection to activate on @device
 * @applied: (nullable): the applied connection to activate on @device
 * @specific_object: the specific object path, if any, for the activation
 * @device: the #NMDevice to activate @sett_conn on. Can be %NULL for VPNs.
 * @subject: the subject which requested activation
 * @activation_type: whether to assume the connection. That is, take over gracefully,
 *   non-destructible.
 * @activation_reason: the reason for activation
 * @initial_state_flags: the initial state flags for the activation.
 * @error: return location for an error
 *
 * Begins a new internally-initiated activation of @sett_conn on @device.
 * @subject should be the subject of the activation that triggered this
 * one, or if this is an autoconnect request, a new internal subject.
 * The returned #NMActiveConnection is owned by the Manager and should be
 * referenced by the caller if the caller continues to use it. If @applied
 * is supplied, it shall not be modified by the caller afterwards.
 *
 * Returns: (transfer none): the new #NMActiveConnection that tracks
 * activation of @sett_conn on @device
 */
NMActiveConnection *
nm_manager_activate_connection(NMManager             *self,
                               NMSettingsConnection  *sett_conn,
                               NMConnection          *applied,
                               const char            *specific_object,
                               NMDevice              *device,
                               NMAuthSubject         *subject,
                               NMActivationType       activation_type,
                               NMActivationReason     activation_reason,
                               NMActivationStateFlags initial_state_flags,
                               GError               **error)
{
    NMManagerPrivate   *priv;
    NMActiveConnection *active;
    AsyncOpData        *async_op_data;
    gboolean            is_vpn;

    g_return_val_if_fail(NM_IS_MANAGER(self), NULL);
    g_return_val_if_fail(NM_IS_SETTINGS_CONNECTION(sett_conn), NULL);
    is_vpn = _connection_is_vpn(nm_settings_connection_get_connection(sett_conn));
    g_return_val_if_fail(is_vpn || NM_IS_DEVICE(device), NULL);
    g_return_val_if_fail(!error || !*error, NULL);
    nm_assert(!nm_streq0(specific_object, "/"));

    priv = NM_MANAGER_GET_PRIVATE(self);

    if (!nm_auth_is_subject_in_acl_set_error(nm_settings_connection_get_connection(sett_conn),
                                             subject,
                                             NM_MANAGER_ERROR,
                                             NM_MANAGER_ERROR_PERMISSION_DENIED,
                                             error))
        return NULL;

    /* Look for a active connection that's equivalent and is already pending authorization
     * and eventual activation. This is used to de-duplicate concurrent activations which would
     * otherwise race and cause the device to disconnect and reconnect repeatedly.
     * In particular, this allows the controller and multiple ports to concurrently auto-activate
     * while all the ports would use the same active-connection. */
    c_list_for_each_entry (async_op_data, &priv->async_op_lst_head, async_op_lst) {
        if (async_op_data->async_op_type != ASYNC_OP_TYPE_AC_AUTH_ACTIVATE_INTERNAL)
            continue;

        active = async_op_data->ac_auth.active;
        if (sett_conn == nm_active_connection_get_settings_connection(active)
            && nm_streq0(nm_active_connection_get_specific_object(active), specific_object)
            && (!device || nm_active_connection_get_device(active) == device)
            && nm_auth_subject_get_subject_type(nm_active_connection_get_subject(active))
                   == NM_AUTH_SUBJECT_TYPE_INTERNAL
            && nm_auth_subject_get_subject_type(subject) == NM_AUTH_SUBJECT_TYPE_INTERNAL
            && nm_active_connection_get_activation_reason(active) == activation_reason)
            return active;
    }

    active = _new_active_connection(self,
                                    is_vpn,
                                    sett_conn,
                                    NULL,
                                    applied,
                                    specific_object,
                                    device,
                                    subject,
                                    activation_type,
                                    activation_reason,
                                    initial_state_flags,
                                    error);
    if (!active)
        return NULL;

    nm_active_connection_authorize(active,
                                   NULL,
                                   _async_op_complete_ac_auth_cb,
                                   _async_op_data_new_authorize_activate_internal(self, active));
    return active;
}

/**
 * validate_activation_request:
 * @self: the #NMManager
 * @context: the D-Bus context of the requestor
 * @sett_conn: the #NMSettingsConnection to be activated, or %NULL if there
 *   is only a partial activation.
 * @connection: the partial #NMConnection to be activated (if @sett_conn is unspecified)
 * @error: location to store an error on failure
 *
 * Performs basic permission validation on an activation request: Ensures that
 * the requestor is a valid Unix process and is not disallowed in @connection
 * permissions.
 *
 * Returns: on success, the #NMAuthSubject representing the requestor, or
 *   %NULL on error
 */
static NMAuthSubject *
validate_activation_request(NMManager             *self,
                            GDBusMethodInvocation *context,
                            NMSettingsConnection  *sett_conn,
                            NMConnection          *connection,
                            GError               **error)
{
    gs_unref_object NMAuthSubject *subject = NULL;

    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!connection || NM_IS_CONNECTION(connection));
    nm_assert(sett_conn || connection);
    nm_assert(!connection || !sett_conn
              || connection == nm_settings_connection_get_connection(sett_conn));

    if (!connection)
        connection = nm_settings_connection_get_connection(sett_conn);

    /* Validate the caller */
    subject = nm_dbus_manager_new_auth_subject_from_context(context);
    if (!subject) {
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_PERMISSION_DENIED,
                            NM_UTILS_ERROR_MSG_REQ_UID_UKNOWN);
        return NULL;
    }

    if (!nm_auth_is_subject_in_acl_set_error(connection,
                                             subject,
                                             NM_MANAGER_ERROR,
                                             NM_MANAGER_ERROR_PERMISSION_DENIED,
                                             error))
        return NULL;
    return g_steal_pointer(&subject);
}

/**
 * find_device_for_activation:
 * @self: the #NMManager
 * @sett_conn: the #NMSettingsConnection to be activated, or %NULL if there
 *   is only a partial activation.
 * @connection: the partial #NMConnection to be activated (if @sett_conn is unspecified)
 * @device_path: the object path of the device to be activated, or NULL
 * @out_device: on successful return, the #NMDevice to be activated with @connection
 *   The caller may pass in a device which shortcuts the lookup by path.
 *   In this case, the passed in device must have the matching @device_path
 *   already.
 * @out_is_vpn: on successful return, %TRUE if @connection is a VPN connection
 * @error: location to store an error on failure
 *
 * Looks up a device that can activate @connection, or indicates the
 * connection is a VPN connection that does not require a device.
 *
 * Returns: %TRUE if the device could be find or connection doesn't
 *   need one, %FALSE otherwise
 */
static gboolean
find_device_for_activation(NMManager            *self,
                           NMSettingsConnection *sett_conn,
                           NMConnection         *connection,
                           const char           *device_path,
                           NMDevice            **out_device,
                           gboolean             *out_is_vpn,
                           GError              **error)
{
    gboolean  is_vpn = FALSE;
    NMDevice *device = NULL;

    nm_assert(!sett_conn || NM_IS_SETTINGS_CONNECTION(sett_conn));
    nm_assert(!connection || NM_IS_CONNECTION(connection));
    nm_assert(sett_conn || connection);
    nm_assert(!connection || !sett_conn
              || connection == nm_settings_connection_get_connection(sett_conn));
    nm_assert(out_device);
    nm_assert(out_is_vpn);

    if (!connection)
        connection = nm_settings_connection_get_connection(sett_conn);

    is_vpn = _connection_is_vpn(connection);

    if (*out_device) {
        device = *out_device;
        nm_assert(NM_IS_DEVICE(device));
        nm_assert(device_path);
        nm_assert(nm_streq0(device_path, nm_dbus_object_get_path(NM_DBUS_OBJECT(device))));
        nm_assert(device == nm_manager_get_device_by_path(self, device_path));
    } else if (device_path) {
        device = nm_manager_get_device_by_path(self, device_path);
        if (!device) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                                "Device not found");
            return FALSE;
        }
    } else if (!is_vpn) {
        gs_free_error GError *local = NULL;

        device = nm_manager_get_best_device_for_connection(self,
                                                           sett_conn,
                                                           connection,
                                                           TRUE,
                                                           NULL,
                                                           &local);
        if (!device) {
            gs_free char *iface = NULL;

            /* VPN and software-device connections don't need a device yet,
             * but non-virtual connections do ... */
            if (!nm_connection_is_virtual(connection)) {
                g_set_error(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                            "No suitable device found for this connection (%s).",
                            local->message);
                return FALSE;
            }

            /* Look for an existing device with the connection's interface name */
            iface = nm_manager_get_connection_iface(self, connection, NULL, NULL, error);
            if (!iface)
                return FALSE;

            device = find_device_by_iface(self, iface, connection, NULL, NULL);
        }
    }

    *out_device = device;
    *out_is_vpn = is_vpn;

    return TRUE;
}

/*****************************************************************************/

static void
_activation_auth_done(NMManager             *self,
                      NMActiveConnection    *active,
                      GDBusMethodInvocation *invocation,
                      gboolean               success,
                      const char            *error_desc)
{
    GError               *error = NULL;
    NMAuthSubject        *subject;
    NMSettingsConnection *connection;

    subject    = nm_active_connection_get_subject(active);
    connection = nm_active_connection_get_settings_connection(active);

    if (!success) {
        error =
            g_error_new_literal(NM_MANAGER_ERROR, NM_MANAGER_ERROR_PERMISSION_DENIED, error_desc);
        goto fail;
    }

    if (!_internal_activate_generic(self, active, &error))
        goto fail;

    nm_settings_connection_autoconnect_blocked_reason_set(
        connection,
        NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_USER_REQUEST,
        FALSE);
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(o)", nm_dbus_object_get_path(NM_DBUS_OBJECT(active))));
    nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ACTIVATE, connection, TRUE, NULL, subject, NULL);
    return;

fail:
    _delete_volatile_connection_do(self, connection);

    nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ACTIVATE,
                               connection,
                               FALSE,
                               NULL,
                               subject,
                               error->message);
    nm_active_connection_set_state_fail(active,
                                        NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN,
                                        error->message);

    g_dbus_method_invocation_take_error(invocation, error);
}

static void
impl_manager_activate_connection(NMDBusObject                      *obj,
                                 const NMDBusInterfaceInfoExtended *interface_info,
                                 const NMDBusMethodInfoExtended    *method_info,
                                 GDBusConnection                   *dbus_connection,
                                 const char                        *sender,
                                 GDBusMethodInvocation             *invocation,
                                 GVariant                          *parameters)
{
    NMManager                          *self      = NM_MANAGER(obj);
    NMManagerPrivate                   *priv      = NM_MANAGER_GET_PRIVATE(self);
    gs_unref_object NMActiveConnection *active    = NULL;
    gs_unref_object NMAuthSubject      *subject   = NULL;
    NMSettingsConnection               *sett_conn = NULL;
    NMDevice                           *device    = NULL;
    gboolean                            is_vpn    = FALSE;
    GError                             *error     = NULL;
    const char                         *connection_path;
    const char                         *device_path;
    const char                         *specific_object_path;

    g_variant_get(parameters, "(&o&o&o)", &connection_path, &device_path, &specific_object_path);

    connection_path      = nm_dbus_path_not_empty(connection_path);
    specific_object_path = nm_dbus_path_not_empty(specific_object_path);
    device_path          = nm_dbus_path_not_empty(device_path);

    /* If the connection path is given and valid, that connection is activated.
     * Otherwise, the "best" connection for the device is chosen and activated,
     * regardless of whether that connection is autoconnect-enabled or not
     * (since this is an explicit request, not an auto-activation request).
     */
    if (connection_path) {
        sett_conn = nm_settings_get_connection_by_path(priv->settings, connection_path);
        if (!sett_conn) {
            error = g_error_new_literal(NM_MANAGER_ERROR,
                                        NM_MANAGER_ERROR_UNKNOWN_CONNECTION,
                                        "Connection could not be found.");
            goto error;
        }
    } else {
        /* If no connection is given, find a suitable connection for the given device path */
        if (!device_path) {
            error = g_error_new_literal(
                NM_MANAGER_ERROR,
                NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                "Only devices may be activated without a specifying a connection");
            goto error;
        }
        device = nm_manager_get_device_by_path(self, device_path);
        if (!device) {
            error = g_error_new(NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                                "Can not activate an unknown device '%s'",
                                device_path);
            goto error;
        }

        sett_conn = nm_device_get_best_connection(device, specific_object_path, &error);
        if (!sett_conn)
            goto error;
    }

    subject = validate_activation_request(self, invocation, sett_conn, NULL, &error);
    if (!subject)
        goto error;

    if (!find_device_for_activation(self, sett_conn, NULL, device_path, &device, &is_vpn, &error))
        goto error;

    if (!device && !is_vpn) {
        g_set_error_literal(&error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_UNKNOWN_DEVICE,
                            "Failed to find a compatible device for this connection");
        goto error;
    }

    active = _new_active_connection(self,
                                    is_vpn,
                                    sett_conn,
                                    NULL,
                                    NULL,
                                    specific_object_path,
                                    device,
                                    subject,
                                    NM_ACTIVATION_TYPE_MANAGED,
                                    NM_ACTIVATION_REASON_USER_REQUEST,
                                    _activation_bind_lifetime_to_profile_visibility(subject),
                                    &error);
    if (!active)
        goto error;

    nm_active_connection_authorize(
        active,
        NULL,
        _async_op_complete_ac_auth_cb,
        _async_op_data_new_ac_auth_activate_user(self, active, invocation));

    /* we passed the pointer on to _async_op_data_new_ac_auth_activate_user() */
    g_steal_pointer(&active);

    return;

error:
    if (sett_conn) {
        nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ACTIVATE,
                                   sett_conn,
                                   FALSE,
                                   NULL,
                                   subject,
                                   error->message);
    }
    g_dbus_method_invocation_take_error(invocation, error);
}

/*****************************************************************************/

static void
activation_add_done(NMSettings            *settings,
                    NMSettingsConnection  *new_connection,
                    GError                *error,
                    GDBusMethodInvocation *context,
                    NMAuthSubject         *subject,
                    gpointer               user_data)
{
    NMManager                          *self;
    gs_unref_object NMActiveConnection *active = NULL;
    gs_free_error GError               *local  = NULL;
    gpointer                            async_op_type_ptr;
    AsyncOpType                         async_op_type;
    GVariant                           *result_floating;

    nm_utils_user_data_unpack(user_data, &self, &active, &async_op_type_ptr);
    async_op_type = GPOINTER_TO_INT(async_op_type_ptr);

    if (error)
        goto fail;

    nm_active_connection_set_settings_connection(active, new_connection);

    if (!_internal_activate_generic(self, active, &local))
        goto fail;

    if (async_op_type == ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE) {
        result_floating = g_variant_new("(oo)",
                                        nm_dbus_object_get_path(NM_DBUS_OBJECT(new_connection)),
                                        nm_dbus_object_get_path(NM_DBUS_OBJECT(active)));
    } else {
        result_floating = g_variant_new("(oo@a{sv})",
                                        nm_dbus_object_get_path(NM_DBUS_OBJECT(new_connection)),
                                        nm_dbus_object_get_path(NM_DBUS_OBJECT(active)),
                                        nm_g_variant_singleton_aLsvI());
    }
    g_dbus_method_invocation_return_value(context, result_floating);

    nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ADD_ACTIVATE,
                               nm_active_connection_get_settings_connection(active),
                               TRUE,
                               NULL,
                               nm_active_connection_get_subject(active),
                               NULL);
    return;

fail:
    if (local) {
        nm_assert(!error);
        error = local;
    } else
        nm_assert(error);

    nm_active_connection_set_state_fail(active,
                                        NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN,
                                        error->message);
    if (new_connection)
        nm_settings_connection_delete(new_connection, FALSE);
    g_dbus_method_invocation_return_gerror(context, error);
    nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ADD_ACTIVATE,
                               NULL,
                               FALSE,
                               NULL,
                               nm_active_connection_get_subject(active),
                               error->message);
}

static void
_add_and_activate_auth_done(NMManager                      *self,
                            AsyncOpType                     async_op_type,
                            NMActiveConnection             *active,
                            NMConnection                   *connection,
                            GDBusMethodInvocation          *invocation,
                            NMSettingsConnectionPersistMode persist_mode,
                            gboolean                        is_volatile,
                            gboolean                        success,
                            const char                     *error_desc)
{
    NMManagerPrivate *priv;
    NMDevice         *device;
    GError           *error = NULL;

    if (!success) {
        error =
            g_error_new_literal(NM_MANAGER_ERROR, NM_MANAGER_ERROR_PERMISSION_DENIED, error_desc);
        nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ADD_ACTIVATE,
                                   NULL,
                                   FALSE,
                                   NULL,
                                   nm_active_connection_get_subject(active),
                                   error->message);
        g_dbus_method_invocation_take_error(invocation, error);

        device = nm_active_connection_get_device(active);
        if (device && nm_device_is_software(device)
            && !nm_device_managed_type_is_external_or_assume(device)
            && _check_remove_dev_on_link_deleted(self, device)) {
            remove_device(self, device, FALSE);
        }

        return;
    }

    priv = NM_MANAGER_GET_PRIVATE(self);

    /* FIXME(shutdown): nm_settings_add_connection_dbus() cannot be cancelled. It should be made
     * cancellable and tracked via AsyncOpData to be able to do a clean
     * shutdown. */
    nm_settings_add_connection_dbus(
        priv->settings,
        NULL,
        connection,
        persist_mode,
        NM_SETTINGS_CONNECTION_ADD_REASON_NONE,
        (is_volatile ? NM_SETTINGS_CONNECTION_INT_FLAGS_VOLATILE
                     : NM_SETTINGS_CONNECTION_INT_FLAGS_NONE),
        nm_active_connection_get_subject(active),
        invocation,
        activation_add_done,
        nm_utils_user_data_pack(self, g_object_ref(active), GINT_TO_POINTER(async_op_type)));
}

static void
impl_manager_add_and_activate_connection(NMDBusObject                      *obj,
                                         const NMDBusInterfaceInfoExtended *interface_info,
                                         const NMDBusMethodInfoExtended    *method_info,
                                         GDBusConnection                   *dbus_connection,
                                         const char                        *sender,
                                         GDBusMethodInvocation             *invocation,
                                         GVariant                          *parameters)
{
    NMManager                          *self         = NM_MANAGER(obj);
    NMManagerPrivate                   *priv         = NM_MANAGER_GET_PRIVATE(self);
    gs_unref_object NMConnection       *incompl_conn = NULL;
    gs_unref_object NMActiveConnection *active       = NULL;
    gs_unref_object NMAuthSubject      *subject      = NULL;
    GError                             *error        = NULL;
    NMDevice                           *device       = NULL;
    gboolean                            is_vpn       = FALSE;
    gs_unref_variant GVariant          *settings     = NULL;
    gs_unref_variant GVariant          *options      = NULL;
    const char                         *device_path;
    const char                         *specific_object_path;
    gs_free NMConnection              **conns        = NULL;
    NMSettingsConnectionPersistMode     persist_mode = NM_SETTINGS_CONNECTION_PERSIST_MODE_TO_DISK;
    gboolean                            is_volatile  = FALSE;
    gboolean                            bind_dbus_client = FALSE;
    AsyncOpType                         async_op_type;

    if (nm_streq(method_info->parent.name, "AddAndActivateConnection2")) {
        async_op_type = ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE2;
        g_variant_get(parameters,
                      "(@a{sa{sv}}&o&o@a{sv})",
                      &settings,
                      &device_path,
                      &specific_object_path,
                      &options);
    } else {
        nm_assert(nm_streq(method_info->parent.name, "AddAndActivateConnection"));
        async_op_type = ASYNC_OP_TYPE_AC_AUTH_ADD_AND_ACTIVATE;
        g_variant_get(parameters,
                      "(@a{sa{sv}}&o&o)",
                      &settings,
                      &device_path,
                      &specific_object_path);
    }

    if (options) {
        GVariantIter iter;
        const char  *option_name;
        GVariant    *option_value;

        g_variant_iter_init(&iter, options);
        while (g_variant_iter_next(&iter, "{&sv}", &option_name, &option_value)) {
            gs_unref_variant GVariant *option_value_free = NULL;
            const char                *s;

            option_value_free = option_value;

            if (nm_streq(option_name, "persist")
                && g_variant_is_of_type(option_value, G_VARIANT_TYPE_STRING)) {
                s = g_variant_get_string(option_value, NULL);

                is_volatile  = FALSE;
                persist_mode = NM_SETTINGS_CONNECTION_PERSIST_MODE_TO_DISK;

                if (nm_streq(s, "volatile")) {
                    persist_mode = NM_SETTINGS_CONNECTION_PERSIST_MODE_IN_MEMORY_ONLY;
                    is_volatile  = TRUE;
                } else if (nm_streq(s, "memory"))
                    persist_mode = NM_SETTINGS_CONNECTION_PERSIST_MODE_IN_MEMORY_ONLY;
                else if (nm_streq(s, "disk")) {
                    /* pass */
                } else {
                    error = g_error_new_literal(
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                        "Option \"persist\" must be one of \"volatile\", \"memory\" or \"disk\"");
                    goto error;
                }
            } else if (nm_streq(option_name, "bind-activation")
                       && g_variant_is_of_type(option_value, G_VARIANT_TYPE_STRING)) {
                s = g_variant_get_string(option_value, NULL);

                if (nm_streq(s, "dbus-client"))
                    bind_dbus_client = TRUE;
                else if (nm_streq(s, "none"))
                    bind_dbus_client = FALSE;
                else {
                    error = g_error_new_literal(
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                        "Option \"bind-activation\" must be one of \"dbus-client\" or \"none\"");
                    goto error;
                }
            } else {
                error = g_error_new_literal(NM_MANAGER_ERROR,
                                            NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                                            "Unknown extra option passed");
                goto error;
            }
        }
    }

    specific_object_path = nm_dbus_path_not_empty(specific_object_path);
    device_path          = nm_dbus_path_not_empty(device_path);

    /* Try to create a new connection with the given settings.
     * We allow empty settings for AddAndActivateConnection(). In that case,
     * the connection will be completed in nm_utils_complete_generic() or
     * nm_device_complete_connection() below. Just make sure we don't expect
     * specific data being in the connection till then (especially in
     * validate_activation_request()).
     */
    incompl_conn = nm_simple_connection_new();
    if (settings && g_variant_n_children(settings))
        _nm_connection_replace_settings(incompl_conn,
                                        settings,
                                        NM_SETTING_PARSE_FLAGS_STRICT,
                                        NULL);

    subject = validate_activation_request(self, invocation, NULL, incompl_conn, &error);
    if (!subject)
        goto error;

    if (!find_device_for_activation(self,
                                    NULL,
                                    incompl_conn,
                                    device_path,
                                    &device,
                                    &is_vpn,
                                    &error)) {
        goto error;
    }

    conns = nm_settings_connections_array_to_connections(
        nm_settings_get_connections(priv->settings, NULL),
        -1);

    if (device) {
        /* Let each device subclass complete the connection */
        if (!nm_device_complete_connection(device,
                                           incompl_conn,
                                           specific_object_path,
                                           conns,
                                           &error))
            goto error;
    } else {
        nm_utils_complete_generic(priv->platform,
                                  incompl_conn,
                                  nm_connection_get_connection_type(incompl_conn),
                                  conns,
                                  is_vpn ? _("VPN connection") : NULL,
                                  nm_connection_get_connection_type(incompl_conn),
                                  NULL,
                                  NULL);

        if (!nm_connection_verify(incompl_conn, &error))
            goto error;

        if (!is_vpn && !device) {
            nm_assert(nm_connection_is_virtual(incompl_conn));

            device = system_create_virtual_device(self, incompl_conn, &error);
            if (!device)
                goto error;
        }
    }

    active = _new_active_connection(self,
                                    is_vpn,
                                    NULL,
                                    incompl_conn,
                                    NULL,
                                    specific_object_path,
                                    device,
                                    subject,
                                    NM_ACTIVATION_TYPE_MANAGED,
                                    NM_ACTIVATION_REASON_USER_REQUEST,
                                    _activation_bind_lifetime_to_profile_visibility(subject),
                                    &error);
    if (!active)
        goto error;

    if (bind_dbus_client) {
        NMKeepAlive *keep_alive;

        keep_alive = nm_active_connection_get_keep_alive(active);
        nm_keep_alive_set_dbus_client_watch(keep_alive, dbus_connection, sender);
        nm_keep_alive_arm(keep_alive);
    }

    nm_active_connection_authorize(active,
                                   incompl_conn,
                                   _async_op_complete_ac_auth_cb,
                                   _async_op_data_new_ac_auth_add_and_activate(self,
                                                                               async_op_type,
                                                                               active,
                                                                               invocation,
                                                                               incompl_conn,
                                                                               persist_mode,
                                                                               is_volatile));

    /* we passed the pointers on to _async_op_data_new_ac_auth_add_and_activate() */
    g_steal_pointer(&incompl_conn);
    g_steal_pointer(&active);
    return;

error:
    nm_audit_log_connection_op(NM_AUDIT_OP_CONN_ADD_ACTIVATE,
                               NULL,
                               FALSE,
                               NULL,
                               subject,
                               error->message);
    g_dbus_method_invocation_take_error(invocation, error);
}

/*****************************************************************************/

gboolean
nm_manager_deactivate_connection(NMManager          *manager,
                                 NMActiveConnection *active,
                                 NMDeviceStateReason reason,
                                 GError            **error)
{
    if (NM_IS_VPN_CONNECTION(active)) {
        NMActiveConnectionStateReason vpn_reason =
            NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED;

        if (nm_device_state_reason_check(reason) == NM_DEVICE_STATE_REASON_CONNECTION_REMOVED)
            vpn_reason = NM_ACTIVE_CONNECTION_STATE_REASON_CONNECTION_REMOVED;

        if (!nm_vpn_connection_deactivate(NM_VPN_CONNECTION(active), vpn_reason, FALSE)) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_CONNECTION_NOT_ACTIVE,
                                "The VPN connection was not active.");
            return FALSE;
        }
    } else {
        nm_assert(NM_IS_ACT_REQUEST(active));
        nm_device_disconnect_active_connection(active,
                                               reason,
                                               NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN);
    }

    _notify(manager, PROP_ACTIVE_CONNECTIONS);
    return TRUE;
}

static void
deactivate_net_auth_done_cb(NMAuthChain *chain, GDBusMethodInvocation *context, gpointer user_data)
{
    NMManager          *self  = NM_MANAGER(user_data);
    GError             *error = NULL;
    NMAuthCallResult    result;
    NMActiveConnection *active;
    char               *path;

    nm_assert(G_IS_DBUS_METHOD_INVOCATION(context));

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));

    path   = nm_auth_chain_get_data(chain, "path");
    result = nm_auth_chain_get_result(chain, NM_AUTH_PERMISSION_NETWORK_CONTROL);
    active = active_connection_get_by_path(self, path);

    if (result != NM_AUTH_CALL_RESULT_YES) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_PERMISSION_DENIED,
                                    "Not authorized to deactivate connections");
    } else if (!active) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_CONNECTION_NOT_ACTIVE,
                                    "The connection was not active.");
    } else {
        /* success; deactivation allowed */
        if (!nm_manager_deactivate_connection(self,
                                              active,
                                              NM_DEVICE_STATE_REASON_USER_REQUESTED,
                                              &error))
            nm_assert(error);
    }

    if (active) {
        nm_audit_log_connection_op(NM_AUDIT_OP_CONN_DEACTIVATE,
                                   nm_active_connection_get_settings_connection(active),
                                   !error,
                                   NULL,
                                   nm_auth_chain_get_subject(chain),
                                   error ? error->message : NULL);
    }

    if (error)
        g_dbus_method_invocation_take_error(context, error);
    else
        g_dbus_method_invocation_return_value(context, NULL);
}

static void
impl_manager_deactivate_connection(NMDBusObject                      *obj,
                                   const NMDBusInterfaceInfoExtended *interface_info,
                                   const NMDBusMethodInfoExtended    *method_info,
                                   GDBusConnection                   *dbus_connection,
                                   const char                        *sender,
                                   GDBusMethodInvocation             *invocation,
                                   GVariant                          *parameters)
{
    NMManager            *self = NM_MANAGER(obj);
    NMManagerPrivate     *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection   *ac;
    NMSettingsConnection *sett_conn = NULL;
    GError               *error     = NULL;
    NMAuthSubject        *subject   = NULL;
    NMAuthChain          *chain;
    const char           *active_path;

    g_variant_get(parameters, "(&o)", &active_path);

    /* Find the connection by its object path */
    ac = active_connection_get_by_path(self, active_path);
    if (ac)
        sett_conn = nm_active_connection_get_settings_connection(ac);

    if (!sett_conn) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_CONNECTION_NOT_ACTIVE,
                                    "The connection was not active.");
        goto done;
    }

    /* Validate the caller */
    subject = nm_dbus_manager_new_auth_subject_from_context(invocation);
    if (!subject) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_PERMISSION_DENIED,
                                    NM_UTILS_ERROR_MSG_REQ_UID_UKNOWN);
        goto done;
    }

    if (!nm_auth_is_subject_in_acl_set_error(nm_settings_connection_get_connection(sett_conn),
                                             subject,
                                             NM_MANAGER_ERROR,
                                             NM_MANAGER_ERROR_PERMISSION_DENIED,
                                             &error))
        goto done;

    chain = nm_auth_chain_new_subject(subject, invocation, deactivate_net_auth_done_cb, self);
    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "path", g_strdup(active_path), g_free);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_NETWORK_CONTROL, TRUE);

done:
    if (error) {
        if (sett_conn) {
            nm_audit_log_connection_op(NM_AUDIT_OP_CONN_DEACTIVATE,
                                       sett_conn,
                                       FALSE,
                                       NULL,
                                       subject,
                                       error->message);
        }
        g_dbus_method_invocation_take_error(invocation, error);
    }
    g_clear_object(&subject);
}

static void
sleep_devices_check_empty(NMManager *self)
{
    NMManagerPrivate      *priv = NM_MANAGER_GET_PRIVATE(self);
    GDBusMethodInvocation *invocation;

    if (g_hash_table_size(priv->sleep_devices) > 0)
        return;

    while (priv->sleep_invocations) {
        invocation = priv->sleep_invocations->data;
        g_dbus_method_invocation_return_value(invocation, NULL);
        priv->sleep_invocations = g_slist_remove(priv->sleep_invocations, invocation);
    }
}

static gboolean
sleep_devices_add(NMManager *self, NMDevice *device, gboolean suspending)
{
    NMManagerPrivate              *priv   = NM_MANAGER_GET_PRIVATE(self);
    NMPowerMonitorInhibitorHandle *handle = NULL;

    if (g_hash_table_lookup_extended(priv->sleep_devices, device, NULL, (gpointer *) &handle)) {
        if (suspending) {
            /* if we are suspending, always insert a new handle in sleep_devices.
             * Even if we had an old handle, it might be stale by now. */
            g_hash_table_insert(priv->sleep_devices,
                                device,
                                nm_power_monitor_inhibit_take(priv->power_monitor));
            if (handle)
                nm_power_monitor_inhibit_release(priv->power_monitor, handle);
        }
        return FALSE;
    }

    g_hash_table_insert(priv->sleep_devices,
                        g_object_ref(device),
                        suspending ? nm_power_monitor_inhibit_take(priv->power_monitor) : NULL);
    g_signal_connect(device, "notify::" NM_DEVICE_STATE, G_CALLBACK(device_sleep_cb), self);
    return TRUE;
}

static gboolean
sleep_devices_remove(NMManager *self, NMDevice *device)
{
    NMManagerPrivate              *priv = NM_MANAGER_GET_PRIVATE(self);
    NMPowerMonitorInhibitorHandle *handle;

    if (!g_hash_table_lookup_extended(priv->sleep_devices, device, NULL, (gpointer *) &handle))
        return FALSE;

    if (handle)
        nm_power_monitor_inhibit_release(priv->power_monitor, handle);

    /* Remove device from hash */
    g_signal_handlers_disconnect_by_func(device, device_sleep_cb, self);
    g_hash_table_remove(priv->sleep_devices, device);
    g_object_unref(device);

    sleep_devices_check_empty(self);

    return TRUE;
}

static void
sleep_devices_clear(NMManager *self)
{
    NMManagerPrivate              *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice                      *device;
    NMPowerMonitorInhibitorHandle *handle;
    GHashTableIter                 iter;

    g_hash_table_iter_init(&iter, priv->sleep_devices);
    while (g_hash_table_iter_next(&iter, (gpointer *) &device, (gpointer *) &handle)) {
        g_signal_handlers_disconnect_by_func(device, device_sleep_cb, self);
        if (handle)
            nm_power_monitor_inhibit_release(priv->power_monitor, handle);
        g_object_unref(device);
        g_hash_table_iter_remove(&iter);
    }

    sleep_devices_check_empty(self);
}

static void
device_sleep_cb(NMDevice *device, GParamSpec *pspec, NMManager *self)
{
    switch (nm_device_get_state(device)) {
    case NM_DEVICE_STATE_DISCONNECTED:
        _LOGD(LOGD_SUSPEND, "sleep: unmanaging device %s", nm_device_get_ip_iface(device));
        nm_device_set_unmanaged_by_flags_queue(device,
                                               NM_UNMANAGED_SLEEPING,
                                               NM_UNMAN_FLAG_OP_SET_UNMANAGED,
                                               NM_DEVICE_STATE_REASON_SLEEPING);
        break;
    case NM_DEVICE_STATE_UNMANAGED:
        _LOGD(LOGD_SUSPEND, "sleep: device %s is ready", nm_device_get_ip_iface(device));

        if (!sleep_devices_remove(self, device))
            g_return_if_reached();

        break;
    default:
        return;
    }
}

static void
_handle_device_takedown(NMManager *self,
                        NMDevice  *device,
                        gboolean   suspending,
                        gboolean   is_shutdown)
{
    nm_device_notify_sleeping(device);

    if (nm_device_is_activating(device)
        || nm_device_get_state(device) == NM_DEVICE_STATE_ACTIVATED) {
        _LOGD(LOGD_SUSPEND,
              "%s: wait disconnection of device %s",
              is_shutdown ? "shutdown" : "sleep",
              nm_device_get_ip_iface(device));

        if (sleep_devices_add(self, device, suspending))
            nm_device_queue_state(device,
                                  NM_DEVICE_STATE_DEACTIVATING,
                                  NM_DEVICE_STATE_REASON_SLEEPING);
    } else {
        nm_device_set_unmanaged_by_flags(device,
                                         NM_UNMANAGED_SLEEPING,
                                         NM_UNMAN_FLAG_OP_SET_UNMANAGED,
                                         NM_DEVICE_STATE_REASON_SLEEPING);
    }
}

static void
do_sleep_wake(NMManager *self, gboolean sleeping_changed)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    gboolean          suspending, waking_from_suspend;
    NMDevice         *device;

    suspending          = sleeping_changed && priv->sleeping;
    waking_from_suspend = sleeping_changed && !priv->sleeping;

    if (manager_sleeping(self)) {
        _LOGD(LOGD_SUSPEND, "sleep: %s...", suspending ? "sleeping" : "disabling");

        /* FIXME: are there still hardware devices that need to be disabled around
         * suspend/resume?
         */
        c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
            if (nm_device_is_software(device)) {
                /* If a user disables networking we consider that as an
                 * indication that also software devices must be disconnected.
                 * But we don't want to destroy them for external events as
                 * a system suspend.
                 */
                if (suspending)
                    continue;
            }
            /* Wake-on-LAN devices will be taken down post-suspend rather than pre- */
            if (suspending && device_is_wake_on_lan(priv->platform, device)) {
                _LOGD(LOGD_SUSPEND,
                      "sleep: device %s has wake-on-lan, skipping",
                      nm_device_get_ip_iface(device));
                continue;
            }

            _handle_device_takedown(self, device, suspending, FALSE);
        }
    } else {
        _LOGD(LOGD_SUSPEND, "sleep: %s...", waking_from_suspend ? "waking up" : "re-enabling");

        sleep_devices_clear(self);

        if (waking_from_suspend) {
            c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
                if (nm_device_is_software(device))
                    continue;

                /* Belatedly take down Wake-on-LAN devices; ideally we wouldn't have to do this
                 * but for now it's the only way to make sure we re-check their connectivity.
                 */
                if (device_is_wake_on_lan(priv->platform, device))
                    nm_device_set_unmanaged_by_flags(device,
                                                     NM_UNMANAGED_SLEEPING,
                                                     NM_UNMAN_FLAG_OP_SET_UNMANAGED,
                                                     NM_DEVICE_STATE_REASON_SLEEPING);

                /* Check if the device is unmanaged but the state transition is still pending.
                 * If so, change state now so that later we re-manage the device forcing a
                 * re-check of available connections.
                 */
                if (!nm_device_get_managed(device, FALSE)
                    && nm_device_get_state(device) != NM_DEVICE_STATE_UNMANAGED) {
                    nm_device_state_changed(device,
                                            NM_DEVICE_STATE_UNMANAGED,
                                            NM_DEVICE_STATE_REASON_SLEEPING);
                }
            }
        }

        /* Ensure rfkill state is up-to-date since we don't respond to state
         * changes during sleep.
         */
        _rfkill_update(self, NM_RFKILL_TYPE_UNKNOWN);

        /* Re-manage managed devices */
        c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
            NMDeviceStateReason reason;
            guint               i;

            if (nm_device_is_software(device)
                && !nm_device_get_unmanaged_flags(device, NM_UNMANAGED_SLEEPING)) {
                /* DHCP leases of software devices could have gone stale
                 * so we need to renew them. */
                nm_device_update_dynamic_ip_setup(device, "wake up");
                continue;
            }

            /* enable/disable wireless devices since that we don't respond
             * to killswitch changes during sleep.
             */
            for (i = 0; i < NM_RFKILL_TYPE_MAX; i++) {
                const NMRfkillType      rtype   = i;
                const RfkillRadioState *rstate  = &priv->radio_states[rtype];
                gboolean                enabled = _rfkill_radio_state_get_enabled(rstate, TRUE);

                _LOGD(LOGD_RFKILL,
                      "rfkill: %s %s devices (hw_enabled %d, sw_enabled %d, user_enabled %d)",
                      enabled ? "enabling" : "disabling",
                      nm_rfkill_type_to_string(rtype),
                      rstate->hw_enabled,
                      rstate->sw_enabled,
                      rstate->user_enabled);
                if (nm_device_get_rfkill_type(device) == rtype)
                    nm_device_set_enabled(device, enabled);
            }

            /* The reason determines whether the device will be managed_type=managed
             * or managed_type=external. Pass the correct reason to restore the state
             * that was set before sleeping. */
            reason =
                nm_device_get_managed_type_before_sleep(device) == NM_DEVICE_MANAGED_TYPE_EXTERNAL
                    ? NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED
                    : NM_DEVICE_STATE_REASON_NOW_MANAGED;
            nm_device_set_unmanaged_by_flags(device,
                                             NM_UNMANAGED_SLEEPING,
                                             NM_UNMAN_FLAG_OP_SET_MANAGED,
                                             reason);
        }

        /* Give the connections a chance to recreate the virtual devices.
         * We've torn them down on sleep. */
        connections_changed(self);
    }

    nm_manager_update_state(self);
}

static void
_internal_sleep(NMManager *self, gboolean do_sleep)
{
    NMManagerPrivate *priv;

    g_return_if_fail(NM_IS_MANAGER(self));

    priv = NM_MANAGER_GET_PRIVATE(self);

    if (priv->sleeping == do_sleep)
        return;

    _LOGI(LOGD_SUSPEND,
          "sleep: %s requested (sleeping: %s  enabled: %s)",
          do_sleep ? "sleep" : "wake",
          priv->sleeping ? "yes" : "no",
          priv->net_enabled ? "yes" : "no");

    priv->sleeping = do_sleep;

    do_sleep_wake(self, TRUE);

    _notify(self, PROP_SLEEPING);
}

static void
impl_manager_sleep(NMDBusObject                      *obj,
                   const NMDBusInterfaceInfoExtended *interface_info,
                   const NMDBusMethodInfoExtended    *method_info,
                   GDBusConnection                   *connection,
                   const char                        *sender,
                   GDBusMethodInvocation             *invocation,
                   GVariant                          *parameters)
{
    NMManager                     *self    = NM_MANAGER(obj);
    NMManagerPrivate              *priv    = NM_MANAGER_GET_PRIVATE(self);
    GError                        *error   = NULL;
    gs_unref_object NMAuthSubject *subject = NULL;
    gboolean                       do_sleep;

    g_variant_get(parameters, "(b)", &do_sleep);

    subject = nm_dbus_manager_new_auth_subject_from_context(invocation);

    if (priv->sleeping == do_sleep) {
        error = g_error_new(NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_ALREADY_ASLEEP_OR_AWAKE,
                            "Already %s",
                            do_sleep ? "asleep" : "awake");
        nm_audit_log_control_op(NM_AUDIT_OP_SLEEP_CONTROL,
                                do_sleep ? "on" : "off",
                                FALSE,
                                subject,
                                error->message);
        g_dbus_method_invocation_take_error(invocation, error);
        return;
    }

    /* Unconditionally allow the request.  Previously it was polkit protected
     * but unfortunately that doesn't work for short-lived processes like
     * pm-utils.  It uses dbus-send without --print-reply, which quits
     * immediately after sending the request, and NM is unable to obtain the
     * sender's UID as dbus-send has already dropped off the bus.  Thus NM
     * fails the request.  Instead, don't validate the request, but rely on
     * D-Bus permissions to restrict the call to root.
     */
    _internal_sleep(self, do_sleep);
    nm_audit_log_control_op(NM_AUDIT_OP_SLEEP_CONTROL,
                            do_sleep ? "on" : "off",
                            TRUE,
                            subject,
                            NULL);

    priv->sleep_invocations = g_slist_prepend(priv->sleep_invocations, invocation);
    sleep_devices_check_empty(self);

    return;
}

static void
sleeping_cb(NMPowerMonitor *monitor, gboolean is_about_to_suspend, gpointer user_data)
{
    NMManager *self = user_data;

    _LOGT(LOGD_SUSPEND, "sleep: received %s signal", is_about_to_suspend ? "sleeping" : "resuming");
    _internal_sleep(self, is_about_to_suspend);
}

static void
shutdown_cb(NMPowerMonitor *monitor, gpointer user_data)
{
    NMManager        *self = user_data;
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    _LOGT(LOGD_SUSPEND, "shutdown: received shutdown signal");

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        NMSettingConnection *s_con;
        gboolean             take_down = FALSE;

        s_con = nm_device_get_applied_setting(device, NM_TYPE_SETTING_CONNECTION);
        if (!s_con)
            continue;

        if (nm_setting_connection_get_down_on_poweroff(s_con)
            == NM_SETTING_CONNECTION_DOWN_ON_POWEROFF_YES)
            take_down = TRUE;
        else if (nm_setting_connection_get_down_on_poweroff(s_con)
                 == NM_SETTING_CONNECTION_DOWN_ON_POWEROFF_DEFAULT)
            take_down = nm_config_data_get_connection_default_int64(
                NM_CONFIG_GET_DATA,
                NM_CON_DEFAULT("connection.down-on-poweroff"),
                device,
                NM_SETTING_CONNECTION_DOWN_ON_POWEROFF_NO,
                NM_SETTING_CONNECTION_DOWN_ON_POWEROFF_YES,
                NM_SETTING_CONNECTION_DOWN_ON_POWEROFF_NO);

        if (take_down)
            _handle_device_takedown(self, device, FALSE, TRUE);
    }
}

static void
_internal_enable(NMManager *self, gboolean enable)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    nm_config_state_set(priv->config,
                        TRUE,
                        FALSE,
                        NM_CONFIG_STATE_PROPERTY_NETWORKING_ENABLED,
                        enable);

    _LOGI(LOGD_SUSPEND,
          "%s requested (sleeping: %s  enabled: %s)",
          enable ? "enable" : "disable",
          priv->sleeping ? "yes" : "no",
          priv->net_enabled ? "yes" : "no");

    priv->net_enabled = enable;

    do_sleep_wake(self, FALSE);

    _notify(self, PROP_NETWORKING_ENABLED);
}

static void
enable_net_done_cb(NMAuthChain *chain, GDBusMethodInvocation *context, gpointer user_data)
{
    NMManager        *self = NM_MANAGER(user_data);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthCallResult  result;
    gboolean          enable;
    NMAuthSubject    *subject;

    nm_assert(G_IS_DBUS_METHOD_INVOCATION(context));

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));
    enable  = GPOINTER_TO_UINT(nm_auth_chain_get_data(chain, "enable"));
    subject = nm_auth_chain_get_subject(chain);

    result = nm_auth_chain_get_result(chain, NM_AUTH_PERMISSION_ENABLE_DISABLE_NETWORK);
    if (result != NM_AUTH_CALL_RESULT_YES) {
        GError *ret_error;

        ret_error = g_error_new_literal(NM_MANAGER_ERROR,
                                        NM_MANAGER_ERROR_PERMISSION_DENIED,
                                        "Not authorized to enable/disable networking");
        nm_audit_log_control_op(NM_AUDIT_OP_NET_CONTROL,
                                enable ? "on" : "off",
                                FALSE,
                                subject,
                                ret_error->message);
        g_dbus_method_invocation_take_error(context, ret_error);
        return;
    }

    _internal_enable(self, enable);
    nm_audit_log_control_op(NM_AUDIT_OP_NET_CONTROL, enable ? "on" : "off", TRUE, subject, NULL);

    priv->sleep_invocations = g_slist_prepend(priv->sleep_invocations, context);
    sleep_devices_check_empty(self);
}

static void
impl_manager_enable(NMDBusObject                      *obj,
                    const NMDBusInterfaceInfoExtended *interface_info,
                    const NMDBusMethodInfoExtended    *method_info,
                    GDBusConnection                   *connection,
                    const char                        *sender,
                    GDBusMethodInvocation             *invocation,
                    GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;
    GError           *error = NULL;
    gboolean          enable;

    g_variant_get(parameters, "(b)", &enable);

    if (priv->net_enabled == enable) {
        error = g_error_new(NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_ALREADY_ENABLED_OR_DISABLED,
                            "Already %s",
                            enable ? "enabled" : "disabled");
        goto done;
    }

    chain = nm_auth_chain_new_context(invocation, enable_net_done_cb, self);
    if (!chain) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_PERMISSION_DENIED,
                                    NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        goto done;
    }

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "enable", GUINT_TO_POINTER(enable), NULL);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_ENABLE_DISABLE_NETWORK, TRUE);

done:
    if (error)
        g_dbus_method_invocation_take_error(invocation, error);
}

/* Permissions */

static void
get_permissions_done_cb(NMAuthChain *chain, GDBusMethodInvocation *context, gpointer user_data)
{
    GVariantBuilder results;
    int             i;

    nm_assert(G_IS_DBUS_METHOD_INVOCATION(context));

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));

    g_variant_builder_init(&results, G_VARIANT_TYPE("a{ss}"));

    for (i = 0; i < (int) G_N_ELEMENTS(nm_auth_permission_sorted); i++) {
        const char *permission = nm_auth_permission_names_by_idx[nm_auth_permission_sorted[i] - 1];
        NMAuthCallResult result;
        const char      *result_str;

        result     = nm_auth_chain_get_result(chain, permission);
        result_str = nm_client_permission_result_to_string(nm_auth_call_result_to_client(result));
        g_variant_builder_add(&results, "{ss}", permission, result_str);
    }

    g_dbus_method_invocation_return_value(context, g_variant_new("(a{ss})", &results));
}

static void
impl_manager_get_permissions(NMDBusObject                      *obj,
                             const NMDBusInterfaceInfoExtended *interface_info,
                             const NMDBusMethodInfoExtended    *method_info,
                             GDBusConnection                   *connection,
                             const char                        *sender,
                             GDBusMethodInvocation             *invocation,
                             GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;
    int               i;

    chain = nm_auth_chain_new_context(invocation, get_permissions_done_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));

    for (i = 0; i < (int) G_N_ELEMENTS(nm_auth_permission_sorted); i++) {
        const char *permission = nm_auth_permission_names_by_idx[nm_auth_permission_sorted[i] - 1];

        nm_auth_chain_add_call_unsafe(chain, permission, FALSE);
    }
}

static void
impl_manager_state(NMDBusObject                      *obj,
                   const NMDBusInterfaceInfoExtended *interface_info,
                   const NMDBusMethodInfoExtended    *method_info,
                   GDBusConnection                   *connection,
                   const char                        *sender,
                   GDBusMethodInvocation             *invocation,
                   GVariant                          *parameters)
{
    NMManager *self = NM_MANAGER(obj);

    nm_manager_update_state(self);
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(u)", NM_MANAGER_GET_PRIVATE(self)->state));
}

static void
impl_manager_set_logging(NMDBusObject                      *obj,
                         const NMDBusInterfaceInfoExtended *interface_info,
                         const NMDBusMethodInfoExtended    *method_info,
                         GDBusConnection                   *connection,
                         const char                        *sender,
                         GDBusMethodInvocation             *invocation,
                         GVariant                          *parameters)
{
    NMManager  *self  = NM_MANAGER(obj);
    GError     *error = NULL;
    const char *level;
    const char *domains;

    /* The permission is already enforced by the D-Bus daemon, but we ensure
     * that the caller is still alive so that clients are forced to wait and
     * we'll be able to switch to polkit without breaking behavior.
     */
    if (!nm_dbus_manager_ensure_uid(nm_dbus_object_get_manager(NM_DBUS_OBJECT(self)),
                                    invocation,
                                    G_MAXULONG,
                                    NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_PERMISSION_DENIED))
        return;

    g_variant_get(parameters, "(&s&s)", &level, &domains);

    if (nm_logging_setup(level, domains, NULL, &error)) {
        _LOGI(LOGD_CORE,
              "logging: level '%s' domains '%s'",
              nm_logging_level_to_string(),
              nm_logging_domains_to_string());
    }

    if (error)
        g_dbus_method_invocation_take_error(invocation, error);
    else
        g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
impl_manager_get_logging(NMDBusObject                      *obj,
                         const NMDBusInterfaceInfoExtended *interface_info,
                         const NMDBusMethodInfoExtended    *method_info,
                         GDBusConnection                   *connection,
                         const char                        *sender,
                         GDBusMethodInvocation             *invocation,
                         GVariant                          *parameters)
{
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(ss)", nm_logging_level_to_string(), nm_logging_domains_to_string()));
}

typedef struct {
    NMManager             *self;
    GDBusMethodInvocation *context;
    guint                  remaining;
} ConnectivityCheckData;

static void
device_connectivity_done(NMDevice                   *device,
                         NMDeviceConnectivityHandle *handle,
                         NMConnectivityState         state,
                         GError                     *error,
                         gpointer                    user_data)
{
    ConnectivityCheckData *data = user_data;
    NMManager             *self;
    NMManagerPrivate      *priv;

    nm_assert(data);
    nm_assert(data->remaining > 0);
    nm_assert(NM_IS_MANAGER(data->self));

    data->remaining--;

    self = data->self;
    priv = NM_MANAGER_GET_PRIVATE(self);

    if (data->context
        && (data->remaining == 0
            || (state == NM_CONNECTIVITY_FULL
                && priv->connectivity_state == NM_CONNECTIVITY_FULL))) {
        /* despite having a @handle and @state returned by the requests, we always
         * return the current connectivity_state. That is, because the connectivity_state
         * and the answer to the connectivity check shall agree.
         *
         * However, if one of the requests (early) returns full connectivity and agrees with
         * the accumulated connectivity state, we no longer have to wait. The result is set.
         *
         * This also works well, because NMDevice first emits change signals to its own
         * connectivity state, which is then taken into account for the accumulated global
         * state. All this happens, before the callback is invoked. */
        g_dbus_method_invocation_return_value(
            g_steal_pointer(&data->context),
            g_variant_new("(u)", (guint) priv->connectivity_state));
    }

    if (data->remaining == 0) {
        g_object_unref(self);
        g_slice_free(ConnectivityCheckData, data);
    }
}

static void
check_connectivity_auth_done_cb(NMAuthChain           *chain,
                                GDBusMethodInvocation *context,
                                gpointer               user_data)
{
    NMManager             *self  = NM_MANAGER(user_data);
    NMManagerPrivate      *priv  = NM_MANAGER_GET_PRIVATE(self);
    GError                *error = NULL;
    NMAuthCallResult       result;
    ConnectivityCheckData *data;
    NMDevice              *device;

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));

    result = nm_auth_chain_get_result(chain, NM_AUTH_PERMISSION_NETWORK_CONTROL);

    if (result != NM_AUTH_CALL_RESULT_YES) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_PERMISSION_DENIED,
                                    "Not authorized to recheck connectivity");
    }
    if (error) {
        g_dbus_method_invocation_take_error(context, error);
        return;
    }

    data            = g_slice_new(ConnectivityCheckData);
    data->self      = g_object_ref(self);
    data->context   = context;
    data->remaining = 0;

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        if (nm_device_check_connectivity(device, AF_INET, device_connectivity_done, data))
            data->remaining++;
        if (nm_device_check_connectivity(device, AF_INET6, device_connectivity_done, data))
            data->remaining++;
    }

    if (data->remaining == 0) {
        /* call the handler at least once. */
        data->remaining = 1;
        device_connectivity_done(NULL, NULL, NM_CONNECTIVITY_UNKNOWN, NULL, data);
        /* @data got destroyed. */
    }
}

static void
impl_manager_check_connectivity(NMDBusObject                      *obj,
                                const NMDBusInterfaceInfoExtended *interface_info,
                                const NMDBusMethodInfoExtended    *method_info,
                                GDBusConnection                   *connection,
                                const char                        *sender,
                                GDBusMethodInvocation             *invocation,
                                GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;

    chain = nm_auth_chain_new_context(invocation, check_connectivity_auth_done_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_NETWORK_CONTROL, TRUE);
}

static void
start_factory(NMDeviceFactory *factory, gpointer user_data)
{
    nm_device_factory_start(factory);
}

gboolean
nm_manager_write_device_state(NMManager *self, NMDevice *device, int *out_ifindex)
{
    NMManagerPrivate              *priv = NM_MANAGER_GET_PRIVATE(self);
    int                            ifindex;
    gboolean                       managed;
    NMConfigDeviceStateManagedType managed_type;
    const char                    *uuid              = NULL;
    const char                    *perm_hw_addr_fake = NULL;
    gboolean                       perm_hw_addr_is_fake;
    guint32                        route_metric_default_aspired;
    guint32                        route_metric_default_effective;
    NMTernary                      nm_owned;

    NM_SET_OUT(out_ifindex, 0);

    ifindex = nm_device_get_ip_ifindex(device);
    if (ifindex <= 0)
        return FALSE;

    if (!nm_platform_link_get(priv->platform, ifindex))
        return FALSE;

    managed = nm_device_get_managed(device, FALSE);
    if (managed) {
        NMSettingsConnection *sett_conn = NULL;

        if (nm_device_get_state(device) <= NM_DEVICE_STATE_ACTIVATED)
            sett_conn = nm_device_get_settings_connection(device);
        if (sett_conn)
            uuid = nm_settings_connection_get_uuid(sett_conn);
        managed_type = NM_CONFIG_DEVICE_STATE_MANAGED_TYPE_MANAGED;
    } else if (nm_device_get_unmanaged_flags(device, NM_UNMANAGED_USER_EXPLICIT))
        managed_type = NM_CONFIG_DEVICE_STATE_MANAGED_TYPE_UNMANAGED;
    else
        managed_type = NM_CONFIG_DEVICE_STATE_MANAGED_TYPE_UNKNOWN;

    perm_hw_addr_fake =
        nm_device_get_permanent_hw_address_full(device, FALSE, &perm_hw_addr_is_fake);
    if (perm_hw_addr_fake && !perm_hw_addr_is_fake)
        perm_hw_addr_fake = NULL;

    nm_owned = nm_device_is_software(device) ? nm_device_is_nm_owned(device) : NM_TERNARY_DEFAULT;

    route_metric_default_effective = _device_route_metric_get(self,
                                                              ifindex,
                                                              NM_DEVICE_TYPE_UNKNOWN,
                                                              TRUE,
                                                              &route_metric_default_aspired);

    if (!nm_config_device_state_write(ifindex,
                                      managed_type,
                                      perm_hw_addr_fake,
                                      uuid,
                                      nm_owned,
                                      route_metric_default_aspired,
                                      route_metric_default_effective,
                                      nm_device_get_dhcp_config(device, AF_INET),
                                      nm_device_get_dhcp_config(device, AF_INET6),
                                      nm_device_is_software(device)
                                          && nm_device_get_device_type(device)
                                                 == NM_DEVICE_TYPE_GENERIC))
        return FALSE;

    NM_SET_OUT(out_ifindex, ifindex);
    return TRUE;
}

void
nm_manager_write_device_state_all(NMManager *self)
{
    NMManagerPrivate              *priv               = NM_MANAGER_GET_PRIVATE(self);
    gs_unref_hashtable GHashTable *preserve_ifindexes = NULL;
    NMDevice                      *device;
    NMActiveConnection            *ac;

    preserve_ifindexes = g_hash_table_new(nm_direct_hash, NULL);

    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        int ifindex;

        if (nm_manager_write_device_state(self, device, &ifindex)) {
            g_hash_table_add(preserve_ifindexes, GINT_TO_POINTER(ifindex));
        }
    }

    /* Save to disk the timestamps of active connections as if we were bringing them down.
     * Otherwise they will be wrong on next start and affect the activation order.
     */
    c_list_for_each_entry (ac, &priv->active_connections_lst_head, active_connections_lst) {
        NMSettingsConnection *sett = nm_active_connection_get_settings_connection(ac);
        nm_settings_connection_update_timestamp(sett, (guint64) time(NULL));
    }

    nm_config_device_state_prune_stale(preserve_ifindexes, NULL);
}

static gboolean
devices_inited_cb(gpointer user_data)
{
    NMManager        *self = user_data;
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    priv->devices_inited_id = 0;
    priv->devices_inited    = TRUE;
    check_if_startup_complete(self);
    return G_SOURCE_REMOVE;
}

gboolean
nm_manager_start(NMManager *self, GError **error)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    guint             i;

    nm_device_factory_manager_load_factories(_register_device_factory, self);

    nm_device_factory_manager_for_each_factory(start_factory, NULL);

    /* Set initial radio enabled/disabled state */
    for (i = 0; i < NM_RFKILL_TYPE_MAX; i++) {
        const NMRfkillType rtype  = i;
        RfkillRadioState  *rstate = &priv->radio_states[rtype];
        gboolean           enabled;

        /* recheck kernel rfkill state */
        _rfkill_radio_state_set_from_manager(self, rtype, rstate);

        _LOGI(LOGD_RFKILL,
              "rfkill: %s %s by radio killswitch; %s by state file",
              nm_rfkill_type_to_string(rtype),
              (rstate->hw_enabled && rstate->sw_enabled) ? "enabled" : "disabled",
              rstate->user_enabled ? "enabled" : "disabled");
        enabled = _rfkill_radio_state_get_enabled(rstate, TRUE);
        _rfkill_update_devices(self, rtype, enabled);
    }

    _LOGI(LOGD_CORE, "Networking is %s by state file", priv->net_enabled ? "enabled" : "disabled");

    system_unmanaged_devices_changed_cb(priv->settings, NULL, self);

    _static_hostname_changed_cb(priv->hostname_manager, NULL, self);

    if (!nm_settings_start(priv->settings, error))
        return FALSE;

    nm_platform_process_events(priv->platform);

    g_signal_connect(priv->platform,
                     NM_PLATFORM_SIGNAL_LINK_CHANGED,
                     G_CALLBACK(platform_link_cb),
                     self);

    platform_query_devices(self);

    /* Load VPN plugins */
    priv->vpn_manager = g_object_ref(nm_vpn_manager_get());

    _LOGD(LOGD_CORE, "creating virtual devices...");
    g_signal_connect(priv->settings,
                     NM_SETTINGS_SIGNAL_CONNECTION_ADDED,
                     G_CALLBACK(connection_added_cb),
                     self);
    g_signal_connect(priv->settings,
                     NM_SETTINGS_SIGNAL_CONNECTION_UPDATED,
                     G_CALLBACK(connection_updated_cb),
                     self);

    /* Make sure virtual devices for all connections are created so
     * that they could be autoconnected.  */
    connections_changed(self);

    nm_clear_g_source(&priv->devices_inited_id);
    priv->devices_inited_id = g_idle_add_full(G_PRIORITY_LOW + 10, devices_inited_cb, self, NULL);

    return TRUE;
}

void
nm_manager_stop(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    /* FIXME(shutdown): we don't do a proper shutdown yet:
     *  - need to ensure that all pending async operations are cancelled
     *    - e.g. operations in priv->async_op_lst_head
     *  - need to ensure that no more asynchronous requests are started,
     *    or that they complete quickly, or that they fail quickly.
     *  - note that cancelling some operations is not possible synchronously.
     *    Hence, stop() only prepares shutdown and tells everybody to not
     *    accept new work, and to complete in a timely manner.
     *    We need to still iterate the mainloop for a bit, to give everybody
     *    the chance to complete.
     *    - e.g. see comment at nm_auth_manager_force_shutdown()
     */

    nm_dbus_manager_stop(nm_dbus_object_get_manager(NM_DBUS_OBJECT(self)));

    while ((device = c_list_first_entry(&priv->devices_lst_head, NMDevice, devices_lst)))
        remove_device(self, device, TRUE);

    _active_connection_cleanup(self);

    nm_clear_g_source(&priv->devices_inited_id);
}

static gboolean
handle_firmware_changed(gpointer user_data)
{
    NMManager        *self = NM_MANAGER(user_data);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMDevice         *device;

    priv->fw_changed_id = 0;

    /* Try to re-enable devices with missing firmware */
    c_list_for_each_entry (device, &priv->devices_lst_head, devices_lst) {
        NMDeviceState state = nm_device_get_state(device);

        if (nm_device_get_firmware_missing(device) && (state == NM_DEVICE_STATE_UNAVAILABLE)) {
            _LOG2I(LOGD_CORE, device, "firmware may now be available");

            /* Re-set unavailable state to try bringing the device up again */
            nm_device_state_changed(device,
                                    NM_DEVICE_STATE_UNAVAILABLE,
                                    NM_DEVICE_STATE_REASON_NONE);
        }
    }

    return FALSE;
}

static void
firmware_dir_changed(GFileMonitor     *monitor,
                     GFile            *file,
                     GFile            *other_file,
                     GFileMonitorEvent event_type,
                     gpointer          user_data)
{
    NMManager        *self = NM_MANAGER(user_data);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    switch (event_type) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_MOVED:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        if (!priv->fw_changed_id) {
            priv->fw_changed_id = g_timeout_add_seconds(4, handle_firmware_changed, self);
            _LOGI(LOGD_CORE, "kernel firmware directory '%s' changed", KERNEL_FIRMWARE_DIR);
        }
        break;
    default:
        break;
    }
}

static void
connection_metered_changed(GObject *object, NMMetered metered, gpointer user_data)
{
    nm_manager_update_metered(NM_MANAGER(user_data));
}

static void
policy_default_ac_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    NMManager          *self = NM_MANAGER(user_data);
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *ac;

    /* Note: this assumes that it's not possible for the IP4 default
     * route to be going over the default-ip6-device. If that changes,
     * we need something more complicated here.
     */
    ac = nm_policy_get_default_ip4_ac(priv->policy);
    if (!ac)
        ac = nm_policy_get_default_ip6_ac(priv->policy);

    if (ac != priv->primary_connection) {
        if (priv->primary_connection) {
            g_signal_handlers_disconnect_by_func(priv->primary_connection,
                                                 G_CALLBACK(connection_metered_changed),
                                                 self);
            g_clear_object(&priv->primary_connection);
        }

        priv->primary_connection = ac ? g_object_ref(ac) : NULL;

        if (priv->primary_connection) {
            g_signal_connect(priv->primary_connection,
                             NM_ACTIVE_CONNECTION_DEVICE_METERED_CHANGED,
                             G_CALLBACK(connection_metered_changed),
                             self);
        }
        _LOGD(LOGD_CORE,
              "PrimaryConnection now %s",
              ac ? nm_active_connection_get_settings_connection_id(ac) : "(none)");
        _notify(self, PROP_PRIMARY_CONNECTION);
        _notify(self, PROP_PRIMARY_CONNECTION_TYPE);
        nm_manager_update_metered(self);
    }
}

static void
policy_activating_ac_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    NMManager          *self = NM_MANAGER(user_data);
    NMManagerPrivate   *priv = NM_MANAGER_GET_PRIVATE(self);
    NMActiveConnection *activating, *best;

    /* We only look at activating-ip6-ac if activating-ip4-ac
     * AND default-ip4-ac are NULL; if default-ip4-ac is
     * non-NULL, then activating-ip6-ac is irrelevant, since while
     * that AC might become the new default-ip6-ac, it can't
     * become primary-connection while default-ip4-ac is set to
     * something else.
     */
    activating = nm_policy_get_activating_ip4_ac(priv->policy);
    best       = nm_policy_get_default_ip4_ac(priv->policy);
    if (!activating && !best)
        activating = nm_policy_get_activating_ip6_ac(priv->policy);

    if (nm_g_object_ref_set(&priv->activating_connection, activating)) {
        _LOGD(LOGD_CORE,
              "ActivatingConnection now %s",
              activating ? nm_active_connection_get_settings_connection_id(activating) : "(none)");
        _notify(self, PROP_ACTIVATING_CONNECTION);
    }
}

/*****************************************************************************/

typedef struct {
    NMManager                         *self;
    NMDBusObject                      *obj;
    const NMDBusInterfaceInfoExtended *interface_info;
    const NMDBusPropertyInfoExtended  *property_info;
    GVariant                          *value;
    guint64                            export_version_id;
} DBusSetPropertyHandle;

#define NM_PERM_DENIED_ERROR "org.freedesktop.NetworkManager.PermissionDenied"

static const char *
_dbus_set_property_audit_log_get_args(NMDBusObject *obj,
                                      const char   *property_name,
                                      GVariant     *value,
                                      char        **str_to_free)
{
    nm_assert(str_to_free && !*str_to_free);

    /* We assert here that the property is one of the few expected ones.
     *
     * Future properties should not made writable! Add a D-Bus method instead,
     * they are more flexible (for example, you can set multiple properties at
     * once). */

    if (NM_IS_DEVICE(obj)) {
        nm_assert(NM_IN_STRSET(property_name,
                               NM_DEVICE_MANAGED,
                               NM_DEVICE_AUTOCONNECT,
                               NM_DEVICE_STATISTICS_REFRESH_RATE_MS));
        return (*str_to_free = g_variant_print(value, FALSE));
    }

    nm_assert(NM_IS_MANAGER(obj));
    if (NM_IN_STRSET(property_name,
                     NM_MANAGER_WIRELESS_ENABLED,
                     NM_MANAGER_WWAN_ENABLED,
                     NM_MANAGER_WIMAX_ENABLED,
                     NM_MANAGER_CONNECTIVITY_CHECK_ENABLED)) {
        return (*str_to_free = g_strdup_printf("%s:%s",
                                               property_name,
                                               g_variant_get_boolean(value) ? "on" : "off"));
    }
    if (NM_IN_STRSET(property_name, NM_MANAGER_GLOBAL_DNS_CONFIGURATION)) {
        return NM_MANAGER_GLOBAL_DNS_CONFIGURATION;
    }

    return nm_assert_unreachable_val("???");
}

/* this is a macro to catch the caller's line number. */
#define _dbus_set_property_audit_log(obj,                                                  \
                                     audit_op,                                             \
                                     auth_subject,                                         \
                                     property_name,                                        \
                                     value,                                                \
                                     error_message)                                        \
    G_STMT_START                                                                           \
    {                                                                                      \
        NMDBusObject *const  _obj           = (obj);                                       \
        const char *const    _audit_op      = (audit_op);                                  \
        NMAuthSubject *const _auth_subject  = (auth_subject);                              \
        const char *const    _property_name = (property_name);                             \
        GVariant *const      _value         = (value);                                     \
        const char *const    _error_message = (error_message);                             \
        gs_free char        *_args_to_free  = NULL;                                        \
                                                                                           \
        if (NM_IS_DEVICE(_obj)) {                                                          \
            nm_audit_log_device_op(_audit_op,                                              \
                                   NM_DEVICE(_obj),                                        \
                                   !_error_message,                                        \
                                   _dbus_set_property_audit_log_get_args(_obj,             \
                                                                         _property_name,   \
                                                                         _value,           \
                                                                         &_args_to_free),  \
                                   _auth_subject,                                          \
                                   _error_message);                                        \
        } else {                                                                           \
            nm_audit_log_control_op(_audit_op,                                             \
                                    _dbus_set_property_audit_log_get_args(_obj,            \
                                                                          _property_name,  \
                                                                          _value,          \
                                                                          &_args_to_free), \
                                    !_error_message,                                       \
                                    _auth_subject,                                         \
                                    _error_message);                                       \
        }                                                                                  \
    }                                                                                      \
    G_STMT_END

static void
_dbus_set_property_auth_cb(NMAuthChain           *chain,
                           GDBusMethodInvocation *invocation,
                           gpointer               user_data)
{
    DBusSetPropertyHandle             *handle_data       = user_data;
    gs_unref_object NMDBusObject      *obj               = handle_data->obj;
    const NMDBusInterfaceInfoExtended *interface_info    = handle_data->interface_info;
    const NMDBusPropertyInfoExtended  *property_info     = handle_data->property_info;
    gs_unref_variant GVariant         *value             = handle_data->value;
    guint64                            export_version_id = handle_data->export_version_id;
    gs_unref_object NMManager         *self              = handle_data->self;
    NMManagerPrivate                  *priv              = NM_MANAGER_GET_PRIVATE(self);
    NMAuthCallResult                   result;
    gs_free_error GError              *local         = NULL;
    const char                        *error_name    = NULL;
    const char                        *error_message = NULL;
    GValue                             gvalue;

    g_slice_free(DBusSetPropertyHandle, handle_data);

    c_list_unlink(nm_auth_chain_parent_lst_list(chain));
    result = nm_auth_chain_get_result(chain, property_info->writable.permission);

    if (result != NM_AUTH_CALL_RESULT_YES) {
        error_name    = NM_PERM_DENIED_ERROR;
        error_message = "Not authorized to perform this operation";
        goto out;
    }

    if (export_version_id != nm_dbus_object_get_export_version_id(obj)) {
        error_name    = "org.freedesktop.DBus.Error.UnknownObject";
        error_message = "Object was deleted while authenticating";
        goto out;
    }

    /* Handle some properties specially *sigh* */
    if (interface_info == &interface_info_manager
        && nm_streq(property_info->property_name, NM_MANAGER_GLOBAL_DNS_CONFIGURATION)) {
        const NMGlobalDnsConfig *global_dns;

        global_dns = nm_config_data_get_global_dns_config(nm_config_get_data(priv->config));
        if (global_dns && !nm_global_dns_config_is_internal(global_dns)) {
            error_name    = NM_PERM_DENIED_ERROR;
            error_message = "Global DNS configuration already set via configuration file";
            goto out;
        }
    }

    g_dbus_gvariant_to_gvalue(value, &gvalue);
    if (!nm_g_object_set_property(G_OBJECT(obj), property_info->property_name, &gvalue, &local)) {
        error_name    = "org.freedesktop.DBus.Error.InvalidArgs";
        error_message = local->message;
    }
    g_value_unset(&gvalue);

out:
    _dbus_set_property_audit_log(obj,
                                 property_info->writable.audit_op,
                                 nm_auth_chain_get_subject(chain),
                                 property_info->property_name,
                                 value,
                                 error_message);

    if (error_message)
        g_dbus_method_invocation_return_dbus_error(invocation, error_name, error_message);
    else
        g_dbus_method_invocation_return_value(invocation, NULL);
}

void
nm_manager_dbus_set_property_handle(NMDBusObject                      *obj,
                                    const NMDBusInterfaceInfoExtended *interface_info,
                                    const NMDBusPropertyInfoExtended  *property_info,
                                    GDBusConnection                   *connection,
                                    const char                        *sender,
                                    GDBusMethodInvocation             *invocation,
                                    GVariant                          *value,
                                    gpointer                           user_data)
{
    NMManager                     *self = user_data;
    NMManagerPrivate              *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain                   *chain;
    const char                    *error_message = NULL;
    gs_unref_object NMAuthSubject *subject       = NULL;
    DBusSetPropertyHandle         *handle_data;

    /* we only have writable properties on Device or Manager. In the future,
     * we probably should not add new API with writable properties. Add
     * methods instead. Systemd also avoids writable properties. */
    nm_assert(obj == (gpointer) self || NM_IS_DEVICE(obj));

    subject = nm_dbus_manager_new_auth_subject_from_context(invocation);
    if (!subject) {
        error_message = NM_UTILS_ERROR_MSG_REQ_UID_UKNOWN;

        _dbus_set_property_audit_log(obj,
                                     property_info->writable.audit_op,
                                     NULL,
                                     property_info->property_name,
                                     value,
                                     error_message);

        g_dbus_method_invocation_return_error_literal(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_AUTH_FAILED,
                                                      error_message);
        return;
    }

    handle_data                    = g_slice_new0(DBusSetPropertyHandle);
    handle_data->self              = g_object_ref(self);
    handle_data->obj               = g_object_ref(obj);
    handle_data->interface_info    = interface_info;
    handle_data->property_info     = property_info;
    handle_data->value             = g_variant_ref(value);
    handle_data->export_version_id = nm_dbus_object_get_export_version_id(obj);

    chain = nm_auth_chain_new_subject(subject, invocation, _dbus_set_property_auth_cb, handle_data);
    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_add_call_unsafe(chain, property_info->writable.permission, TRUE);
}

/*****************************************************************************/

static NMCheckpointManager *
_checkpoint_mgr_get(NMManager *self, gboolean create_as_needed)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    if (G_UNLIKELY(!priv->checkpoint_mgr) && create_as_needed)
        priv->checkpoint_mgr = nm_checkpoint_manager_new(self, obj_properties[PROP_CHECKPOINTS]);
    return priv->checkpoint_mgr;
}

static void
checkpoint_auth_done_cb(NMAuthChain *chain, GDBusMethodInvocation *context, gpointer user_data)
{
    NMManager       *self = NM_MANAGER(user_data);
    char            *op;
    char            *checkpoint_path = NULL;
    char           **devices;
    NMCheckpoint    *checkpoint;
    NMAuthCallResult result;
    guint32          timeout, flags;
    GVariant        *variant = NULL;
    GError          *error   = NULL;
    const char      *arg     = NULL;
    guint32          add_timeout;

    op = nm_auth_chain_get_data(chain, "audit-op");
    c_list_unlink(nm_auth_chain_parent_lst_list(chain));
    result = nm_auth_chain_get_result(chain, NM_AUTH_PERMISSION_CHECKPOINT_ROLLBACK);

    if (NM_IN_STRSET(op,
                     NM_AUDIT_OP_CHECKPOINT_DESTROY,
                     NM_AUDIT_OP_CHECKPOINT_ROLLBACK,
                     NM_AUDIT_OP_CHECKPOINT_ADJUST_ROLLBACK_TIMEOUT))
        arg = checkpoint_path = nm_auth_chain_get_data(chain, "checkpoint_path");

    if (result != NM_AUTH_CALL_RESULT_YES) {
        error = g_error_new_literal(NM_MANAGER_ERROR,
                                    NM_MANAGER_ERROR_PERMISSION_DENIED,
                                    "Not authorized to checkpoint/rollback");
    } else {
        if (nm_streq0(op, NM_AUDIT_OP_CHECKPOINT_CREATE)) {
            timeout = GPOINTER_TO_UINT(nm_auth_chain_get_data(chain, "timeout"));
            flags   = GPOINTER_TO_UINT(nm_auth_chain_get_data(chain, "flags"));
            devices = nm_auth_chain_get_data(chain, "devices");

            checkpoint = nm_checkpoint_manager_create(_checkpoint_mgr_get(self, TRUE),
                                                      (const char *const *) devices,
                                                      timeout,
                                                      (NMCheckpointCreateFlags) flags,
                                                      &error);
            if (checkpoint) {
                arg     = nm_dbus_object_get_path(NM_DBUS_OBJECT(checkpoint));
                variant = g_variant_new("(o)", arg);
            }
        } else if (nm_streq0(op, NM_AUDIT_OP_CHECKPOINT_DESTROY)) {
            nm_checkpoint_manager_destroy(_checkpoint_mgr_get(self, TRUE), checkpoint_path, &error);
        } else if (nm_streq0(op, NM_AUDIT_OP_CHECKPOINT_ROLLBACK)) {
            nm_checkpoint_manager_rollback(_checkpoint_mgr_get(self, TRUE),
                                           checkpoint_path,
                                           &variant,
                                           &error);
        } else if (nm_streq0(op, NM_AUDIT_OP_CHECKPOINT_ADJUST_ROLLBACK_TIMEOUT)) {
            add_timeout = GPOINTER_TO_UINT(nm_auth_chain_get_data(chain, "add_timeout"));
            nm_checkpoint_manager_adjust_rollback_timeout(_checkpoint_mgr_get(self, TRUE),
                                                          checkpoint_path,
                                                          add_timeout,
                                                          &error);
        } else
            g_return_if_reached();
    }

    nm_audit_log_checkpoint_op(op,
                               arg ?: "",
                               !error,
                               nm_auth_chain_get_subject(chain),
                               error ? error->message : NULL);

    if (error)
        g_dbus_method_invocation_take_error(context, error);
    else
        g_dbus_method_invocation_return_value(context, variant);
}

static void
impl_manager_checkpoint_create(NMDBusObject                      *obj,
                               const NMDBusInterfaceInfoExtended *interface_info,
                               const NMDBusMethodInfoExtended    *method_info,
                               GDBusConnection                   *connection,
                               const char                        *sender,
                               GDBusMethodInvocation             *invocation,
                               GVariant                          *parameters)
{
    NMManager         *self = NM_MANAGER(obj);
    NMManagerPrivate  *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain       *chain;
    gs_strfreev char **devices = NULL;
    guint32            rollback_timeout;
    guint32            flags;

    G_STATIC_ASSERT_EXPR(sizeof(flags) <= sizeof(NMCheckpointCreateFlags));

    g_variant_get(parameters, "(^aouu)", &devices, &rollback_timeout, &flags);

    if ((NMCheckpointCreateFlags) flags != flags
        || NM_FLAGS_ANY(flags,
                        ~((guint32) (NM_CHECKPOINT_CREATE_FLAG_DESTROY_ALL
                                     | NM_CHECKPOINT_CREATE_FLAG_DELETE_NEW_CONNECTIONS
                                     | NM_CHECKPOINT_CREATE_FLAG_DISCONNECT_NEW_DEVICES
                                     | NM_CHECKPOINT_CREATE_FLAG_ALLOW_OVERLAPPING
                                     | NM_CHECKPOINT_CREATE_FLAG_NO_PRESERVE_EXTERNAL_PORTS
                                     | NM_CHECKPOINT_CREATE_FLAG_TRACK_INTERNAL_GLOBAL_DNS)))) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                                                      "Invalid flags");
        return;
    }

    chain = nm_auth_chain_new_context(invocation, checkpoint_auth_done_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "audit-op", NM_AUDIT_OP_CHECKPOINT_CREATE, NULL);
    nm_auth_chain_set_data(chain,
                           "devices",
                           g_steal_pointer(&devices),
                           (GDestroyNotify) g_strfreev);
    nm_auth_chain_set_data(chain, "flags", GUINT_TO_POINTER(flags), NULL);
    nm_auth_chain_set_data(chain, "timeout", GUINT_TO_POINTER(rollback_timeout), NULL);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_CHECKPOINT_ROLLBACK, TRUE);
}

static void
impl_manager_checkpoint_destroy(NMDBusObject                      *obj,
                                const NMDBusInterfaceInfoExtended *interface_info,
                                const NMDBusMethodInfoExtended    *method_info,
                                GDBusConnection                   *connection,
                                const char                        *sender,
                                GDBusMethodInvocation             *invocation,
                                GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;
    const char       *checkpoint_path;

    chain = nm_auth_chain_new_context(invocation, checkpoint_auth_done_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    g_variant_get(parameters, "(&o)", &checkpoint_path);

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "audit-op", NM_AUDIT_OP_CHECKPOINT_DESTROY, NULL);
    nm_auth_chain_set_data(chain, "checkpoint_path", g_strdup(checkpoint_path), g_free);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_CHECKPOINT_ROLLBACK, TRUE);
}

static void
impl_manager_checkpoint_rollback(NMDBusObject                      *obj,
                                 const NMDBusInterfaceInfoExtended *interface_info,
                                 const NMDBusMethodInfoExtended    *method_info,
                                 GDBusConnection                   *connection,
                                 const char                        *sender,
                                 GDBusMethodInvocation             *invocation,
                                 GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;
    const char       *checkpoint_path;

    chain = nm_auth_chain_new_context(invocation, checkpoint_auth_done_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    g_variant_get(parameters, "(&o)", &checkpoint_path);

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "audit-op", NM_AUDIT_OP_CHECKPOINT_ROLLBACK, NULL);
    nm_auth_chain_set_data(chain, "checkpoint_path", g_strdup(checkpoint_path), g_free);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_CHECKPOINT_ROLLBACK, TRUE);
}

static void
impl_manager_checkpoint_adjust_rollback_timeout(NMDBusObject                      *obj,
                                                const NMDBusInterfaceInfoExtended *interface_info,
                                                const NMDBusMethodInfoExtended    *method_info,
                                                GDBusConnection                   *connection,
                                                const char                        *sender,
                                                GDBusMethodInvocation             *invocation,
                                                GVariant                          *parameters)
{
    NMManager        *self = NM_MANAGER(obj);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    NMAuthChain      *chain;
    const char       *checkpoint_path;
    guint32           add_timeout;

    chain = nm_auth_chain_new_context(invocation, checkpoint_auth_done_cb, self);
    if (!chain) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      NM_MANAGER_ERROR,
                                                      NM_MANAGER_ERROR_PERMISSION_DENIED,
                                                      NM_UTILS_ERROR_MSG_REQ_AUTH_FAILED);
        return;
    }

    g_variant_get(parameters, "(&ou)", &checkpoint_path, &add_timeout);

    c_list_link_tail(&priv->auth_lst_head, nm_auth_chain_parent_lst_list(chain));
    nm_auth_chain_set_data(chain, "audit-op", NM_AUDIT_OP_CHECKPOINT_ADJUST_ROLLBACK_TIMEOUT, NULL);
    nm_auth_chain_set_data(chain, "checkpoint_path", g_strdup(checkpoint_path), g_free);
    nm_auth_chain_set_data(chain, "add_timeout", GUINT_TO_POINTER(add_timeout), NULL);
    nm_auth_chain_add_call(chain, NM_AUTH_PERMISSION_CHECKPOINT_ROLLBACK, TRUE);
}

/*****************************************************************************/

NMDnsManager *
nm_manager_get_dns_manager(NMManager *self)
{
    NMManagerPrivate *priv;

    g_return_val_if_fail(NM_IS_MANAGER(self), NULL);

    priv = NM_MANAGER_GET_PRIVATE(self);

    if (G_UNLIKELY(!priv->dns_mgr)) {
        /* Initialize lazily on first use.
         *
         * But keep a reference. This is to ensure proper lifetimes between
         * singleton instances (i.e. nm_dns_manager_get() outlives NMManager). */
        priv->dns_mgr = g_object_ref(nm_dns_manager_get());
    }

    return priv->dns_mgr;
}

/*****************************************************************************/

static void
auth_mgr_changed(NMAuthManager *auth_manager, gpointer user_data)
{
    /* Let clients know they should re-check their authorization */
    nm_dbus_object_emit_signal(user_data,
                               &interface_info_manager,
                               &signal_info_check_permissions,
                               "()");
}

/*****************************************************************************/

void
nm_manager_unblock_failed_ovs_interfaces(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    nm_policy_unblock_failed_ovs_interfaces(priv->policy);
}

/*****************************************************************************/

void
nm_manager_set_capability(NMManager *self, NMCapability cap)
{
    NMManagerPrivate *priv;
    guint32           cap_i;
    gssize            idx;

    g_return_if_fail(NM_IS_MANAGER(self));
    if (cap < 1 || cap > _NM_CAPABILITY_MAX)
        g_return_if_reached();

    cap_i = (guint32) cap;

    priv = NM_MANAGER_GET_PRIVATE(self);

    idx = nm_array_find_bsearch(nm_g_array_first_p(priv->capabilities, guint32),
                                priv->capabilities->len,
                                sizeof(guint32),
                                &cap_i,
                                nm_cmp_uint32_p_with_data,
                                NULL);
    if (idx >= 0)
        return;

    nm_assert((~idx) <= (gssize) priv->capabilities->len);

    g_array_insert_val(priv->capabilities, ~idx, cap_i);
    _notify(self, PROP_CAPABILITIES);
}

void
nm_manager_emit_device_ifindex_changed(NMManager *self, NMDevice *device)
{
    g_signal_emit(self, signals[DEVICE_IFINDEX_CHANGED], 0, device);
}

/*****************************************************************************/

NM_DEFINE_SINGLETON_REGISTER(NMManager);

NMManager *
nm_manager_get(void)
{
    g_return_val_if_fail(singleton_instance, NULL);
    return singleton_instance;
}

NMSettings *
nm_settings_get(void)
{
    g_return_val_if_fail(singleton_instance, NULL);

    return NM_MANAGER_GET_PRIVATE(singleton_instance)->settings;
}

NMPolicy *
nm_manager_get_policy(NMManager *self)
{
    g_return_val_if_fail(NM_IS_MANAGER(self), NULL);

    return NM_MANAGER_GET_PRIVATE(self)->policy;
}

NMManager *
nm_manager_setup(void)
{
    NMManager *self;

    g_return_val_if_fail(!singleton_instance, singleton_instance);

    self = g_object_new(NM_TYPE_MANAGER, NULL);
    nm_assert(NM_IS_MANAGER(self));
    singleton_instance = self;

    nm_singleton_instance_register();
    nm_log_dbg(LOGD_CORE,
               "setup %s singleton (" NM_HASH_OBFUSCATE_PTR_FMT ")",
               "NMManager",
               NM_HASH_OBFUSCATE_PTR(singleton_instance));

    nm_dbus_object_export(NM_DBUS_OBJECT(self));
    return self;
}

static void
constructed(GObject *object)
{
    NMManager           *self = NM_MANAGER(object);
    NMManagerPrivate    *priv = NM_MANAGER_GET_PRIVATE(self);
    const NMConfigState *state;

    G_OBJECT_CLASS(nm_manager_parent_class)->constructed(object);

    priv->settings = nm_settings_new(self);

    nm_dbus_object_export(NM_DBUS_OBJECT(priv->settings));

    g_signal_connect(priv->settings,
                     "notify::" NM_SETTINGS_STARTUP_COMPLETE,
                     G_CALLBACK(settings_startup_complete_changed),
                     self);
    g_signal_connect(priv->settings,
                     "notify::" NM_SETTINGS_UNMANAGED_SPECS,
                     G_CALLBACK(system_unmanaged_devices_changed_cb),
                     self);
    g_signal_connect(priv->settings,
                     NM_SETTINGS_SIGNAL_CONNECTION_FLAGS_CHANGED,
                     G_CALLBACK(connection_flags_changed),
                     self);

    priv->hostname_manager = g_object_ref(nm_hostname_manager_get());
    g_signal_connect(priv->hostname_manager,
                     "notify::" NM_HOSTNAME_MANAGER_STATIC_HOSTNAME,
                     G_CALLBACK(_static_hostname_changed_cb),
                     self);

    /*
     * Do not delete existing virtual devices to keep connectivity up.
     * Virtual devices are reused when NetworkManager is restarted.
     * Hence, don't react on NM_SETTINGS_SIGNAL_CONNECTION_REMOVED.
     */

    priv->policy = nm_policy_new(self, priv->settings);
    g_signal_connect(priv->policy,
                     "notify::" NM_POLICY_DEFAULT_IP4_AC,
                     G_CALLBACK(policy_default_ac_changed),
                     self);
    g_signal_connect(priv->policy,
                     "notify::" NM_POLICY_DEFAULT_IP6_AC,
                     G_CALLBACK(policy_default_ac_changed),
                     self);
    g_signal_connect(priv->policy,
                     "notify::" NM_POLICY_ACTIVATING_IP4_AC,
                     G_CALLBACK(policy_activating_ac_changed),
                     self);
    g_signal_connect(priv->policy,
                     "notify::" NM_POLICY_ACTIVATING_IP6_AC,
                     G_CALLBACK(policy_activating_ac_changed),
                     self);

    priv->config = g_object_ref(nm_config_get());
    g_signal_connect(G_OBJECT(priv->config),
                     NM_CONFIG_SIGNAL_CONFIG_CHANGED,
                     G_CALLBACK(_config_changed_cb),
                     self);

    state = nm_config_state_get(priv->config);

    priv->net_enabled = state->net_enabled;

    priv->radio_states[NM_RFKILL_TYPE_WLAN].user_enabled = state->wifi_enabled;
    priv->radio_states[NM_RFKILL_TYPE_WWAN].user_enabled = state->wwan_enabled;

    priv->rfkill_mgr = nm_rfkill_manager_new();
    g_signal_connect(priv->rfkill_mgr,
                     NM_RFKILL_MANAGER_SIGNAL_RFKILL_CHANGED,
                     G_CALLBACK(rfkill_manager_rfkill_changed_cb),
                     self);

    /* Force kernel Wi-Fi/WWAN rfkill state to follow NM saved Wi-Fi/WWAN state
     * in case the BIOS doesn't save rfkill state, and to be consistent with user
     * changes to the WirelessEnabled/WWANEnabled properties which toggle kernel
     * rfkill.
     */
    _rfkill_update_system(self,
                          NM_RFKILL_TYPE_WLAN,
                          priv->radio_states[NM_RFKILL_TYPE_WLAN].user_enabled);
    _rfkill_update_system(self,
                          NM_RFKILL_TYPE_WWAN,
                          priv->radio_states[NM_RFKILL_TYPE_WWAN].user_enabled);
}

static void
nm_manager_init(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    GFile            *file;

    c_list_init(&priv->auth_lst_head);
    c_list_init(&priv->link_cb_lst);
    c_list_init(&priv->devices_lst_head);
    c_list_init(&priv->active_connections_lst_head);
    c_list_init(&priv->async_op_lst_head);
    c_list_init(&priv->delete_volatile_connection_lst_head);

    priv->platform = g_object_ref(NM_PLATFORM_GET);

    priv->capabilities = g_array_new(FALSE, FALSE, sizeof(guint32));

    priv->radio_states[NM_RFKILL_TYPE_WLAN] = (RfkillRadioState) {
        .user_enabled = TRUE,
        .sw_enabled   = FALSE,
        .hw_enabled   = TRUE,
    };
    priv->radio_states[NM_RFKILL_TYPE_WWAN] = (RfkillRadioState) {
        .user_enabled = TRUE,
        .sw_enabled   = FALSE,
        .hw_enabled   = TRUE,
    };

    priv->sleeping = FALSE;
    priv->state    = NM_STATE_DISCONNECTED;
    priv->startup  = TRUE;

    priv->devcon_data_dict = g_hash_table_new(_devcon_data_hash, _devcon_data_equal);

    /* sleep/wake handling */
    priv->power_monitor = nm_power_monitor_new();
    g_signal_connect(priv->power_monitor, NM_POWER_MONITOR_SLEEPING, G_CALLBACK(sleeping_cb), self);
    g_signal_connect(priv->power_monitor, NM_POWER_MONITOR_SHUTDOWN, G_CALLBACK(shutdown_cb), self);

    /* Listen for authorization changes */
    priv->auth_mgr = g_object_ref(nm_auth_manager_get());
    g_signal_connect(priv->auth_mgr,
                     NM_AUTH_MANAGER_SIGNAL_CHANGED,
                     G_CALLBACK(auth_mgr_changed),
                     self);

    /* Monitor the firmware directory */
    if (strlen(KERNEL_FIRMWARE_DIR)) {
        file             = g_file_new_for_path(KERNEL_FIRMWARE_DIR "/");
        priv->fw_monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, NULL, NULL);
        g_object_unref(file);
    }

    if (priv->fw_monitor) {
        g_signal_connect(priv->fw_monitor, "changed", G_CALLBACK(firmware_dir_changed), self);
        _LOGI(LOGD_CORE, "monitoring kernel firmware directory '%s'.", KERNEL_FIRMWARE_DIR);
    } else {
        _LOGW(LOGD_CORE, "failed to monitor kernel firmware directory '%s'.", KERNEL_FIRMWARE_DIR);
    }

    priv->metered       = NM_METERED_UNKNOWN;
    priv->sleep_devices = g_hash_table_new(nm_direct_hash, NULL);
}

static void
get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMManager               *self = NM_MANAGER(object);
    NMManagerPrivate        *priv = NM_MANAGER_GET_PRIVATE(self);
    NMConfigData            *config_data;
    const NMGlobalDnsConfig *dns_config;
    const char              *type;
    const char              *path;
    NMActiveConnection      *ac;
    GPtrArray               *ptrarr;

    switch (prop_id) {
    case PROP_VERSION:
        g_value_set_string(value, VERSION);
        break;
    case PROP_VERSION_INFO:
        g_value_set_variant(value, _version_info_get());
        break;
    case PROP_CAPABILITIES:
        g_value_set_variant(value,
                            nm_g_variant_new_au(nm_g_array_first_p(priv->capabilities, guint32),
                                                priv->capabilities->len));
        break;
    case PROP_STATE:
        g_value_set_uint(value, priv->state);
        break;
    case PROP_STARTUP:
        g_value_set_boolean(value, priv->startup);
        break;
    case PROP_NETWORKING_ENABLED:
        g_value_set_boolean(value, priv->net_enabled);
        break;
    case PROP_WIRELESS_ENABLED:
        g_value_set_boolean(value, _rfkill_radio_state_get(self, NM_RFKILL_TYPE_WLAN));
        break;
    case PROP_WIRELESS_HARDWARE_ENABLED:
        g_value_set_boolean(value, priv->radio_states[NM_RFKILL_TYPE_WLAN].hw_enabled);
        break;
    case PROP_WWAN_ENABLED:
        g_value_set_boolean(value, _rfkill_radio_state_get(self, NM_RFKILL_TYPE_WWAN));
        break;
    case PROP_WWAN_HARDWARE_ENABLED:
        g_value_set_boolean(value, priv->radio_states[NM_RFKILL_TYPE_WWAN].hw_enabled);
        break;
    case PROP_WIMAX_ENABLED:
        g_value_set_boolean(value, FALSE);
        break;
    case PROP_WIMAX_HARDWARE_ENABLED:
        g_value_set_boolean(value, FALSE);
        break;
    case PROP_RADIO_FLAGS:
        g_value_set_uint(value, priv->radio_flags);
        break;
    case PROP_ACTIVE_CONNECTIONS:
        ptrarr = g_ptr_array_new();
        c_list_for_each_entry_prev (ac,
                                    &priv->active_connections_lst_head,
                                    active_connections_lst) {
            path = nm_dbus_object_get_path(NM_DBUS_OBJECT(ac));
            if (path)
                g_ptr_array_add(ptrarr, g_strdup(path));
        }
        g_ptr_array_add(ptrarr, NULL);
        g_value_take_boxed(value, g_ptr_array_free(ptrarr, FALSE));
        break;
    case PROP_CONNECTIVITY:
        g_value_set_uint(value, priv->connectivity_state);
        break;
    case PROP_CONNECTIVITY_CHECK_AVAILABLE:
        config_data = nm_config_get_data(priv->config);
        g_value_set_boolean(value, nm_config_data_get_connectivity_uri(config_data) != NULL);
        break;
    case PROP_CONNECTIVITY_CHECK_ENABLED:
        g_value_set_boolean(value, concheck_enabled(self, NULL));
        break;
    case PROP_CONNECTIVITY_CHECK_URI:
        config_data = nm_config_get_data(priv->config);
        g_value_set_string(value, nm_config_data_get_connectivity_uri(config_data));
        break;
    case PROP_PRIMARY_CONNECTION:
        nm_dbus_utils_g_value_set_object_path(value, priv->primary_connection);
        break;
    case PROP_PRIMARY_CONNECTION_TYPE:
        type = NULL;
        if (priv->primary_connection) {
            NMConnection *con;

            con = nm_active_connection_get_applied_connection(priv->primary_connection);
            if (con)
                type = nm_connection_get_connection_type(con);
        }
        g_value_set_string(value, type ?: "");
        break;
    case PROP_ACTIVATING_CONNECTION:
        nm_dbus_utils_g_value_set_object_path(value, priv->activating_connection);
        break;
    case PROP_SLEEPING:
        g_value_set_boolean(value, priv->sleeping);
        break;
    case PROP_DEVICES:
        g_value_take_boxed(value, nm_strv_make_deep_copied(_get_devices_paths(self, FALSE)));
        break;
    case PROP_METERED:
        g_value_set_uint(value, priv->metered);
        break;
    case PROP_GLOBAL_DNS_CONFIGURATION:
        config_data = nm_config_get_data(priv->config);
        dns_config  = nm_config_data_get_global_dns_config(config_data);
        nm_global_dns_config_to_dbus(dns_config, value);
        break;
    case PROP_ALL_DEVICES:
        g_value_take_boxed(value, nm_strv_make_deep_copied(_get_devices_paths(self, TRUE)));
        break;
    case PROP_CHECKPOINTS:
        g_value_take_boxed(
            value,
            priv->checkpoint_mgr
                ? nm_strv_make_deep_copied(
                      nm_checkpoint_manager_get_checkpoint_paths(priv->checkpoint_mgr, NULL))
                : NULL);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    NMManager         *self = NM_MANAGER(object);
    NMManagerPrivate  *priv = NM_MANAGER_GET_PRIVATE(self);
    NMGlobalDnsConfig *dns_config;
    GError            *error = NULL;

    switch (prop_id) {
    case PROP_WIRELESS_ENABLED:
        _rfkill_update_from_user(NM_MANAGER(object),
                                 NM_RFKILL_TYPE_WLAN,
                                 g_value_get_boolean(value));
        break;
    case PROP_WWAN_ENABLED:
        _rfkill_update_from_user(NM_MANAGER(object),
                                 NM_RFKILL_TYPE_WWAN,
                                 g_value_get_boolean(value));
        break;
    case PROP_WIMAX_ENABLED:
        /* WIMAX is deprecated. This does nothing. */
        break;
    case PROP_CONNECTIVITY_CHECK_ENABLED:
        nm_config_set_connectivity_check_enabled(priv->config, g_value_get_boolean(value));
        break;
    case PROP_GLOBAL_DNS_CONFIGURATION:
        dns_config = nm_global_dns_config_from_dbus(value, &error);
        if (!error)
            nm_config_set_global_dns(priv->config, dns_config, &error);

        nm_global_dns_config_free(dns_config);

        if (error) {
            _LOGD(LOGD_CORE, "set global DNS failed with error: %s", error->message);
            g_error_free(error);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
_deinit_device_factory(NMDeviceFactory *factory, gpointer user_data)
{
    g_signal_handlers_disconnect_matched(factory,
                                         G_SIGNAL_MATCH_DATA,
                                         0,
                                         0,
                                         NULL,
                                         NULL,
                                         NM_MANAGER(user_data));
}

static void
dispose(GObject *object)
{
    NMManager        *self = NM_MANAGER(object);
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);
    CList            *iter;

    nm_assert(c_list_is_empty(&priv->async_op_lst_head));

    g_signal_handlers_disconnect_by_func(priv->platform, G_CALLBACK(platform_link_cb), self);
    while ((iter = c_list_first(&priv->link_cb_lst))) {
        PlatformLinkCbData *data = c_list_entry(iter, PlatformLinkCbData, lst);

        g_source_remove(data->idle_id);
        c_list_unlink_stale(&data->lst);
        g_slice_free(PlatformLinkCbData, data);
    }

    while ((iter = c_list_first(&priv->auth_lst_head)))
        nm_auth_chain_destroy(nm_auth_chain_parent_lst_entry(iter));

    nm_clear_g_source(&priv->devices_inited_id);

    nm_clear_pointer(&priv->checkpoint_mgr, nm_checkpoint_manager_free);

    if (priv->concheck_mgr) {
        g_signal_handlers_disconnect_by_func(priv->concheck_mgr,
                                             G_CALLBACK(concheck_config_changed_cb),
                                             self);
        g_clear_object(&priv->concheck_mgr);
    }

    nm_clear_g_signal_handler(priv->dns_mgr, &priv->dns_mgr_update_pending_signal_id);
    g_clear_object(&priv->dns_mgr);

    if (priv->auth_mgr) {
        g_signal_handlers_disconnect_by_func(priv->auth_mgr, G_CALLBACK(auth_mgr_changed), self);
        g_clear_object(&priv->auth_mgr);
    }

    nm_assert(c_list_is_empty(&priv->devices_lst_head));

    nm_clear_g_source(&priv->ac_cleanup_id);

    while ((iter = c_list_first(&priv->active_connections_lst_head)))
        active_connection_remove(self,
                                 c_list_entry(iter, NMActiveConnection, active_connections_lst));

    nm_assert(c_list_is_empty(&priv->active_connections_lst_head));
    g_clear_object(&priv->primary_connection);
    g_clear_object(&priv->activating_connection);

    if (priv->config) {
        g_signal_handlers_disconnect_by_func(priv->config, _config_changed_cb, self);
        g_clear_object(&priv->config);
    }

    if (priv->policy) {
        g_signal_handlers_disconnect_by_func(priv->policy, policy_default_ac_changed, self);
        g_signal_handlers_disconnect_by_func(priv->policy, policy_activating_ac_changed, self);
        g_clear_object(&priv->policy);
    }

    if (priv->settings) {
        g_signal_handlers_disconnect_by_func(priv->settings,
                                             settings_startup_complete_changed,
                                             self);
        g_signal_handlers_disconnect_by_func(priv->settings,
                                             system_unmanaged_devices_changed_cb,
                                             self);
        g_signal_handlers_disconnect_by_func(priv->settings, connection_added_cb, self);
        g_signal_handlers_disconnect_by_func(priv->settings, connection_updated_cb, self);
        g_signal_handlers_disconnect_by_func(priv->settings, connection_flags_changed, self);
        g_clear_object(&priv->settings);
    }

    if (priv->hostname_manager) {
        g_signal_handlers_disconnect_by_func(priv->hostname_manager,
                                             _static_hostname_changed_cb,
                                             self);
        g_clear_object(&priv->hostname_manager);
    }

    g_clear_object(&priv->vpn_manager);

    if (priv->sleep_devices) {
        sleep_devices_clear(self);
        nm_clear_pointer(&priv->sleep_devices, g_hash_table_unref);
    }

    if (priv->power_monitor) {
        g_signal_handlers_disconnect_by_func(priv->power_monitor, sleeping_cb, self);
        g_clear_object(&priv->power_monitor);
    }

    if (priv->fw_monitor) {
        g_signal_handlers_disconnect_by_func(priv->fw_monitor, firmware_dir_changed, self);

        nm_clear_g_source(&priv->fw_changed_id);

        g_file_monitor_cancel(priv->fw_monitor);
        g_clear_object(&priv->fw_monitor);
    }

    if (priv->rfkill_mgr) {
        g_signal_handlers_disconnect_by_func(priv->rfkill_mgr,
                                             rfkill_manager_rfkill_changed_cb,
                                             self);
        g_clear_object(&priv->rfkill_mgr);
    }

    nm_clear_g_source(&priv->delete_volatile_connection_idle_id);
    _delete_volatile_connection_all(self, FALSE);
    nm_assert(!priv->delete_volatile_connection_idle_id);
    nm_assert(c_list_is_empty(&priv->delete_volatile_connection_lst_head));

    nm_device_factory_manager_for_each_factory(_deinit_device_factory, self);

    nm_clear_g_source(&priv->timestamp_update_id);

    nm_clear_pointer(&priv->device_route_metrics, g_hash_table_destroy);

    nm_clear_pointer(&priv->devcon_data_dict, g_hash_table_destroy);

    G_OBJECT_CLASS(nm_manager_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(object);

    g_array_free(priv->capabilities, TRUE);

    G_OBJECT_CLASS(nm_manager_parent_class)->finalize(object);

    g_object_unref(priv->platform);
}

static const GDBusSignalInfo signal_info_check_permissions =
    NM_DEFINE_GDBUS_SIGNAL_INFO_INIT("CheckPermissions", );

static const GDBusSignalInfo signal_info_state_changed = NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
    "StateChanged",
    .args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("state", "u"), ), );

static const GDBusSignalInfo signal_info_device_added = NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
    "DeviceAdded",
    .args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("device_path", "o"), ), );

static const GDBusSignalInfo signal_info_device_removed = NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
    "DeviceRemoved",
    .args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("device_path", "o"), ), );

static const NMDBusInterfaceInfoExtended interface_info_manager = {
    .parent = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
        NM_DBUS_INTERFACE,
        .methods = NM_DEFINE_GDBUS_METHOD_INFOS(
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT("Reload",
                                                 .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                                                     NM_DEFINE_GDBUS_ARG_INFO("flags", "u"), ), ),
                .handle = impl_manager_reload, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "GetDevices",
                    .out_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("devices", "ao"), ), ),
                .handle = impl_manager_get_devices, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "GetAllDevices",
                    .out_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("devices", "ao"), ), ),
                .handle = impl_manager_get_all_devices, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "GetDeviceByIpIface",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("iface", "s"), ),
                    .out_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("device", "o"), ), ),
                .handle = impl_manager_get_device_by_ip_iface, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "ActivateConnection",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("connection", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("device", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("specific_object", "o"), ),
                    .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("active_connection", "o"), ), ),
                .handle = impl_manager_activate_connection, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "AddAndActivateConnection",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("connection", "a{sa{sv}}"),
                        NM_DEFINE_GDBUS_ARG_INFO("device", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("specific_object", "o"), ),
                    .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("path", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("active_connection", "o"), ), ),
                .handle = impl_manager_add_and_activate_connection, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "AddAndActivateConnection2",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("connection", "a{sa{sv}}"),
                        NM_DEFINE_GDBUS_ARG_INFO("device", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("specific_object", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("options", "a{sv}"), ),
                    .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("path", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("active_connection", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("result", "a{sv}"), ), ),
                .handle = impl_manager_add_and_activate_connection, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "DeactivateConnection",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("active_connection", "o"), ), ),
                .handle = impl_manager_deactivate_connection, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT("Sleep",
                                                 .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                                                     NM_DEFINE_GDBUS_ARG_INFO("sleep", "b"), ), ),
                .handle = impl_manager_sleep, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT("Enable",
                                                 .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                                                     NM_DEFINE_GDBUS_ARG_INFO("enable", "b"), ), ),
                .handle = impl_manager_enable, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "GetPermissions",
                    .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("permissions", "a{ss}"), ), ),
                .handle = impl_manager_get_permissions, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT("SetLogging",
                                                 .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                                                     NM_DEFINE_GDBUS_ARG_INFO("level", "s"),
                                                     NM_DEFINE_GDBUS_ARG_INFO("domains", "s"), ), ),
                .handle = impl_manager_set_logging, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT("GetLogging",
                                                 .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                                                     NM_DEFINE_GDBUS_ARG_INFO("level", "s"),
                                                     NM_DEFINE_GDBUS_ARG_INFO("domains", "s"), ), ),
                .handle = impl_manager_get_logging, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "CheckConnectivity",
                    .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("connectivity", "u"), ), ),
                .handle = impl_manager_check_connectivity, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT("state",
                                                 .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                                                     NM_DEFINE_GDBUS_ARG_INFO("state", "u"), ), ),
                .handle = impl_manager_state, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "CheckpointCreate",
                    .in_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("devices", "ao"),
                                                  NM_DEFINE_GDBUS_ARG_INFO("rollback_timeout", "u"),
                                                  NM_DEFINE_GDBUS_ARG_INFO("flags", "u"), ),
                    .out_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("checkpoint", "o"), ), ),
                .handle = impl_manager_checkpoint_create, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "CheckpointDestroy",
                    .in_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("checkpoint", "o"), ), ),
                .handle = impl_manager_checkpoint_destroy, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "CheckpointRollback",
                    .in_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("checkpoint", "o"), ),
                    .out_args =
                        NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("result", "a{su}"), ), ),
                .handle = impl_manager_checkpoint_rollback, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "CheckpointAdjustRollbackTimeout",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("checkpoint", "o"),
                        NM_DEFINE_GDBUS_ARG_INFO("add_timeout", "u"), ), ),
                .handle = impl_manager_checkpoint_adjust_rollback_timeout, ), ),
        .signals    = NM_DEFINE_GDBUS_SIGNAL_INFOS(&signal_info_check_permissions,
                                                &signal_info_state_changed,
                                                &signal_info_device_added,
                                                &signal_info_device_removed, ),
        .properties = NM_DEFINE_GDBUS_PROPERTY_INFOS(
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Devices", "ao", NM_MANAGER_DEVICES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("AllDevices",
                                                           "ao",
                                                           NM_MANAGER_ALL_DEVICES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Checkpoints",
                                                           "ao",
                                                           NM_MANAGER_CHECKPOINTS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("NetworkingEnabled",
                                                           "b",
                                                           NM_MANAGER_NETWORKING_ENABLED),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READWRITABLE(
                "WirelessEnabled",
                "b",
                NM_MANAGER_WIRELESS_ENABLED,
                NM_AUTH_PERMISSION_ENABLE_DISABLE_WIFI,
                NM_AUDIT_OP_RADIO_CONTROL),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("WirelessHardwareEnabled",
                                                           "b",
                                                           NM_MANAGER_WIRELESS_HARDWARE_ENABLED),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READWRITABLE(
                "WwanEnabled",
                "b",
                NM_MANAGER_WWAN_ENABLED,
                NM_AUTH_PERMISSION_ENABLE_DISABLE_WWAN,
                NM_AUDIT_OP_RADIO_CONTROL),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("WwanHardwareEnabled",
                                                           "b",
                                                           NM_MANAGER_WWAN_HARDWARE_ENABLED),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READWRITABLE(
                "WimaxEnabled",
                "b",
                NM_MANAGER_WIMAX_ENABLED,
                NM_AUTH_PERMISSION_ENABLE_DISABLE_WIMAX,
                NM_AUDIT_OP_RADIO_CONTROL,
                .annotations = NM_GDBUS_ANNOTATION_INFO_LIST_DEPRECATED(), ),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE(
                "WimaxHardwareEnabled",
                "b",
                NM_MANAGER_WIMAX_HARDWARE_ENABLED,
                .annotations = NM_GDBUS_ANNOTATION_INFO_LIST_DEPRECATED(), ),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("RadioFlags",
                                                           "u",
                                                           NM_MANAGER_RADIO_FLAGS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("ActiveConnections",
                                                           "ao",
                                                           NM_MANAGER_ACTIVE_CONNECTIONS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("PrimaryConnection",
                                                           "o",
                                                           NM_MANAGER_PRIMARY_CONNECTION),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("PrimaryConnectionType",
                                                           "s",
                                                           NM_MANAGER_PRIMARY_CONNECTION_TYPE),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Metered", "u", NM_MANAGER_METERED),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("ActivatingConnection",
                                                           "o",
                                                           NM_MANAGER_ACTIVATING_CONNECTION),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Startup", "b", NM_MANAGER_STARTUP),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Version", "s", NM_MANAGER_VERSION),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("VersionInfo",
                                                           "au",
                                                           NM_MANAGER_VERSION_INFO),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Capabilities",
                                                           "au",
                                                           NM_MANAGER_CAPABILITIES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("State", "u", NM_MANAGER_STATE),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Connectivity",
                                                           "u",
                                                           NM_MANAGER_CONNECTIVITY),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("ConnectivityCheckAvailable",
                                                           "b",
                                                           NM_MANAGER_CONNECTIVITY_CHECK_AVAILABLE),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READWRITABLE(
                "ConnectivityCheckEnabled",
                "b",
                NM_MANAGER_CONNECTIVITY_CHECK_ENABLED,
                NM_AUTH_PERMISSION_ENABLE_DISABLE_CONNECTIVITY_CHECK,
                NM_AUDIT_OP_NET_CONTROL),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("ConnectivityCheckUri",
                                                           "s",
                                                           NM_MANAGER_CONNECTIVITY_CHECK_URI),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READWRITABLE(
                "GlobalDnsConfiguration",
                "a{sv}",
                NM_MANAGER_GLOBAL_DNS_CONFIGURATION,
                NM_AUTH_PERMISSION_SETTINGS_MODIFY_GLOBAL_DNS,
                NM_AUDIT_OP_NET_CONTROL), ), ),
};

static void
nm_manager_class_init(NMManagerClass *manager_class)
{
    GObjectClass      *object_class      = G_OBJECT_CLASS(manager_class);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(manager_class);

#if WITH_OPENVSWITCH
    /* Use the symbols. These symbols are in NetworkManager binary but will be
     * used by the OVS device plugin. If we don't use the symbol here, it will
     * be wrongly dropped. */
    manager_class->_use_symbol_nm_priv_helper_call_get_fd =
        (void (*)(void)) nm_priv_helper_call_get_fd;
    manager_class->_use_symbol_nm_priv_helper_utils_open_fd =
        (void (*)(void)) nm_priv_helper_utils_open_fd;
#endif

    dbus_object_class->export_path     = NM_DBUS_EXPORT_PATH_STATIC(NM_DBUS_PATH);
    dbus_object_class->interface_infos = NM_DBUS_INTERFACE_INFOS(&interface_info_manager);

    object_class->constructed  = constructed;
    object_class->set_property = set_property;
    object_class->get_property = get_property;
    object_class->dispose      = dispose;
    object_class->finalize     = finalize;

    obj_properties[PROP_VERSION] = g_param_spec_string(NM_MANAGER_VERSION,
                                                       "",
                                                       "",
                                                       NULL,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_VERSION_INFO] =
        g_param_spec_variant(NM_MANAGER_VERSION_INFO,
                             "",
                             "",
                             G_VARIANT_TYPE("au"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CAPABILITIES] =
        g_param_spec_variant(NM_MANAGER_CAPABILITIES,
                             "",
                             "",
                             G_VARIANT_TYPE("au"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_STATE] = g_param_spec_uint(NM_MANAGER_STATE,
                                                   "",
                                                   "",
                                                   0,
                                                   NM_STATE_DISCONNECTED,
                                                   0,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_STARTUP] = g_param_spec_boolean(NM_MANAGER_STARTUP,
                                                        "",
                                                        "",
                                                        TRUE,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_NETWORKING_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_NETWORKING_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WIRELESS_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_WIRELESS_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WIRELESS_HARDWARE_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_WIRELESS_HARDWARE_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WWAN_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_WWAN_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WWAN_HARDWARE_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_WWAN_HARDWARE_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WIMAX_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_WIMAX_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WIMAX_HARDWARE_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_WIMAX_HARDWARE_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_RADIO_FLAGS] = g_param_spec_uint(NM_MANAGER_RADIO_FLAGS,
                                                         "",
                                                         "",
                                                         0,
                                                         G_MAXUINT32,
                                                         NM_RADIO_FLAG_NONE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_ACTIVE_CONNECTIONS] =
        g_param_spec_boxed(NM_MANAGER_ACTIVE_CONNECTIONS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY] =
        g_param_spec_uint(NM_MANAGER_CONNECTIVITY,
                          "",
                          "",
                          NM_CONNECTIVITY_UNKNOWN,
                          NM_CONNECTIVITY_FULL,
                          NM_CONNECTIVITY_UNKNOWN,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_CHECK_AVAILABLE] =
        g_param_spec_boolean(NM_MANAGER_CONNECTIVITY_CHECK_AVAILABLE,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_CHECK_ENABLED] =
        g_param_spec_boolean(NM_MANAGER_CONNECTIVITY_CHECK_ENABLED,
                             "",
                             "",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_CHECK_URI] =
        g_param_spec_string(NM_MANAGER_CONNECTIVITY_CHECK_URI,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_PRIMARY_CONNECTION] =
        g_param_spec_string(NM_MANAGER_PRIMARY_CONNECTION,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_PRIMARY_CONNECTION_TYPE] =
        g_param_spec_string(NM_MANAGER_PRIMARY_CONNECTION_TYPE,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_ACTIVATING_CONNECTION] =
        g_param_spec_string(NM_MANAGER_ACTIVATING_CONNECTION,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /* Sleeping is not exported over D-Bus */
    obj_properties[PROP_SLEEPING] = g_param_spec_boolean(NM_MANAGER_SLEEPING,
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_DEVICES] = g_param_spec_boxed(NM_MANAGER_DEVICES,
                                                      "",
                                                      "",
                                                      G_TYPE_STRV,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * NMManager:metered:
     *
     * Whether the connectivity is metered.
     *
     * Since: 1.2
     **/
    obj_properties[PROP_METERED] = g_param_spec_uint(NM_MANAGER_METERED,
                                                     "",
                                                     "",
                                                     0,
                                                     G_MAXUINT32,
                                                     NM_METERED_UNKNOWN,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * NMManager:global-dns-configuration:
     *
     * The global DNS configuration.
     *
     * Since: 1.2
     **/
    obj_properties[PROP_GLOBAL_DNS_CONFIGURATION] =
        g_param_spec_variant(NM_MANAGER_GLOBAL_DNS_CONFIGURATION,
                             "",
                             "",
                             G_VARIANT_TYPE("a{sv}"),
                             NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * NMManager:all-devices:
     *
     * All devices, including those that are not realized.
     *
     * Since: 1.2
     **/
    obj_properties[PROP_ALL_DEVICES] =
        g_param_spec_boxed(NM_MANAGER_ALL_DEVICES,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CHECKPOINTS] =
        g_param_spec_boxed(NM_MANAGER_CHECKPOINTS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);

    /* signals */

    /* emitted only for realized devices */
    signals[DEVICE_ADDED] = g_signal_new(NM_MANAGER_DEVICE_ADDED,
                                         G_OBJECT_CLASS_TYPE(object_class),
                                         G_SIGNAL_RUN_FIRST,
                                         0,
                                         NULL,
                                         NULL,
                                         NULL,
                                         G_TYPE_NONE,
                                         1,
                                         NM_TYPE_DEVICE);

    /* Emitted for both realized devices and placeholder devices */
    signals[INTERNAL_DEVICE_ADDED] = g_signal_new(NM_MANAGER_INTERNAL_DEVICE_ADDED,
                                                  G_OBJECT_CLASS_TYPE(object_class),
                                                  G_SIGNAL_RUN_FIRST,
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  G_TYPE_NONE,
                                                  1,
                                                  G_TYPE_OBJECT);

    /* emitted only for realized devices when a device
     * becomes unrealized or removed */
    signals[DEVICE_REMOVED] = g_signal_new(NM_MANAGER_DEVICE_REMOVED,
                                           G_OBJECT_CLASS_TYPE(object_class),
                                           G_SIGNAL_RUN_FIRST,
                                           0,
                                           NULL,
                                           NULL,
                                           NULL,
                                           G_TYPE_NONE,
                                           1,
                                           NM_TYPE_DEVICE);

    /* Emitted for both realized devices and placeholder devices */
    signals[INTERNAL_DEVICE_REMOVED] = g_signal_new(NM_MANAGER_INTERNAL_DEVICE_REMOVED,
                                                    G_OBJECT_CLASS_TYPE(object_class),
                                                    G_SIGNAL_RUN_FIRST,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    1,
                                                    G_TYPE_OBJECT);

    signals[ACTIVE_CONNECTION_ADDED] = g_signal_new(NM_MANAGER_ACTIVE_CONNECTION_ADDED,
                                                    G_OBJECT_CLASS_TYPE(object_class),
                                                    G_SIGNAL_RUN_FIRST,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    1,
                                                    NM_TYPE_ACTIVE_CONNECTION);

    signals[ACTIVE_CONNECTION_REMOVED] = g_signal_new(NM_MANAGER_ACTIVE_CONNECTION_REMOVED,
                                                      G_OBJECT_CLASS_TYPE(object_class),
                                                      G_SIGNAL_RUN_FIRST,
                                                      0,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      G_TYPE_NONE,
                                                      1,
                                                      NM_TYPE_ACTIVE_CONNECTION);

    signals[CONFIGURE_QUIT] = g_signal_new(NM_MANAGER_CONFIGURE_QUIT,
                                           G_OBJECT_CLASS_TYPE(object_class),
                                           G_SIGNAL_RUN_FIRST,
                                           0,
                                           NULL,
                                           NULL,
                                           NULL,
                                           G_TYPE_NONE,
                                           0);

    signals[DEVICE_IFINDEX_CHANGED] = g_signal_new(NM_MANAGER_DEVICE_IFINDEX_CHANGED,
                                                   G_OBJECT_CLASS_TYPE(object_class),
                                                   G_SIGNAL_RUN_FIRST,
                                                   0,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   G_TYPE_NONE,
                                                   1,
                                                   NM_TYPE_DEVICE);
}

NMConfig *
nm_manager_get_config(NMManager *self)
{
    NMManagerPrivate *priv = NM_MANAGER_GET_PRIVATE(self);

    return priv->config;
}
