/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2006 - 2013 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-dbus-manager.h"

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "c-list/src/c-list.h"
#include "libnm-glib-aux/nm-c-list.h"
#include "nm-dbus-interface.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-std-aux/nm-dbus-compat.h"
#include "nm-dbus-object.h"
#include "NetworkManagerUtils.h"
#include "libnm-core-aux-intern/nm-auth-subject.h"

/* The base path for our GDBusObjectManagerServers.  They do not contain
 * "NetworkManager" because GDBusObjectManagerServer requires that all
 * exported objects be *below* the base path, and eg the Manager object
 * is the base path already.
 */
#define OBJECT_MANAGER_SERVER_BASE_PATH "/org/freedesktop"

/*****************************************************************************/

typedef struct {
    CList  caller_info_lst;
    gulong uid;
    gulong pid;
    gint64 uid_checked_at;
    gint64 pid_checked_at;
    bool   uid_valid : 1;
    bool   pid_valid : 1;
    char   sender[0];
} CallerInfo;

typedef struct {
    GVariant *value;
} PropertyCacheData;

typedef struct {
    CList              registration_lst;
    NMDBusObject      *obj;
    NMDBusObjectClass *klass;
    guint              info_idx;
    guint              registration_id;
    PropertyCacheData  property_cache[];
} RegistrationData;

/* we require that @path is the first member of NMDBusManagerData
 * because _objects_by_path_hash() requires that. */
G_STATIC_ASSERT(G_STRUCT_OFFSET(struct _NMDBusObjectInternal, path) == 0);

enum {
    PRIVATE_CONNECTION_NEW,
    PRIVATE_CONNECTION_DISCONNECTED,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef struct {
    GHashTable *objects_by_path;
    CList       objects_lst_head;

    CList private_servers_lst_head;

    NMDBusManagerSetPropertyHandler set_property_handler;
    gpointer                        set_property_handler_data;

    GDBusConnection *main_dbus_connection;

    CList caller_info_lst_head;

    guint objmgr_registration_id;
    bool  started : 1;
    bool  shutting_down : 1;
} NMDBusManagerPrivate;

struct _NMDBusManager {
    GObject              parent;
    NMDBusManagerPrivate _priv;
};

struct _NMDBusManagerClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMDBusManager, nm_dbus_manager, G_TYPE_OBJECT)

#define NM_DBUS_MANAGER_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMDBusManager, NM_IS_DBUS_MANAGER)

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_CORE
#define _NMLOG(level, ...) __NMLOG_DEFAULT(level, _NMLOG_DOMAIN, "bus-manager", __VA_ARGS__)

NM_DEFINE_SINGLETON_GETTER(NMDBusManager, nm_dbus_manager_get, NM_TYPE_DBUS_MANAGER);

/*****************************************************************************/

static const GDBusInterfaceInfo interface_info_objmgr;
static const GDBusSignalInfo    signal_info_objmgr_interfaces_added;
static const GDBusSignalInfo    signal_info_objmgr_interfaces_removed;
static GVariantBuilder *_obj_collect_properties_all(NMDBusObject *obj, GVariantBuilder *builder);

/*****************************************************************************/

static guint
_objects_by_path_hash(gconstpointer user_data)
{
    const char *const *p_data = user_data;

    nm_assert(p_data);
    nm_assert(*p_data);
    nm_assert((*p_data)[0] == '/');

    return nm_hash_str(*p_data);
}

static gboolean
_objects_by_path_equal(gconstpointer user_data_a, gconstpointer user_data_b)
{
    const char *const *p_data_a = user_data_a;
    const char *const *p_data_b = user_data_b;

    nm_assert(p_data_a);
    nm_assert(*p_data_a);
    nm_assert((*p_data_a)[0] == '/');
    nm_assert(p_data_b);
    nm_assert(*p_data_b);
    nm_assert((*p_data_b)[0] == '/');

    return nm_streq(*p_data_a, *p_data_b);
}

/*****************************************************************************/

typedef struct {
    CList private_servers_lst;

    const char  *tag;
    GQuark       detail;
    char        *address;
    GDBusServer *server;

    /* With peer bus connections, we'll get a new connection for each
     * client.  For each connection we create an ObjectManager for
     * that connection to handle exporting our objects.
     *
     * Note that even for connections that don't export any objects
     * we'll still create GDBusObjectManager since that's where we store
     * the pointer to the GDBusConnection.
     */
    CList object_mgr_lst_head;

    NMDBusManager *manager;
} PrivateServer;

typedef struct {
    CList                     object_mgr_lst;
    GDBusObjectManagerServer *manager;
    char                     *fake_sender;
} ObjectMgrData;

typedef struct {
    GDBusConnection *connection;
    PrivateServer   *server;
    gboolean         remote_peer_vanished;
} CloseConnectionInfo;

/*****************************************************************************/

static void
_object_mgr_data_free(ObjectMgrData *obj_mgr_data)
{
    GDBusConnection *connection;

    c_list_unlink_stale(&obj_mgr_data->object_mgr_lst);

    connection = g_dbus_object_manager_server_get_connection(obj_mgr_data->manager);
    if (!g_dbus_connection_is_closed(connection))
        g_dbus_connection_close(connection, NULL, NULL, NULL);
    g_dbus_object_manager_server_set_connection(obj_mgr_data->manager, NULL);
    g_object_unref(obj_mgr_data->manager);
    g_object_unref(connection);

    g_free(obj_mgr_data->fake_sender);

    g_slice_free(ObjectMgrData, obj_mgr_data);
}

/*****************************************************************************/

static gboolean
close_connection_in_idle(gpointer user_data)
{
    CloseConnectionInfo *info   = user_data;
    PrivateServer       *server = info->server;
    ObjectMgrData       *obj_mgr_data, *obj_mgr_data_safe;

    /* Emit this for the manager */
    g_signal_emit(server->manager,
                  signals[PRIVATE_CONNECTION_DISCONNECTED],
                  server->detail,
                  info->connection);

    /* FIXME: there's a bug (754730) in GLib for which the connection
     * is marked as closed when the remote peer vanishes but its
     * resources are not cleaned up.  Work around it by explicitly
     * closing the connection in that case. */
    if (info->remote_peer_vanished)
        g_dbus_connection_close(info->connection, NULL, NULL, NULL);

    c_list_for_each_entry_safe (obj_mgr_data,
                                obj_mgr_data_safe,
                                &server->object_mgr_lst_head,
                                object_mgr_lst) {
        gs_unref_object GDBusConnection *connection = NULL;

        connection = g_dbus_object_manager_server_get_connection(obj_mgr_data->manager);
        if (connection == info->connection) {
            _object_mgr_data_free(obj_mgr_data);
            break;
        }
    }

    g_object_unref(server->manager);
    g_slice_free(CloseConnectionInfo, info);

    return G_SOURCE_REMOVE;
}

static void
private_server_closed_connection(GDBusConnection *conn,
                                 gboolean         remote_peer_vanished,
                                 GError          *error,
                                 gpointer         user_data)
{
    PrivateServer       *s = user_data;
    CloseConnectionInfo *info;

    /* Clean up after the connection */
    _LOGD("(%s) closed connection " NM_HASH_OBFUSCATE_PTR_FMT " on private socket",
          s->tag,
          NM_HASH_OBFUSCATE_PTR(conn));

    info                       = g_slice_new0(CloseConnectionInfo);
    info->connection           = conn;
    info->server               = s;
    info->remote_peer_vanished = remote_peer_vanished;

    g_object_ref(s->manager);

    /* Delay the close of connection to ensure that D-Bus signals
     * are handled */
    nm_g_idle_add(close_connection_in_idle, info);
}

static gboolean
private_server_new_connection(GDBusServer *server, GDBusConnection *conn, gpointer user_data)
{
    PrivateServer            *s = user_data;
    ObjectMgrData            *obj_mgr_data;
    static guint32            counter = 0;
    GDBusObjectManagerServer *manager;
    char                     *sender;

    g_signal_connect(conn, "closed", G_CALLBACK(private_server_closed_connection), s);

    /* Fake a sender since private connections don't have one */
    sender = g_strdup_printf("x:y:%d", counter++);

    manager = g_dbus_object_manager_server_new(OBJECT_MANAGER_SERVER_BASE_PATH);
    g_dbus_object_manager_server_set_connection(manager, conn);

    obj_mgr_data              = g_slice_new(ObjectMgrData);
    obj_mgr_data->manager     = manager;
    obj_mgr_data->fake_sender = sender;
    c_list_link_tail(&s->object_mgr_lst_head, &obj_mgr_data->object_mgr_lst);

    _LOGD("(%s) accepted connection " NM_HASH_OBFUSCATE_PTR_FMT " on private socket",
          s->tag,
          NM_HASH_OBFUSCATE_PTR(conn));

    /* Emit this for the manager.
     *
     * It is essential to do this from the "new-connection" signal handler, as
     * at that point no messages from the connection are yet processed
     * (which avoids races with registering objects). */
    g_signal_emit(s->manager, signals[PRIVATE_CONNECTION_NEW], s->detail, conn, manager);
    return TRUE;
}

static gboolean
private_server_authorize(GDBusAuthObserver *observer,
                         GIOStream         *stream,
                         GCredentials      *credentials,
                         gpointer           user_data)
{
    return g_credentials_get_unix_user(credentials, NULL) == 0;
}

static gboolean
private_server_allow_mechanism(GDBusAuthObserver *observer,
                               const char        *mechanism,
                               gpointer           user_data)
{
    return NM_IN_STRSET(mechanism, "EXTERNAL");
}

static void
private_server_free(gpointer ptr)
{
    PrivateServer *s = ptr;
    ObjectMgrData *obj_mgr_data, *obj_mgr_data_safe;

    c_list_unlink_stale(&s->private_servers_lst);

    unlink(s->address);
    g_free(s->address);

    c_list_for_each_entry_safe (obj_mgr_data,
                                obj_mgr_data_safe,
                                &s->object_mgr_lst_head,
                                object_mgr_lst)
        _object_mgr_data_free(obj_mgr_data);

    g_dbus_server_stop(s->server);

    g_signal_handlers_disconnect_by_func(s->server, G_CALLBACK(private_server_new_connection), s);

    g_object_unref(s->server);

    g_slice_free(PrivateServer, s);
}

void
nm_dbus_manager_private_server_register(NMDBusManager *self, const char *path, const char *tag)
{
    NMDBusManagerPrivate              *priv;
    PrivateServer                     *s;
    gs_unref_object GDBusAuthObserver *auth_observer = NULL;
    GDBusServer                       *server;
    GError                            *error   = NULL;
    gs_free char                      *address = NULL;
    gs_free char                      *guid    = NULL;

    g_return_if_fail(NM_IS_DBUS_MANAGER(self));
    g_return_if_fail(path);
    g_return_if_fail(tag);

    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    /* Only one instance per tag; but don't warn */
    c_list_for_each_entry (s, &priv->private_servers_lst_head, private_servers_lst) {
        if (nm_streq0(tag, s->tag))
            return;
    }

    unlink(path);
    address = g_strdup_printf("unix:path=%s", path);

    _LOGD("(%s) creating private socket %s", tag, address);

    guid          = g_dbus_generate_guid();
    auth_observer = g_dbus_auth_observer_new();
    g_signal_connect(auth_observer,
                     "authorize-authenticated-peer",
                     G_CALLBACK(private_server_authorize),
                     NULL);
    g_signal_connect(auth_observer,
                     "allow-mechanism",
                     G_CALLBACK(private_server_allow_mechanism),
                     NULL);
    server = g_dbus_server_new_sync(address,
                                    G_DBUS_SERVER_FLAGS_NONE,
                                    guid,
                                    auth_observer,
                                    NULL,
                                    &error);

    if (!server) {
        _LOGW("(%s) failed to set up private socket %s: %s", tag, address, error->message);
        g_error_free(error);
        return;
    }

    s          = g_slice_new0(PrivateServer);
    s->address = g_steal_pointer(&address);
    s->server  = server;
    g_signal_connect(server, "new-connection", G_CALLBACK(private_server_new_connection), s);

    c_list_init(&s->object_mgr_lst_head);

    s->manager = self;
    s->detail  = g_quark_from_string(tag);
    s->tag     = g_quark_to_string(s->detail);

    c_list_link_tail(&priv->private_servers_lst_head, &s->private_servers_lst);

    g_dbus_server_start(server);
}

static const char *
private_server_get_connection_owner(PrivateServer *s, GDBusConnection *connection)
{
    ObjectMgrData *obj_mgr_data;

    nm_assert(s);
    nm_assert(G_IS_DBUS_CONNECTION(connection));

    c_list_for_each_entry (obj_mgr_data, &s->object_mgr_lst_head, object_mgr_lst) {
        gs_unref_object GDBusConnection *c = NULL;

        c = g_dbus_object_manager_server_get_connection(obj_mgr_data->manager);
        if (c == connection)
            return obj_mgr_data->fake_sender;
    }
    return NULL;
}

static GDBusConnection *
private_server_get_connection_by_owner(PrivateServer *s, const char *owner)
{
    ObjectMgrData *obj_mgr_data;

    nm_assert(s);
    nm_assert(owner);

    c_list_for_each_entry (obj_mgr_data, &s->object_mgr_lst_head, object_mgr_lst) {
        if (nm_streq(owner, obj_mgr_data->fake_sender))
            return g_dbus_object_manager_server_get_connection(obj_mgr_data->manager);
    }
    return NULL;
}

/*****************************************************************************/

static void
_caller_info_free(CallerInfo *caller_info)
{
    c_list_unlink_stale(&caller_info->caller_info_lst);
    g_free(caller_info);
}

static gboolean
_bus_get_unix_pid(NMDBusManager *self, const char *sender, gulong *out_pid)
{
    NMDBusManagerPrivate      *priv     = NM_DBUS_MANAGER_GET_PRIVATE(self);
    guint32                    unix_pid = G_MAXUINT32;
    gs_unref_variant GVariant *ret      = NULL;

    if (!priv->main_dbus_connection)
        return FALSE;

    ret = g_dbus_connection_call_sync(priv->main_dbus_connection,
                                      DBUS_SERVICE_DBUS,
                                      DBUS_PATH_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "GetConnectionUnixProcessID",
                                      g_variant_new("(s)", sender),
                                      G_VARIANT_TYPE("(u)"),
                                      G_DBUS_CALL_FLAGS_NONE,
                                      2000,
                                      NULL,
                                      NULL);
    if (!ret)
        return FALSE;

    g_variant_get(ret, "(u)", &unix_pid);

    *out_pid = (gulong) unix_pid;
    return TRUE;
}

static gboolean
_bus_get_unix_user(NMDBusManager *self, const char *sender, gulong *out_user)
{
    NMDBusManagerPrivate      *priv     = NM_DBUS_MANAGER_GET_PRIVATE(self);
    guint32                    unix_uid = G_MAXUINT32;
    gs_unref_variant GVariant *ret      = NULL;

    if (!priv->main_dbus_connection)
        return FALSE;

    ret = g_dbus_connection_call_sync(priv->main_dbus_connection,
                                      DBUS_SERVICE_DBUS,
                                      DBUS_PATH_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "GetConnectionUnixUser",
                                      g_variant_new("(s)", sender),
                                      G_VARIANT_TYPE("(u)"),
                                      G_DBUS_CALL_FLAGS_NONE,
                                      2000,
                                      NULL,
                                      NULL);
    if (!ret)
        return FALSE;

    g_variant_get(ret, "(u)", &unix_uid);

    *out_user = (gulong) unix_uid;
    return TRUE;
}

static const CallerInfo *
_get_caller_info_ensure(NMDBusManager *self,
                        const char    *sender,
                        gboolean       ensure_uid,
                        gboolean       ensure_pid)
{
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    CallerInfo           *caller_info;
    CallerInfo           *ci;
    gint64                now_ns;
    gsize                 num;

#define CALLER_INFO_MAX_AGE (NM_UTILS_NSEC_PER_SEC * 1)

    /* Linear search the cache for the sender.
     *
     * The number of cached caller-infos is limited. Hence, it's O(1) and
     * the list is reasonably short.
     * Also, the entire caching assumes that we repeatedly ask for the
     * same sender. That means, we expect to find the right caller info
     * at the front of the list. */
    num         = 1;
    caller_info = NULL;
    c_list_for_each_entry (ci, &priv->caller_info_lst_head, caller_info_lst) {
        if (nm_streq(sender, ci->sender)) {
            caller_info = ci;
            break;
        }
        num++;
    }

    if (caller_info)
        nm_c_list_move_front(&priv->caller_info_lst_head, &caller_info->caller_info_lst);
    else {
        gsize l = strlen(sender) + 1;

        caller_info  = g_malloc(sizeof(CallerInfo) + l);
        *caller_info = (CallerInfo) {
            .uid_checked_at = -CALLER_INFO_MAX_AGE,
            .pid_checked_at = -CALLER_INFO_MAX_AGE,
        };
        memcpy(caller_info->sender, sender, l);
        c_list_link_front(&priv->caller_info_lst_head, &caller_info->caller_info_lst);

        /* only cache the last few entries. */
        while (TRUE) {
            nm_assert(num > 0 && num == c_list_length(&priv->caller_info_lst_head));
            if (num-- <= 5)
                break;
            _caller_info_free(
                c_list_last_entry(&priv->caller_info_lst_head, CallerInfo, caller_info_lst));
        }
    }

    now_ns = nm_utils_get_monotonic_timestamp_nsec();

    if (ensure_uid && (now_ns - caller_info->uid_checked_at) > CALLER_INFO_MAX_AGE) {
        caller_info->uid_checked_at = now_ns;
        if (!(caller_info->uid_valid = _bus_get_unix_user(self, sender, &caller_info->uid)))
            caller_info->uid = G_MAXULONG;
    }

    if (ensure_pid && (now_ns - caller_info->pid_checked_at) > CALLER_INFO_MAX_AGE) {
        caller_info->pid_checked_at = now_ns;
        if (!(caller_info->pid_valid = _bus_get_unix_pid(self, sender, &caller_info->pid)))
            caller_info->pid = G_MAXULONG;
    }

    return caller_info;
}

static gboolean
_get_caller_info(NMDBusManager         *self,
                 GDBusMethodInvocation *context,
                 GDBusConnection       *connection,
                 GDBusMessage          *message,
                 const char           **out_sender,
                 gulong                *out_uid,
                 gulong                *out_pid)
{
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    const CallerInfo     *caller_info;
    const char           *sender;

    if (context) {
        nm_assert(G_IS_DBUS_METHOD_INVOCATION(context));
        connection = g_dbus_method_invocation_get_connection(context);

        /* only bus connections will have a sender */
        sender = g_dbus_method_invocation_get_sender(context);
    } else {
        nm_assert(G_IS_DBUS_MESSAGE(message));
        sender = g_dbus_message_get_sender(message);
    }
    nm_assert(G_IS_DBUS_CONNECTION(connection));

    if (!sender) {
        PrivateServer *s;

        /* Might be a private connection, for which we fake a sender */
        c_list_for_each_entry (s, &priv->private_servers_lst_head, private_servers_lst) {
            sender = private_server_get_connection_owner(s, connection);
            if (sender) {
                NM_SET_OUT(out_uid, 0);
                NM_SET_OUT(out_sender, sender);
                if (out_pid) {
                    GCredentials *creds;

                    creds = g_dbus_connection_get_peer_credentials(connection);
                    if (creds) {
                        pid_t pid;

                        pid = g_credentials_get_unix_pid(creds, NULL);
                        if (pid == -1)
                            *out_pid = G_MAXULONG;
                        else
                            *out_pid = pid;
                    } else
                        *out_pid = G_MAXULONG;
                }
                return TRUE;
            }
        }
        NM_SET_OUT(out_sender, NULL);
        NM_SET_OUT(out_uid, G_MAXULONG);
        NM_SET_OUT(out_pid, G_MAXULONG);
        return FALSE;
    }

    caller_info = _get_caller_info_ensure(self, sender, !!out_uid, !!out_pid);

    NM_SET_OUT(out_sender, caller_info->sender);
    NM_SET_OUT(out_uid, caller_info->uid);
    NM_SET_OUT(out_pid, caller_info->pid);

    if (out_uid && !caller_info->uid_valid)
        return FALSE;
    if (out_pid && !caller_info->pid_valid)
        return FALSE;
    return TRUE;
}

gboolean
nm_dbus_manager_get_caller_info(NMDBusManager         *self,
                                GDBusMethodInvocation *context,
                                const char           **out_sender,
                                gulong                *out_uid,
                                gulong                *out_pid)
{
    return _get_caller_info(self, context, NULL, NULL, out_sender, out_uid, out_pid);
}

gboolean
nm_dbus_manager_get_caller_info_from_message(NMDBusManager   *self,
                                             GDBusConnection *connection,
                                             GDBusMessage    *message,
                                             const char     **out_sender,
                                             gulong          *out_uid,
                                             gulong          *out_pid)
{
    return _get_caller_info(self, NULL, connection, message, out_sender, out_uid, out_pid);
}

/**
 * nm_dbus_manager_ensure_uid:
 *
 * @self: bus manager instance
 * @context: D-Bus method invocation
 * @uid: a user-id
 * @error_domain: error domain to return on failure
 * @error_code: error code to return on failure
 *
 * Retrieves the uid of the D-Bus method caller and
 * checks that it matches @uid, unless @uid is G_MAXULONG.
 * In case of failure the function returns FALSE and finishes
 * handling the D-Bus method with an error.
 *
 * Returns: %TRUE if the check succeeded, %FALSE otherwise
 */
gboolean
nm_dbus_manager_ensure_uid(NMDBusManager         *self,
                           GDBusMethodInvocation *context,
                           gulong                 uid,
                           GQuark                 error_domain,
                           int                    error_code)
{
    gulong  caller_uid;
    GError *error = NULL;

    g_return_val_if_fail(NM_IS_DBUS_MANAGER(self), FALSE);
    g_return_val_if_fail(G_IS_DBUS_METHOD_INVOCATION(context), FALSE);

    if (!nm_dbus_manager_get_caller_info(self, context, NULL, &caller_uid, NULL)) {
        error = g_error_new_literal(error_domain, error_code, "Unable to determine request UID.");
        g_dbus_method_invocation_take_error(context, error);
        return FALSE;
    }

    if (uid != G_MAXULONG && caller_uid != uid) {
        error = g_error_new_literal(error_domain, error_code, "Permission denied");
        g_dbus_method_invocation_take_error(context, error);
        return FALSE;
    }

    return TRUE;
}

gboolean
nm_dbus_manager_get_unix_user(NMDBusManager *self, const char *sender, gulong *out_uid)
{
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    const CallerInfo     *caller_info;
    PrivateServer        *s;

    g_return_val_if_fail(sender != NULL, FALSE);
    g_return_val_if_fail(out_uid != NULL, FALSE);

    /* Check if it's a private connection sender, which we fake */
    c_list_for_each_entry (s, &priv->private_servers_lst_head, private_servers_lst) {
        gs_unref_object GDBusConnection *connection = NULL;

        connection = private_server_get_connection_by_owner(s, sender);
        if (connection) {
            *out_uid = 0;
            return TRUE;
        }
    }

    /* Otherwise, a bus connection */
    caller_info = _get_caller_info_ensure(self, sender, TRUE, FALSE);
    *out_uid    = caller_info->uid;
    if (!caller_info->uid_valid) {
        _LOGW("failed to get unix user for dbus sender '%s'", sender);
        return FALSE;
    }
    return TRUE;
}

/*****************************************************************************/

static const NMDBusInterfaceInfoExtended *
_reg_data_get_interface_info(RegistrationData *reg_data)
{
    nm_assert(reg_data);

    return reg_data->klass->interface_infos[reg_data->info_idx];
}

/*****************************************************************************/

static void
dbus_vtable_method_call(GDBusConnection       *connection,
                        const char            *sender,
                        const char            *object_path,
                        const char            *interface_name,
                        const char            *method_name,
                        GVariant              *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
{
    NMDBusManager                     *self;
    NMDBusManagerPrivate              *priv;
    RegistrationData                  *reg_data       = user_data;
    NMDBusObject                      *obj            = reg_data->obj;
    const NMDBusInterfaceInfoExtended *interface_info = _reg_data_get_interface_info(reg_data);
    const NMDBusMethodInfoExtended    *method_info    = NULL;
    gboolean                           on_same_interface;

    on_same_interface = nm_streq(interface_info->parent.name, interface_name);

    /* handle property setter first... */
    if (!on_same_interface && nm_streq(interface_name, DBUS_INTERFACE_PROPERTIES)
        && nm_streq(method_name, "Set")) {
        const NMDBusPropertyInfoExtended *property_info = NULL;
        const char                       *property_interface;
        const char                       *property_name;
        gs_unref_variant GVariant        *value = NULL;

        self = nm_dbus_object_get_manager(obj);
        priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

        g_variant_get(parameters, "(&s&sv)", &property_interface, &property_name, &value);

        nm_assert(nm_streq(property_interface, interface_info->parent.name));

        property_info =
            (const NMDBusPropertyInfoExtended *) nm_dbus_utils_interface_info_lookup_property(
                &interface_info->parent,
                property_name,
                NULL);
        if (!property_info
            || !NM_FLAGS_HAS(property_info->parent.flags, G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE))
            g_return_if_reached();

        if (!priv->set_property_handler) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR,
                                                  G_DBUS_ERROR_AUTH_FAILED,
                                                  "Cannot authenticate setting property %s",
                                                  property_name);
            return;
        }

        priv->set_property_handler(obj,
                                   interface_info,
                                   property_info,
                                   connection,
                                   sender,
                                   invocation,
                                   value,
                                   priv->set_property_handler_data);
        return;
    }

    if (on_same_interface) {
        method_info = (const NMDBusMethodInfoExtended *) nm_dbus_utils_interface_info_lookup_method(
            &interface_info->parent,
            method_name);
    }
    if (!method_info) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method %s",
                                              method_name);
        return;
    }

    self = nm_dbus_object_get_manager(obj);
    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    if (priv->shutting_down && !method_info->allow_during_shutdown) {
        g_dbus_method_invocation_return_error_literal(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_FAILED,
                                                      "NetworkManager is exiting");
        return;
    }

    method_info->handle(reg_data->obj,
                        interface_info,
                        method_info,
                        connection,
                        sender,
                        invocation,
                        parameters);
}

static GVariant *
_obj_get_property(RegistrationData *reg_data, guint property_idx, gboolean refetch)
{
    const NMDBusInterfaceInfoExtended *interface_info = _reg_data_get_interface_info(reg_data);
    const NMDBusPropertyInfoExtended  *property_info;
    GVariant                          *value;

    property_info =
        (const NMDBusPropertyInfoExtended *) (interface_info->parent.properties[property_idx]);

    if (refetch)
        nm_clear_g_variant(&reg_data->property_cache[property_idx].value);
    else {
        value = reg_data->property_cache[property_idx].value;
        if (value)
            goto out;
    }

    value = nm_dbus_utils_get_property(G_OBJECT(reg_data->obj),
                                       property_info->parent.signature,
                                       property_info->property_name);
    reg_data->property_cache[property_idx].value = value;
out:
    return g_variant_ref(value);
}

static GVariant *
dbus_vtable_get_property(GDBusConnection *connection,
                         const char      *sender,
                         const char      *object_path,
                         const char      *interface_name,
                         const char      *property_name,
                         GError         **error,
                         gpointer         user_data)
{
    RegistrationData                  *reg_data       = user_data;
    const NMDBusInterfaceInfoExtended *interface_info = _reg_data_get_interface_info(reg_data);
    guint                              property_idx;

    if (!nm_dbus_utils_interface_info_lookup_property(&interface_info->parent,
                                                      property_name,
                                                      &property_idx))
        g_return_val_if_reached(NULL);

    return _obj_get_property(reg_data, property_idx, FALSE);
}

static const GDBusInterfaceVTable dbus_vtable = {
    .method_call  = dbus_vtable_method_call,
    .get_property = dbus_vtable_get_property,

    /* set_property is handled via method_call as well. We need to authenticate
     * which requires an asynchronous handler. */
    .set_property = NULL,
};

static void
_obj_register(NMDBusManager *self, NMDBusObject *obj)
{
    NMDBusManagerPrivate                     *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    guint                                     i, k;
    guint                                     n_klasses;
    GType                                     gtype;
    NMDBusObjectClass                        *klasses[10];
    const NMDBusInterfaceInfoExtended *const *prev_interface_infos = NULL;
    GVariantBuilder                           builder;

    nm_assert(c_list_is_empty(&obj->internal.registration_lst_head));
    nm_assert(priv->main_dbus_connection);
    nm_assert(priv->objmgr_registration_id != 0);
    nm_assert(priv->started);

    n_klasses = 0;
    gtype     = G_OBJECT_TYPE(obj);
    while (gtype != NM_TYPE_DBUS_OBJECT) {
        nm_assert(n_klasses < G_N_ELEMENTS(klasses));
        klasses[n_klasses++] = g_type_class_ref(gtype);
        gtype                = g_type_parent(gtype);
    }

    for (k = n_klasses; k > 0;) {
        NMDBusObjectClass *klass = NM_DBUS_OBJECT_CLASS(klasses[--k]);

        if (!klass->interface_infos)
            continue;

        if (prev_interface_infos == klass->interface_infos) {
            /* derived classes inherrit the interface-infos from the parent class.
             * For convenience, we allow the subclass to leave interface-infos untouched,
             * but it means we must ignore the parent's interface, because we already
             * handled it.
             *
             * Note that the loop goes from the parent classes to child classes */
            continue;
        }
        prev_interface_infos = klass->interface_infos;

        for (i = 0; klass->interface_infos[i]; i++) {
            const NMDBusInterfaceInfoExtended *interface_info = klass->interface_infos[i];
            RegistrationData                  *reg_data;
            gs_free_error GError              *error = NULL;
            guint                              registration_id;
            guint prop_len = NM_PTRARRAY_LEN(interface_info->parent.properties);

            reg_data = g_malloc0(sizeof(RegistrationData) + (sizeof(PropertyCacheData) * prop_len));

            registration_id = g_dbus_connection_register_object(
                priv->main_dbus_connection,
                obj->internal.path,
                NM_UNCONST_PTR(GDBusInterfaceInfo, &interface_info->parent),
                &dbus_vtable,
                reg_data,
                NULL,
                &error);
            if (!registration_id) {
                _LOGE("failure to register object %s: %s", obj->internal.path, error->message);
                g_free(reg_data);
                continue;
            }

            reg_data->obj             = obj;
            reg_data->klass           = g_type_class_ref(G_TYPE_FROM_CLASS(klass));
            reg_data->info_idx        = i;
            reg_data->registration_id = registration_id;
            c_list_link_tail(&obj->internal.registration_lst_head, &reg_data->registration_lst);
        }
    }

    for (k = 0; k < n_klasses; k++)
        g_type_class_unref(klasses[k]);

    nm_assert(!c_list_is_empty(&obj->internal.registration_lst_head));

    /* Currently, the interfaces of an object do not changed and strictly depend on the object glib type.
     * We don't need more flexibility, and it simplifies the code. Hence, now emit interface-added
     * signal for the new object.
     *
     * Warning: note that if @obj's notify signal is currently blocked via g_object_freeze_notify(),
     * we might emit properties with an inconsistent (internal) state. There is no easy solution,
     * because we have to emit the signal now, and we don't know what the correct desired state
     * of the properties is.
     * Another problem is, upon unfreezing the signals, we immediately send PropertiesChanged
     * notifications out. Which is a bit odd, as we just export the object.
     *
     * In general, it's ok to export an object with frozen signals. But you better make sure
     * that all properties are in a self-consistent state when exporting the object. */
    g_dbus_connection_emit_signal(priv->main_dbus_connection,
                                  NULL,
                                  OBJECT_MANAGER_SERVER_BASE_PATH,
                                  interface_info_objmgr.name,
                                  signal_info_objmgr_interfaces_added.name,
                                  g_variant_new("(oa{sa{sv}})",
                                                obj->internal.path,
                                                _obj_collect_properties_all(obj, &builder)),
                                  NULL);
}

static void
_obj_unregister(NMDBusManager *self, NMDBusObject *obj)
{
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    RegistrationData     *reg_data;
    GVariantBuilder       builder;

    nm_assert(NM_IS_DBUS_OBJECT(obj));
    nm_assert(priv->main_dbus_connection);
    nm_assert(priv->objmgr_registration_id != 0);
    nm_assert(priv->started);
    nm_assert(!c_list_is_empty(&obj->internal.registration_lst_head));

    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

    while ((reg_data = c_list_last_entry(&obj->internal.registration_lst_head,
                                         RegistrationData,
                                         registration_lst))) {
        const NMDBusInterfaceInfoExtended *interface_info = _reg_data_get_interface_info(reg_data);
        guint                              i;

        g_variant_builder_add(&builder, "s", interface_info->parent.name);
        c_list_unlink_stale(&reg_data->registration_lst);
        if (!g_dbus_connection_unregister_object(priv->main_dbus_connection,
                                                 reg_data->registration_id))
            nm_assert_not_reached();

        if (interface_info->parent.properties) {
            for (i = 0; interface_info->parent.properties[i]; i++)
                nm_clear_g_variant(&reg_data->property_cache[i].value);
        }

        g_type_class_unref(reg_data->klass);
        g_free(reg_data);
    }

    g_dbus_connection_emit_signal(priv->main_dbus_connection,
                                  NULL,
                                  OBJECT_MANAGER_SERVER_BASE_PATH,
                                  interface_info_objmgr.name,
                                  signal_info_objmgr_interfaces_removed.name,
                                  g_variant_new("(oas)", obj->internal.path, &builder),
                                  NULL);
}

gpointer
nm_dbus_manager_lookup_object(NMDBusManager *self, const char *path)
{
    NMDBusManagerPrivate *priv;
    gpointer              ptr;
    NMDBusObject         *obj;

    g_return_val_if_fail(NM_IS_DBUS_MANAGER(self), NULL);
    g_return_val_if_fail(path, NULL);

    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    ptr = g_hash_table_lookup(priv->objects_by_path, &path);
    if (!ptr)
        return NULL;

    obj = NM_CAST_ALIGN(NMDBusObject, (((char *) ptr) - G_STRUCT_OFFSET(NMDBusObject, internal)));
    nm_assert(NM_IS_DBUS_OBJECT(obj));
    return obj;
}

gpointer
nm_dbus_manager_lookup_object_with_type(NMDBusManager *self, GType gtype, const char *path)
{
    gpointer ptr;

    nm_assert(g_type_is_a(gtype, NM_TYPE_DBUS_OBJECT));
    nm_assert(gtype != NM_TYPE_DBUS_OBJECT);

    ptr = nm_dbus_manager_lookup_object(self, path);
    if (!ptr || !G_TYPE_CHECK_INSTANCE_TYPE(ptr, gtype))
        return NULL;

    return ptr;
}

void
_nm_dbus_manager_obj_export(NMDBusObject *obj)
{
    NMDBusManager        *self;
    NMDBusManagerPrivate *priv;

    g_return_if_fail(NM_IS_DBUS_OBJECT(obj));
    g_return_if_fail(obj->internal.path);
    g_return_if_fail(NM_IS_DBUS_MANAGER(obj->internal.bus_manager));
    g_return_if_fail(c_list_is_empty(&obj->internal.objects_lst));
    nm_assert(c_list_is_empty(&obj->internal.registration_lst_head));

    self = obj->internal.bus_manager;
    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    if (!g_hash_table_add(priv->objects_by_path, &obj->internal))
        nm_assert_not_reached();
    c_list_link_tail(&priv->objects_lst_head, &obj->internal.objects_lst);

    if (priv->started)
        _obj_register(self, obj);
}

void
_nm_dbus_manager_obj_unexport(NMDBusObject *obj)
{
    NMDBusManager        *self;
    NMDBusManagerPrivate *priv;

    g_return_if_fail(NM_IS_DBUS_OBJECT(obj));
    g_return_if_fail(obj->internal.path);
    g_return_if_fail(NM_IS_DBUS_MANAGER(obj->internal.bus_manager));
    g_return_if_fail(!c_list_is_empty(&obj->internal.objects_lst));

    self = obj->internal.bus_manager;
    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    nm_assert(&obj->internal == g_hash_table_lookup(priv->objects_by_path, &obj->internal));
    nm_assert(c_list_contains(&priv->objects_lst_head, &obj->internal.objects_lst));

    if (priv->started)
        _obj_unregister(self, obj);
    else
        nm_assert(c_list_is_empty(&obj->internal.registration_lst_head));

    if (!g_hash_table_remove(priv->objects_by_path, &obj->internal))
        nm_assert_not_reached();
    c_list_unlink(&obj->internal.objects_lst);
}

void
_nm_dbus_manager_obj_notify(NMDBusObject *obj, guint n_pspecs, const GParamSpec *const *pspecs)
{
    NMDBusManager        *self;
    NMDBusManagerPrivate *priv;
    RegistrationData     *reg_data;
    guint                 i, p;

    nm_assert(NM_IS_DBUS_OBJECT(obj));
    nm_assert(obj->internal.path);
    nm_assert(NM_IS_DBUS_MANAGER(obj->internal.bus_manager));
    nm_assert(!c_list_is_empty(&obj->internal.objects_lst));

    self = obj->internal.bus_manager;
    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    nm_assert(!priv->started || priv->objmgr_registration_id != 0);
    nm_assert(priv->objmgr_registration_id == 0 || priv->main_dbus_connection);
    nm_assert(c_list_is_empty(&obj->internal.registration_lst_head) != priv->started);

    if (G_UNLIKELY(!priv->started))
        return;

    /* do a naive search for the matching NMDBusPropertyInfoExtended infos. Since the number of
     * (interfaces x properties) is static and possibly small, this naive search is effectively
     * O(1). We might wanna introduce some index to lookup the properties in question faster.
     *
     * The nice part of this implementation is however, that the order in which properties
     * are added to the GVariant is strictly defined to be the order in which the D-Bus property-info
     * is declared. Getting a defined ordering with some smart lookup would be hard. */
    c_list_for_each_entry (reg_data, &obj->internal.registration_lst_head, registration_lst) {
        const NMDBusInterfaceInfoExtended *interface_info = _reg_data_get_interface_info(reg_data);
        gboolean                           has_properties = FALSE;
        GVariantBuilder                    builder;
        GVariantBuilder                    invalidated_builder;
        GVariant                          *args;

        if (!interface_info->parent.properties)
            continue;

        for (i = 0; interface_info->parent.properties[i]; i++) {
            const NMDBusPropertyInfoExtended *property_info =
                (const NMDBusPropertyInfoExtended *) interface_info->parent.properties[i];

            for (p = 0; p < n_pspecs; p++) {
                const GParamSpec          *pspec = pspecs[p];
                gs_unref_variant GVariant *value = NULL;

                if (!nm_streq(property_info->property_name, pspec->name))
                    continue;

                value = _obj_get_property(reg_data, i, TRUE);

                if (!has_properties) {
                    has_properties = TRUE;
                    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
                }
                g_variant_builder_add(&builder, "{sv}", property_info->parent.name, value);
            }
        }

        if (!has_properties)
            continue;

        args = g_variant_builder_end(&builder);

        g_variant_builder_init(&invalidated_builder, G_VARIANT_TYPE("as"));
        g_dbus_connection_emit_signal(
            priv->main_dbus_connection,
            NULL,
            obj->internal.path,
            DBUS_INTERFACE_PROPERTIES,
            "PropertiesChanged",
            g_variant_new("(s@a{sv}as)", interface_info->parent.name, args, &invalidated_builder),
            NULL);
    }
}

void
_nm_dbus_manager_obj_emit_signal(NMDBusObject                      *obj,
                                 const NMDBusInterfaceInfoExtended *interface_info,
                                 const GDBusSignalInfo             *signal_info,
                                 GVariant                          *args)
{
    NMDBusManager        *self;
    NMDBusManagerPrivate *priv;

    g_return_if_fail(NM_IS_DBUS_OBJECT(obj));
    g_return_if_fail(obj->internal.path);
    g_return_if_fail(NM_IS_DBUS_MANAGER(obj->internal.bus_manager));
    g_return_if_fail(!c_list_is_empty(&obj->internal.objects_lst));

    self = obj->internal.bus_manager;
    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    if (!priv->started) {
        nm_g_variant_unref_floating(args);
        return;
    }

    g_dbus_connection_emit_signal(priv->main_dbus_connection,
                                  NULL,
                                  obj->internal.path,
                                  interface_info->parent.name,
                                  signal_info->name,
                                  args,
                                  NULL);
}

/*****************************************************************************/

static GVariantBuilder *
_obj_collect_properties_per_interface(NMDBusObject     *obj,
                                      RegistrationData *reg_data,
                                      GVariantBuilder  *builder)
{
    const NMDBusInterfaceInfoExtended *interface_info = _reg_data_get_interface_info(reg_data);
    guint                              i;

    g_variant_builder_init(builder, G_VARIANT_TYPE("a{sv}"));
    if (interface_info->parent.properties) {
        for (i = 0; interface_info->parent.properties[i]; i++) {
            const NMDBusPropertyInfoExtended *property_info =
                (const NMDBusPropertyInfoExtended *) interface_info->parent.properties[i];
            gs_unref_variant GVariant *variant = NULL;

            variant = _obj_get_property(reg_data, i, FALSE);
            g_variant_builder_add(builder, "{sv}", property_info->parent.name, variant);
        }
    }
    return builder;
}

static GVariantBuilder *
_obj_collect_properties_all(NMDBusObject *obj, GVariantBuilder *builder)
{
    RegistrationData *reg_data;

    g_variant_builder_init(builder, G_VARIANT_TYPE("a{sa{sv}}"));

    c_list_for_each_entry (reg_data, &obj->internal.registration_lst_head, registration_lst) {
        GVariantBuilder properties_builder;

        g_variant_builder_add(
            builder,
            "{sa{sv}}",
            _reg_data_get_interface_info(reg_data)->parent.name,
            _obj_collect_properties_per_interface(obj, reg_data, &properties_builder));
    }

    return builder;
}

static void
dbus_vtable_objmgr_method_call(GDBusConnection       *connection,
                               const char            *sender,
                               const char            *object_path,
                               const char            *interface_name,
                               const char            *method_name,
                               GVariant              *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer               user_data)
{
    NMDBusManager        *self = user_data;
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    GVariantBuilder       array_builder;
    NMDBusObject         *obj;

    nm_assert(nm_streq0(object_path, OBJECT_MANAGER_SERVER_BASE_PATH));

    if (!nm_streq(method_name, "GetManagedObjects")
        || !nm_streq(interface_name, interface_info_objmgr.name)) {
        g_dbus_method_invocation_return_error(
            invocation,
            G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown method %s - only GetManagedObjects() is supported",
            method_name);
        return;
    }

    g_variant_builder_init(&array_builder, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    c_list_for_each_entry (obj, &priv->objects_lst_head, internal.objects_lst) {
        GVariantBuilder interfaces_builder;

        /* note that we are called on an idle handler. Hence, all properties are
         * supposed to be in a consistent state. That is true, if you always
         * g_object_thaw_notify() before returning to the mainloop. Keeping
         * signals frozen between while returning from the current call stack
         * is anyway a very fragile thing, easy to get wrong. Don't do that. */
        g_variant_builder_add(&array_builder,
                              "{oa{sa{sv}}}",
                              obj->internal.path,
                              _obj_collect_properties_all(obj, &interfaces_builder));
    }
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(a{oa{sa{sv}}})", &array_builder));
}

static const GDBusInterfaceVTable dbus_vtable_objmgr = {.method_call =
                                                            dbus_vtable_objmgr_method_call};

static const GDBusSignalInfo signal_info_objmgr_interfaces_added = NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
    "InterfacesAdded",
    .args = NM_DEFINE_GDBUS_ARG_INFOS(
        NM_DEFINE_GDBUS_ARG_INFO("object_path", "o"),
        NM_DEFINE_GDBUS_ARG_INFO("interfaces_and_properties", "a{sa{sv}}"), ), );

static const GDBusSignalInfo signal_info_objmgr_interfaces_removed =
    NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
        "InterfacesRemoved",
        .args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("object_path", "o"),
                                          NM_DEFINE_GDBUS_ARG_INFO("interfaces", "as"), ), );

static const GDBusInterfaceInfo interface_info_objmgr = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
    DBUS_INTERFACE_OBJECT_MANAGER,
    .methods = NM_DEFINE_GDBUS_METHOD_INFOS(
        NM_DEFINE_GDBUS_METHOD_INFO(
            "GetManagedObjects",
            .out_args = NM_DEFINE_GDBUS_ARG_INFOS(
                NM_DEFINE_GDBUS_ARG_INFO("object_paths_interfaces_and_properties",
                                         "a{oa{sa{sv}}}"), ), ), ),
    .signals = NM_DEFINE_GDBUS_SIGNAL_INFOS(&signal_info_objmgr_interfaces_added,
                                            &signal_info_objmgr_interfaces_removed, ), );

/*****************************************************************************/

GDBusConnection *
nm_dbus_manager_get_dbus_connection(NMDBusManager *self)
{
    g_return_val_if_fail(NM_IS_DBUS_MANAGER(self), NULL);

    return NM_DBUS_MANAGER_GET_PRIVATE(self)->main_dbus_connection;
}

void
nm_dbus_manager_start(NMDBusManager                  *self,
                      NMDBusManagerSetPropertyHandler set_property_handler,
                      gpointer                        set_property_handler_data)
{
    NMDBusManagerPrivate *priv;
    NMDBusObject         *obj;

    g_return_if_fail(NM_IS_DBUS_MANAGER(self));

    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    nm_assert(!priv->started);

    if (priv->objmgr_registration_id == 0) {
        /* Do nothing. We're presumably in the configure-and-quit mode. */
        return;
    }

    priv->set_property_handler      = set_property_handler;
    priv->set_property_handler_data = set_property_handler_data;
    priv->started                   = TRUE;

    c_list_for_each_entry (obj, &priv->objects_lst_head, internal.objects_lst)
        _obj_register(self, obj);
}

gboolean
nm_dbus_manager_request_name_sync(NMDBusManager *self)
{
    NMDBusManagerPrivate      *priv;
    gs_free_error GError      *error = NULL;
    gs_unref_variant GVariant *ret   = NULL;
    guint32                    result;

    g_return_val_if_fail(NM_IS_DBUS_MANAGER(self), FALSE);

    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    if (priv->objmgr_registration_id == 0) {
        /* Do nothing. We're presumably in the configure-and-quit mode. */
        return TRUE;
    }

    g_return_val_if_fail(G_IS_DBUS_CONNECTION(priv->main_dbus_connection), FALSE);

    ret = g_dbus_connection_call_sync(
        priv->main_dbus_connection,
        DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS,
        "RequestName",
        g_variant_new("(su)", NM_DBUS_SERVICE, DBUS_NAME_FLAG_DO_NOT_QUEUE),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (!ret) {
        _LOGE("fatal failure to acquire D-Bus service \"%s"
              ": %s",
              NM_DBUS_SERVICE,
              error->message);
        return FALSE;
    }

    g_variant_get(ret, "(u)", &result);
    if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        _LOGE("fatal failure to acquire D-Bus service \"%s\" (%u). Service already taken",
              NM_DBUS_SERVICE,
              (guint) result);
        return FALSE;
    }

    _LOGI("acquired D-Bus service \"%s\"", NM_DBUS_SERVICE);
    return TRUE;
}

gboolean
nm_dbus_manager_setup(NMDBusManager *self)
{
    NMDBusManagerPrivate *priv;
    gs_free_error GError *error = NULL;
    guint                 registration_id;

    g_return_val_if_fail(NM_IS_DBUS_MANAGER(self), FALSE);

    priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    g_return_val_if_fail(!priv->main_dbus_connection, FALSE);

    /* Create the D-Bus connection and registering the name synchronously.
     * That is necessary because we need to exit right away if we can't
     * acquire the name despite connecting to the bus successfully.
     * It means that something is gravely broken -- such as another NetworkManager
     * instance running. */
    priv->main_dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!priv->main_dbus_connection) {
        _LOGE("cannot connect to D-Bus: %s", error->message);
        return FALSE;
    }

    g_dbus_connection_set_exit_on_close(priv->main_dbus_connection, FALSE);

    registration_id = g_dbus_connection_register_object(
        priv->main_dbus_connection,
        OBJECT_MANAGER_SERVER_BASE_PATH,
        NM_UNCONST_PTR(GDBusInterfaceInfo, &interface_info_objmgr),
        &dbus_vtable_objmgr,
        self,
        NULL,
        &error);
    if (!registration_id) {
        _LOGE("failure to register object manager: %s", error->message);
        return FALSE;
    }

    priv->objmgr_registration_id = registration_id;

    _LOGD("D-Bus connection created and ObjectManager object registered");

    return TRUE;
}

void
nm_dbus_manager_stop(NMDBusManager *self)
{
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    priv->shutting_down = TRUE;

    /* during shutdown we also clear the set-property-handler. It's no longer
     * possible to set a property, because doing so would require authorization,
     * which is async, which is just complicated to get right. No more property
     * setting from now on. */
    priv->set_property_handler      = NULL;
    priv->set_property_handler_data = NULL;
}

gboolean
nm_dbus_manager_is_stopping(NMDBusManager *self)
{
    return NM_DBUS_MANAGER_GET_PRIVATE(self)->shutting_down;
}

/*****************************************************************************/

static void
nm_dbus_manager_init(NMDBusManager *self)
{
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);

    c_list_init(&priv->private_servers_lst_head);
    c_list_init(&priv->objects_lst_head);

    priv->objects_by_path =
        g_hash_table_new((GHashFunc) _objects_by_path_hash, (GEqualFunc) _objects_by_path_equal);

    c_list_init(&priv->caller_info_lst_head);
}

static void
dispose(GObject *object)
{
    NMDBusManager        *self = NM_DBUS_MANAGER(object);
    NMDBusManagerPrivate *priv = NM_DBUS_MANAGER_GET_PRIVATE(self);
    PrivateServer        *s, *s_safe;
    CallerInfo           *caller_info;

    /* All exported NMDBusObject instances keep the manager alive, so we don't
     * expect any remaining objects. */
    nm_assert(!priv->objects_by_path || g_hash_table_size(priv->objects_by_path) == 0);
    nm_assert(c_list_is_empty(&priv->objects_lst_head));

    nm_clear_pointer(&priv->objects_by_path, g_hash_table_destroy);

    c_list_for_each_entry_safe (s, s_safe, &priv->private_servers_lst_head, private_servers_lst)
        private_server_free(s);

    if (priv->objmgr_registration_id) {
        g_dbus_connection_unregister_object(priv->main_dbus_connection,
                                            nm_steal_int(&priv->objmgr_registration_id));
    }

    g_clear_object(&priv->main_dbus_connection);

    G_OBJECT_CLASS(nm_dbus_manager_parent_class)->dispose(object);

    while ((caller_info =
                c_list_first_entry(&priv->caller_info_lst_head, CallerInfo, caller_info_lst)))
        _caller_info_free(caller_info);
}

static void
nm_dbus_manager_class_init(NMDBusManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = dispose;

    signals[PRIVATE_CONNECTION_NEW] = g_signal_new(NM_DBUS_MANAGER_PRIVATE_CONNECTION_NEW,
                                                   G_OBJECT_CLASS_TYPE(object_class),
                                                   G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                                   0,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   G_TYPE_NONE,
                                                   2,
                                                   G_TYPE_DBUS_CONNECTION,
                                                   G_TYPE_DBUS_OBJECT_MANAGER_SERVER);

    signals[PRIVATE_CONNECTION_DISCONNECTED] =
        g_signal_new(NM_DBUS_MANAGER_PRIVATE_CONNECTION_DISCONNECTED,
                     G_OBJECT_CLASS_TYPE(object_class),
                     G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                     0,
                     NULL,
                     NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);
}

static NMAuthSubject *
_new_unix_process(GDBusMethodInvocation *context,
                  GDBusConnection       *connection,
                  GDBusMessage          *message)
{
    NMAuthSubject *self;
    const char    *dbus_sender = NULL;
    gulong         uid         = 0;
    gulong         pid         = 0;
    gboolean       success;

    g_return_val_if_fail(context || (connection && message), NULL);

    if (context) {
        success = nm_dbus_manager_get_caller_info(nm_dbus_manager_get(),
                                                  context,
                                                  &dbus_sender,
                                                  &uid,
                                                  &pid);
    } else {
        nm_assert(message);
        success = nm_dbus_manager_get_caller_info_from_message(nm_dbus_manager_get(),
                                                               connection,
                                                               message,
                                                               &dbus_sender,
                                                               &uid,
                                                               &pid);
    }

    if (!success)
        return NULL;

    g_return_val_if_fail(dbus_sender && *dbus_sender, NULL);
    /* polkit glib library stores uid and pid as int. There might be some
     * pitfalls if the id ever happens to be larger then that. Just assert against
     * it here. */
    g_return_val_if_fail(uid <= NM_MIN(G_MAXINT, G_MAXINT32), NULL);
    g_return_val_if_fail(pid > 0 && pid <= NM_MIN(G_MAXINT, G_MAXINT32), NULL);

    self = nm_auth_subject_new_unix_process(dbus_sender, pid, uid);

    if (nm_auth_subject_get_subject_type(self) != NM_AUTH_SUBJECT_TYPE_UNIX_PROCESS) {
        /* this most likely happened because the process is gone (start_time==0).
         * Either that is not assert-worthy, or constructed() already asserted.
         * Just return NULL. */
        g_clear_object(&self);
    }
    return self;
}

NMAuthSubject *
nm_dbus_manager_new_auth_subject_from_context(GDBusMethodInvocation *context)
{
    return _new_unix_process(context, NULL, NULL);
}

NMAuthSubject *
nm_dbus_manager_new_auth_subject_from_message(GDBusConnection *connection, GDBusMessage *message)
{
    return _new_unix_process(NULL, connection, message);
}
