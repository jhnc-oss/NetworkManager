/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-device-wifi-p2p.h"

#include <sys/socket.h>

#include "supplicant/nm-supplicant-manager.h"
#include "supplicant/nm-supplicant-interface.h"

#include "NetworkManagerUtils.h"
#include "devices/nm-device-private.h"
#include "libnm-core-aux-intern/nm-libnm-core-utils.h"
#include "libnm-core-aux-intern/nm-common-macros.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-glib-aux/nm-ref-string.h"
#include "libnm-platform/nm-platform.h"
#include "libnm-platform/nmp-object.h"
#include "nm-act-request.h"
#include "nm-l3-config-data.h"
#include "nm-manager.h"
#include "nm-setting-wifi-p2p.h"
#include "nm-utils.h"
#include "nm-wifi-p2p-peer.h"
#include "settings/nm-settings.h"

#define _NMLOG_DEVICE_TYPE NMDeviceWifiP2P
#include "devices/nm-device-logging.h"

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE(NMDeviceWifiP2P, PROP_PEERS, );

typedef struct {
    NMSupplicantManager *sup_mgr;

    /* NOTE: In theory management and group ifaces could be identical. However,
     * in practice, this cannot happen currently as NMDeviceWifiP2P is only
     * created for existing non-P2P interfaces.
     * (i.e. a single standalone P2P interface is not supported at this point)
     */
    NMSupplicantInterface *mgmt_iface;
    NMSupplicantInterface *group_iface;

    CList peers_lst_head;

    guint find_peer_timeout_id;
    guint sup_timeout_id;
    guint peer_dump_id;
    guint peer_missing_id;

    bool is_waiting_for_supplicant : 1;
    bool enabled : 1;
} NMDeviceWifiP2PPrivate;

struct _NMDeviceWifiP2P {
    NMDevice               parent;
    NMDeviceWifiP2PPrivate _priv;
};

struct _NMDeviceWifiP2PClass {
    NMDeviceClass parent;
};

G_DEFINE_TYPE(NMDeviceWifiP2P, nm_device_wifi_p2p, NM_TYPE_DEVICE)

#define NM_DEVICE_WIFI_P2P_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMDeviceWifiP2P, NM_IS_DEVICE_WIFI_P2P, NMDevice)

/*****************************************************************************/

static const NMDBusInterfaceInfoExtended interface_info_device_wifi_p2p;
static const GDBusSignalInfo             nm_signal_info_wifi_p2p_peer_added;
static const GDBusSignalInfo             nm_signal_info_wifi_p2p_peer_removed;

static void supplicant_group_interface_release(NMDeviceWifiP2P *self);
static void supplicant_interfaces_release(NMDeviceWifiP2P *self, gboolean set_is_waiting);

/*****************************************************************************/

static void
_peer_dump(NMDeviceWifiP2P     *self,
           NMLogLevel           log_level,
           const NMWifiP2PPeer *peer,
           const char          *prefix,
           gint32               now_s)
{
    char buf[1024];

    _NMLOG(log_level,
           LOGD_WIFI_SCAN,
           "wifi-peer: %-7s %s",
           prefix,
           nm_wifi_p2p_peer_to_string(peer, buf, sizeof(buf), now_s));
}

static gboolean
peer_list_dump(gpointer user_data)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(user_data);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    priv->peer_dump_id = 0;

    if (_LOGD_ENABLED(LOGD_WIFI_SCAN)) {
        NMWifiP2PPeer *peer;
        gint32         now_s = nm_utils_get_monotonic_timestamp_sec();

        _LOGD(LOGD_WIFI_SCAN, "P2P Peers: [now:%u]", now_s);
        c_list_for_each_entry (peer, &priv->peers_lst_head, peers_lst)
            _peer_dump(self, LOGL_DEBUG, peer, "dump", now_s);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_peer_list_dump(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (!priv->peer_dump_id && _LOGD_ENABLED(LOGD_WIFI_SCAN))
        priv->peer_dump_id = g_timeout_add_seconds(1, peer_list_dump, self);
}

/*****************************************************************************/

static void
_set_is_waiting_for_supplicant(NMDeviceWifiP2P *self, gboolean is_waiting)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (priv->is_waiting_for_supplicant == (!!is_waiting))
        return;

    priv->is_waiting_for_supplicant = is_waiting;

    if (is_waiting)
        nm_device_add_pending_action(NM_DEVICE(self),
                                     NM_PENDING_ACTION_WAITING_FOR_SUPPLICANT,
                                     TRUE);
    else
        nm_device_remove_pending_action(NM_DEVICE(self),
                                        NM_PENDING_ACTION_WAITING_FOR_SUPPLICANT,
                                        TRUE);
}

/*****************************************************************************/

static gboolean
check_connection_peer_joined(NMDeviceWifiP2P *device)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(device);
    NMConnection           *conn = nm_device_get_applied_connection(NM_DEVICE(device));
    NMWifiP2PPeer          *peer;
    const char             *group;
    const char *const      *groups;

    if (!conn || !priv->group_iface)
        return FALSE;

    /* Comparing the object path found on the group_iface with the peers
     * found on the mgmt_iface is legal. */
    group = nm_supplicant_interface_get_p2p_group_path(priv->group_iface);
    if (!group)
        return FALSE;

    /* NOTE: We currently only support connections to a specific peer */
    peer = nm_wifi_p2p_peers_find_first_compatible(&priv->peers_lst_head, conn, FALSE);
    if (!peer)
        return FALSE;

    groups = nm_wifi_p2p_peer_get_groups(peer);
    if (!groups || !g_strv_contains(groups, group))
        return FALSE;

    return TRUE;
}

static gboolean
disconnect_on_connection_peer_missing_cb(gpointer user_data)
{
    NMDevice               *device = NM_DEVICE(user_data);
    NMDeviceWifiP2P        *self   = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv   = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    _LOGW(LOGD_WIFI, "Peer requested in connection is missing for too long, failing connection.");

    priv->peer_missing_id = 0;

    nm_device_state_changed(device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_PEER_NOT_FOUND);
    return FALSE;
}

static void
update_disconnect_on_connection_peer_missing(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    NMDeviceState           state;

    state = nm_device_get_state(NM_DEVICE(self));
    if (state < NM_DEVICE_STATE_IP_CONFIG || state > NM_DEVICE_STATE_ACTIVATED) {
        nm_clear_g_source(&priv->peer_missing_id);
        return;
    }

    if (check_connection_peer_joined(self)) {
        if (nm_clear_g_source(&priv->peer_missing_id))
            _LOGD(LOGD_WIFI, "Peer requested in connection is joined, removing timeout");
        return;
    }

    if (priv->peer_missing_id == 0) {
        _LOGD(LOGD_WIFI, "Peer requested in connection is missing, adding timeout");
        priv->peer_missing_id =
            g_timeout_add_seconds(5, disconnect_on_connection_peer_missing_cb, self);
    }
}

static gboolean
is_available(NMDevice *device, NMDeviceCheckDevAvailableFlags flags)
{
    NMDeviceWifiP2P           *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate    *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    NMSupplicantInterfaceState supplicant_state;

    if (!priv->mgmt_iface)
        return FALSE;

    supplicant_state = nm_supplicant_interface_get_state(priv->mgmt_iface);
    return nm_supplicant_interface_state_is_operational(supplicant_state);
}

static gboolean
check_connection_compatible(NMDevice     *device,
                            NMConnection *connection,
                            gboolean      check_properties,
                            GError      **error)
{
    if (!NM_DEVICE_CLASS(nm_device_wifi_p2p_parent_class)
             ->check_connection_compatible(device, connection, check_properties, error))
        return FALSE;

    /* TODO: Allow limitting the interface using the HW-address? */

    /* We don't need to check anything else here. The P2P device will only
     * exists if we are able to establish a P2P connection, and there should
     * be no further restrictions necessary.
     */

    return TRUE;
}

static gboolean
complete_connection(NMDevice            *device,
                    NMConnection        *connection,
                    const char          *specific_object,
                    NMConnection *const *existing_connections,
                    GError             **error)
{
    NMDeviceWifiP2P  *self         = NM_DEVICE_WIFI_P2P(device);
    gs_free char     *setting_name = NULL;
    NMSettingWifiP2P *s_wifi_p2p;
    NMWifiP2PPeer    *peer;
    const char       *setting_peer;

    s_wifi_p2p =
        NM_SETTING_WIFI_P2P(nm_connection_get_setting(connection, NM_TYPE_SETTING_WIFI_P2P));

    if (!specific_object) {
        /* If not given a specific object, we need at minimum a peer address */
        if (!s_wifi_p2p) {
            g_set_error(error,
                        NM_DEVICE_ERROR,
                        NM_DEVICE_ERROR_INVALID_CONNECTION,
                        "A '%s' setting is required if no Peer path was given",
                        NM_SETTING_WIFI_P2P_SETTING_NAME);
            return FALSE;
        }

        setting_peer = nm_setting_wifi_p2p_get_peer(s_wifi_p2p);
        if (!setting_peer) {
            g_set_error(error,
                        NM_DEVICE_ERROR,
                        NM_DEVICE_ERROR_INVALID_CONNECTION,
                        "A '%s' setting with a valid Peer is required if no Peer path was given",
                        NM_SETTING_WIFI_P2P_SETTING_NAME);
            return FALSE;
        }

    } else {
        peer = nm_wifi_p2p_peer_lookup_for_device(NM_DEVICE(self), specific_object);
        if (!peer) {
            g_set_error(error,
                        NM_DEVICE_ERROR,
                        NM_DEVICE_ERROR_SPECIFIC_OBJECT_NOT_FOUND,
                        "The P2P peer %s is unknown",
                        specific_object);
            return FALSE;
        }

        setting_peer = nm_wifi_p2p_peer_get_address(peer);
        g_return_val_if_fail(setting_peer, FALSE);
    }

    /* Add a Wi-Fi P2P setting if one doesn't exist yet */
    s_wifi_p2p = _nm_connection_ensure_setting(connection, NM_TYPE_SETTING_WIFI_P2P);

    g_object_set(G_OBJECT(s_wifi_p2p), NM_SETTING_WIFI_P2P_PEER, setting_peer, NULL);

    setting_name = g_strdup_printf("Wi-Fi P2P Peer %s", setting_peer);
    nm_utils_complete_generic(nm_device_get_platform(device),
                              connection,
                              NM_SETTING_WIFI_P2P_SETTING_NAME,
                              existing_connections,
                              setting_name,
                              setting_name,
                              NULL,
                              NULL);

    return TRUE;
}

/*
 * supplicant_find_timeout_cb
 *
 * Called when the supplicant has been unable to find the peer we want to connect to.
 */
static gboolean
supplicant_find_timeout_cb(gpointer user_data)
{
    NMDevice               *device = NM_DEVICE(user_data);
    NMDeviceWifiP2P        *self   = NM_DEVICE_WIFI_P2P(user_data);
    NMDeviceWifiP2PPrivate *priv   = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    priv->find_peer_timeout_id = 0;

    nm_supplicant_interface_p2p_cancel_connect(priv->mgmt_iface);

    if (nm_device_is_activating(device)) {
        _LOGW(LOGD_DEVICE | LOGD_WIFI,
              "Activation: (wifi-p2p) could not find peer, failing activation");
        nm_device_state_changed(device,
                                NM_DEVICE_STATE_FAILED,
                                NM_DEVICE_STATE_REASON_PEER_NOT_FOUND);
    }

    return G_SOURCE_REMOVE;
}

static NMActStageReturn
act_stage1_prepare(NMDevice *device, NMDeviceStateReason *out_failure_reason)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    NMConnection           *connection;
    NMSettingWifiP2P       *s_wifi_p2p;
    NMWifiP2PPeer          *peer;

    if (!priv->mgmt_iface) {
        NM_SET_OUT(out_failure_reason, NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        return NM_ACT_STAGE_RETURN_FAILURE;
    }

    connection = nm_device_get_applied_connection(NM_DEVICE(self));
    g_return_val_if_fail(connection, NM_ACT_STAGE_RETURN_FAILURE);

    s_wifi_p2p =
        NM_SETTING_WIFI_P2P(nm_connection_get_setting(connection, NM_TYPE_SETTING_WIFI_P2P));
    g_return_val_if_fail(s_wifi_p2p, NM_ACT_STAGE_RETURN_FAILURE);

    peer = nm_wifi_p2p_peers_find_first_compatible(&priv->peers_lst_head, connection, FALSE);
    if (!peer) {
        /* Set up a timeout on the find attempt and run a find for the same period of time */
        if (priv->find_peer_timeout_id == 0) {
            priv->find_peer_timeout_id =
                g_timeout_add_seconds(10, supplicant_find_timeout_cb, self);

            nm_supplicant_interface_p2p_start_find(priv->mgmt_iface, 10);
        }
        return NM_ACT_STAGE_RETURN_POSTPONE;
    }

    return NM_ACT_STAGE_RETURN_SUCCESS;
}

/*
 * supplicant_connection_timeout_cb
 *
 * Called when the supplicant has been unable to connect to a peer
 * within a specified period of time.
 */
static gboolean
supplicant_connection_timeout_cb(gpointer user_data)
{
    NMDevice               *device = NM_DEVICE(user_data);
    NMDeviceWifiP2P        *self   = NM_DEVICE_WIFI_P2P(user_data);
    NMDeviceWifiP2PPrivate *priv   = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    priv->sup_timeout_id = 0;

    nm_supplicant_interface_p2p_cancel_connect(priv->mgmt_iface);

    if (nm_device_is_activating(device)) {
        _LOGW(LOGD_DEVICE | LOGD_WIFI,
              "Activation: (wifi-p2p) connecting took too long, failing activation");
        nm_device_state_changed(device,
                                NM_DEVICE_STATE_FAILED,
                                NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT);
    }

    return G_SOURCE_REMOVE;
}

static NMActStageReturn
act_stage2_config(NMDevice *device, NMDeviceStateReason *out_failure_reason)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    NMConnection           *connection;
    NMSettingWifiP2P       *s_wifi_p2p;
    NMWifiP2PPeer          *peer;
    GBytes                 *wfd_ies;

    if (nm_clear_g_source(&priv->find_peer_timeout_id))
        nm_assert_not_reached();

    if (!priv->mgmt_iface) {
        NM_SET_OUT(out_failure_reason, NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        return NM_ACT_STAGE_RETURN_FAILURE;
    }

    connection = nm_device_get_applied_connection(device);
    g_return_val_if_fail(connection, NM_ACT_STAGE_RETURN_FAILURE);
    nm_assert(
        NM_IS_SETTING_WIFI_P2P(nm_connection_get_setting(connection, NM_TYPE_SETTING_WIFI_P2P)));

    /* The prepare stage ensures that the peer has been found */
    peer = nm_wifi_p2p_peers_find_first_compatible(&priv->peers_lst_head, connection, FALSE);
    if (!peer) {
        NM_SET_OUT(out_failure_reason, NM_DEVICE_STATE_REASON_PEER_NOT_FOUND);
        return NM_ACT_STAGE_RETURN_FAILURE;
    }

    /* Set the WFD IEs before trying to establish the connection. */
    s_wifi_p2p =
        NM_SETTING_WIFI_P2P(nm_connection_get_setting(connection, NM_TYPE_SETTING_WIFI_P2P));
    wfd_ies = nm_setting_wifi_p2p_get_wfd_ies(s_wifi_p2p);
    nm_supplicant_manager_set_wfd_ies(priv->sup_mgr, wfd_ies);

    /* TODO: Grab secrets if we don't have them yet! */

    /* TODO: Fix "pbc" being hardcoded here! */
    nm_supplicant_interface_p2p_connect(priv->mgmt_iface,
                                        nm_wifi_p2p_peer_get_supplicant_path(peer),
                                        "pbc",
                                        NULL);

    /* Set up a timeout on the connect attempt */
    if (priv->sup_timeout_id == 0) {
        priv->sup_timeout_id = g_timeout_add_seconds(45, supplicant_connection_timeout_cb, self);
    }

    /* We'll get stage3 started when the P2P group has been started */
    return NM_ACT_STAGE_RETURN_POSTPONE;
}

/*****************************************************************************/

static void
emit_signal_p2p_peer_add_remove(NMDeviceWifiP2P *device,
                                NMWifiP2PPeer   *peer,
                                gboolean         is_added /* or else is_removed */)
{
    nm_dbus_object_emit_signal(NM_DBUS_OBJECT(device),
                               &interface_info_device_wifi_p2p,
                               is_added ? &nm_signal_info_wifi_p2p_peer_added
                                        : &nm_signal_info_wifi_p2p_peer_removed,
                               "(o)",
                               nm_dbus_object_get_path(NM_DBUS_OBJECT(peer)));
}

static void
peer_add_remove(NMDeviceWifiP2P *self,
                gboolean         is_adding, /* or else removing */
                NMWifiP2PPeer   *peer,
                gboolean         recheck_available_connections)
{
    NMDevice               *device = NM_DEVICE(self);
    NMDeviceWifiP2PPrivate *priv   = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (is_adding) {
        g_object_ref(peer);
        peer->wifi_device = device;
        c_list_link_tail(&priv->peers_lst_head, &peer->peers_lst);
        nm_dbus_object_export(NM_DBUS_OBJECT(peer));
        _peer_dump(self, LOGL_DEBUG, peer, "added", 0);

        emit_signal_p2p_peer_add_remove(self, peer, TRUE);
    } else {
        peer->wifi_device = NULL;
        c_list_unlink(&peer->peers_lst);
        _peer_dump(self, LOGL_DEBUG, peer, "removed", 0);
    }

    _notify(self, PROP_PEERS);

    if (!is_adding) {
        emit_signal_p2p_peer_add_remove(self, peer, FALSE);
        nm_dbus_object_clear_and_unexport(&peer);
    }

    if (is_adding) {
        /* If we are in prepare state, then we are currently runnign a find
         * to search for the requested peer. */
        if (priv->find_peer_timeout_id != 0) {
            NMConnection *connection;

            nm_assert(nm_device_get_state(device) == NM_DEVICE_STATE_PREPARE);

            connection = nm_device_get_applied_connection(device);
            nm_assert(NM_IS_CONNECTION(connection));

            peer =
                nm_wifi_p2p_peers_find_first_compatible(&priv->peers_lst_head, connection, FALSE);
            if (peer) {
                /* A peer for the connection was found, cancel the timeout and go to configure state. */
                nm_clear_g_source(&priv->find_peer_timeout_id);
                nm_device_activate_schedule_stage1_device_prepare(device, FALSE);
            }
        }

        /* TODO: We may want to re-check auto-activation here, otherwise it will never work. */
    }

    update_disconnect_on_connection_peer_missing(self);
}

static void
remove_all_peers(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    NMWifiP2PPeer          *peer;

    if (c_list_is_empty(&priv->peers_lst_head))
        return;

    while ((peer = c_list_first_entry(&priv->peers_lst_head, NMWifiP2PPeer, peers_lst)))
        peer_add_remove(self, FALSE, peer, FALSE);

    nm_device_recheck_available_connections(NM_DEVICE(self));
}

/*****************************************************************************/

static void
act_stage3_ip_config(NMDevice *device, int addr_family)
{
    NMDeviceWifiP2PPrivate *priv    = NM_DEVICE_WIFI_P2P_GET_PRIVATE(device);
    const int               IS_IPv4 = NM_IS_IPv4(addr_family);
    gboolean                indicate_addressing_running;
    NMConnection           *connection;
    const char             *method;

    connection = nm_device_get_applied_connection(device);

    method = nm_utils_get_ip_config_method(connection, addr_family);

    /* We may have an address assigned by the group owner */
    if (IS_IPv4 && NM_IN_STRSET(method, NM_SETTING_IP4_CONFIG_METHOD_AUTO) && priv->group_iface
        && !nm_supplicant_interface_get_p2p_group_owner(priv->group_iface)) {
        in_addr_t addr;
        guint8    plen;

        if (nm_supplicant_interface_get_p2p_assigned_addr(priv->group_iface, &addr, &plen)) {
            nm_auto_unref_l3cd_init NML3ConfigData *l3cd    = NULL;
            NMPlatformIP4Address                    address = {
                                   .addr_source = NM_IP_CONFIG_SOURCE_DHCP,
            };

            nm_platform_ip4_address_set_addr(&address, addr, plen);

            l3cd = nm_device_create_l3_config_data(device, NM_IP_CONFIG_SOURCE_DHCP);
            nm_l3_config_data_add_address_4(l3cd, &address);

            nm_device_devip_set_state(device, AF_INET, NM_DEVICE_IP_STATE_READY, l3cd);

            /* This just disables the addressing indicator. */
            method = NM_SETTING_IP4_CONFIG_METHOD_MANUAL;
        }
    }

    if (IS_IPv4)
        indicate_addressing_running = NM_IN_STRSET(method, NM_SETTING_IP4_CONFIG_METHOD_AUTO);
    else {
        indicate_addressing_running = NM_IN_STRSET(method,
                                                   NM_SETTING_IP6_CONFIG_METHOD_AUTO,
                                                   NM_SETTING_IP6_CONFIG_METHOD_DHCP);
    }

    if (indicate_addressing_running)
        nm_platform_wifi_indicate_addressing_running(nm_device_get_platform(device),
                                                     nm_device_get_ip_ifindex(device),
                                                     TRUE);
}

static void
deactivate(NMDevice *device)
{
    NMDeviceWifiP2P        *self    = NM_DEVICE_WIFI_P2P(device);
    int                     ifindex = nm_device_get_ip_ifindex(device);
    NMDeviceWifiP2PPrivate *priv    = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    nm_clear_g_source(&priv->find_peer_timeout_id);
    nm_clear_g_source(&priv->sup_timeout_id);
    nm_clear_g_source(&priv->peer_missing_id);

    if (priv->mgmt_iface)
        nm_supplicant_interface_p2p_cancel_connect(priv->mgmt_iface);

    if (priv->group_iface)
        nm_supplicant_interface_p2p_disconnect(priv->group_iface);

    /* Clear any critical protocol notification in the Wi-Fi stack */
    if (ifindex > 0)
        nm_platform_wifi_indicate_addressing_running(nm_device_get_platform(device),
                                                     ifindex,
                                                     FALSE);
}

static guint32
get_configured_mtu(NMDevice *device, NMDeviceMtuSource *out_source, gboolean *out_force)
{
    *out_source = NM_DEVICE_MTU_SOURCE_NONE;
    return 0;
}

static const char *
get_auto_ip_config_method(NMDevice *device, int addr_family)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (addr_family == AF_INET && priv->group_iface
        && !nm_supplicant_interface_get_p2p_group_owner(priv->group_iface)
        && nm_supplicant_interface_get_p2p_assigned_addr(priv->group_iface, NULL, NULL))
        return NM_SETTING_IP4_CONFIG_METHOD_MANUAL;

    /* Override the AUTO method to mean shared if we are group owner. */
    if (priv->group_iface && nm_supplicant_interface_get_p2p_group_owner(priv->group_iface)) {
        if (addr_family == AF_INET)
            return NM_SETTING_IP4_CONFIG_METHOD_SHARED;

        if (addr_family == AF_INET6)
            return NM_SETTING_IP6_CONFIG_METHOD_SHARED;
    }

    return NULL;
}

static gboolean
unmanaged_on_quit(NMDevice *self)
{
    return TRUE;
}

static void
supplicant_iface_state_cb(NMSupplicantInterface *iface,
                          int                    new_state_i,
                          int                    old_state_i,
                          int                    disconnect_reason,
                          gpointer               user_data)
{
    NMDeviceWifiP2P           *self      = NM_DEVICE_WIFI_P2P(user_data);
    NMDevice                  *device    = NM_DEVICE(self);
    NMSupplicantInterfaceState new_state = new_state_i;
    NMSupplicantInterfaceState old_state = old_state_i;

    _LOGI(LOGD_DEVICE | LOGD_WIFI,
          "supplicant management interface state: %s -> %s",
          nm_supplicant_interface_state_to_string(old_state),
          nm_supplicant_interface_state_to_string(new_state));

    if (new_state == NM_SUPPLICANT_INTERFACE_STATE_DOWN) {
        supplicant_interfaces_release(self, TRUE);
        nm_device_queue_recheck_available(device,
                                          NM_DEVICE_STATE_REASON_SUPPLICANT_AVAILABLE,
                                          NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        return;
    }

    if (old_state == NM_SUPPLICANT_INTERFACE_STATE_STARTING) {
        _LOGD(LOGD_WIFI, "supplicant ready");
        nm_device_queue_recheck_available(device,
                                          NM_DEVICE_STATE_REASON_SUPPLICANT_AVAILABLE,
                                          NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        _set_is_waiting_for_supplicant(self, FALSE);
    }
}

static void
supplicant_iface_peer_changed_cb(NMSupplicantInterface *iface,
                                 NMSupplicantPeerInfo  *peer_info,
                                 gboolean               is_present,
                                 NMDeviceWifiP2P       *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    NMWifiP2PPeer          *found_peer;

    found_peer =
        nm_wifi_p2p_peers_find_by_supplicant_path(&priv->peers_lst_head, peer_info->peer_path->str);

    if (!is_present) {
        if (!found_peer)
            return;

        peer_add_remove(self, FALSE, found_peer, TRUE);
        goto out;
    }

    if (found_peer) {
        if (!nm_wifi_p2p_peer_update_from_properties(found_peer, peer_info))
            return;

        update_disconnect_on_connection_peer_missing(self);
        _peer_dump(self, LOGL_DEBUG, found_peer, "updated", 0);
    } else {
        gs_unref_object NMWifiP2PPeer *peer = NULL;

        peer = nm_wifi_p2p_peer_new_from_properties(peer_info);
        peer_add_remove(self, TRUE, peer, TRUE);
    }

out:
    schedule_peer_list_dump(self);
}

static void
check_group_iface_ready(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (!priv->group_iface)
        return;

    if (!nm_supplicant_interface_state_is_operational(
            nm_supplicant_interface_get_state(priv->group_iface)))
        return;

    if (!nm_supplicant_interface_get_p2p_group_joined(priv->group_iface))
        return;

    nm_clear_g_source(&priv->sup_timeout_id);
    update_disconnect_on_connection_peer_missing(self);

    nm_device_activate_schedule_stage3_ip_config(NM_DEVICE(self), FALSE);
}

static void
supplicant_group_iface_is_ready(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    _LOGD(LOGD_WIFI, "P2P Group supplicant ready");

    if (!nm_device_set_ip_iface(NM_DEVICE(self),
                                nm_supplicant_interface_get_ifname(priv->group_iface))) {
        nm_device_state_changed(NM_DEVICE(self),
                                NM_DEVICE_STATE_FAILED,
                                NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        return;
    }

    check_group_iface_ready(self);
}

static void
supplicant_group_iface_state_cb(NMSupplicantInterface *iface,
                                int                    new_state_i,
                                int                    old_state_i,
                                int                    disconnect_reason,
                                gpointer               user_data)
{
    NMDeviceWifiP2P           *self      = NM_DEVICE_WIFI_P2P(user_data);
    NMSupplicantInterfaceState new_state = new_state_i;
    NMSupplicantInterfaceState old_state = old_state_i;

    _LOGI(LOGD_DEVICE | LOGD_WIFI,
          "P2P Group supplicant interface state: %s -> %s",
          nm_supplicant_interface_state_to_string(old_state),
          nm_supplicant_interface_state_to_string(new_state));

    if (new_state == NM_SUPPLICANT_INTERFACE_STATE_DOWN) {
        supplicant_group_interface_release(self);

        nm_device_state_changed(NM_DEVICE(self),
                                NM_DEVICE_STATE_DISCONNECTED,
                                NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT);
        return;
    }

    if (old_state == NM_SUPPLICANT_INTERFACE_STATE_STARTING) {
        supplicant_group_iface_is_ready(self);
        return;
    }
}

static void
supplicant_group_iface_group_finished_cb(NMSupplicantInterface *iface,
                                         const char            *iface_path,
                                         void                  *user_data)
{
    NMDeviceWifiP2P *self = NM_DEVICE_WIFI_P2P(user_data);

    supplicant_group_interface_release(self);

    nm_device_state_changed(NM_DEVICE(self),
                            NM_DEVICE_STATE_DISCONNECTED,
                            NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT);
}

static void
supplicant_iface_group_joined_updated_cb(NMSupplicantInterface *iface,
                                         GParamSpec            *pspec,
                                         void                  *user_data)
{
    NMDeviceWifiP2P *self = NM_DEVICE_WIFI_P2P(user_data);

    check_group_iface_ready(self);
}

static void
supplicant_iface_group_started_cb(NMSupplicantInterface *iface,
                                  NMSupplicantInterface *group_iface,
                                  NMDeviceWifiP2P       *self)
{
    NMDeviceWifiP2PPrivate    *priv;
    NMSupplicantInterfaceState state;

    g_return_if_fail(self);

    if (!nm_device_is_activating(NM_DEVICE(self))) {
        _LOGW(LOGD_DEVICE | LOGD_WIFI,
              "P2P: WPA supplicant notified a group start but we are not trying to connect! "
              "Ignoring the event.");
        return;
    }

    priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    supplicant_group_interface_release(self);

    priv->group_iface = g_object_ref(group_iface);

    /* We need to wait for the interface to be ready and the group
     * information to be resolved. */
    g_signal_connect(priv->group_iface,
                     "notify::" NM_SUPPLICANT_INTERFACE_P2P_GROUP_JOINED,
                     G_CALLBACK(supplicant_iface_group_joined_updated_cb),
                     self);

    g_signal_connect(priv->group_iface,
                     NM_SUPPLICANT_INTERFACE_STATE,
                     G_CALLBACK(supplicant_group_iface_state_cb),
                     self);

    g_signal_connect(priv->group_iface,
                     NM_SUPPLICANT_INTERFACE_GROUP_FINISHED,
                     G_CALLBACK(supplicant_group_iface_group_finished_cb),
                     self);

    state = nm_supplicant_interface_get_state(priv->group_iface);
    if (state == NM_SUPPLICANT_INTERFACE_STATE_STARTING) {
        return;
    }

    supplicant_group_iface_is_ready(self);
}

static void
supplicant_group_interface_release(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (!priv->group_iface)
        return;

    g_signal_handlers_disconnect_by_data(priv->group_iface, self);

    nm_supplicant_interface_p2p_disconnect(priv->group_iface);

    g_clear_object(&priv->group_iface);
}

static void
supplicant_interfaces_release(NMDeviceWifiP2P *self, gboolean set_is_waiting)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    nm_clear_g_source(&priv->peer_dump_id);

    remove_all_peers(self);

    if (priv->mgmt_iface) {
        _LOGD(LOGD_DEVICE | LOGD_WIFI, "P2P: Releasing WPA supplicant interface.");
        nm_supplicant_manager_set_wfd_ies(priv->sup_mgr, NULL);
        g_signal_handlers_disconnect_by_data(priv->mgmt_iface, self);
        g_clear_object(&priv->mgmt_iface);
        nm_clear_g_source(&priv->find_peer_timeout_id);
        nm_clear_g_source(&priv->sup_timeout_id);
    }

    supplicant_group_interface_release(self);

    if (set_is_waiting && priv->enabled)
        _set_is_waiting_for_supplicant(self, TRUE);
}

static void
device_state_changed(NMDevice           *device,
                     NMDeviceState       new_state,
                     NMDeviceState       old_state,
                     NMDeviceStateReason reason)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    update_disconnect_on_connection_peer_missing(self);

    if (new_state <= NM_DEVICE_STATE_UNAVAILABLE) {
        /* Clean up the supplicant interface because in these states the
         * device cannot be used.
         * Do not clean up for the UNMANAGED to UNAVAILABLE transition which
         * will happen during initialization.
         */
        if (priv->mgmt_iface && old_state > new_state)
            supplicant_interfaces_release(self, TRUE);

        /* TODO: More cleanup needed? */
    }

    switch (new_state) {
    case NM_DEVICE_STATE_UNMANAGED:
        break;
    case NM_DEVICE_STATE_UNAVAILABLE:
        if (priv->enabled
            && (!priv->mgmt_iface
                || !nm_supplicant_interface_state_is_operational(
                    nm_supplicant_interface_get_state(priv->mgmt_iface))))
            _set_is_waiting_for_supplicant(self, TRUE);
        break;
    case NM_DEVICE_STATE_NEED_AUTH:
        /* Disconnect? */
        break;
    case NM_DEVICE_STATE_IP_CHECK:
        /* Clear any critical protocol notification in the wifi stack */
        nm_platform_wifi_indicate_addressing_running(nm_device_get_platform(device),
                                                     nm_device_get_ip_ifindex(device),
                                                     FALSE);
        break;
    case NM_DEVICE_STATE_ACTIVATED:
        //activation_success_handler (device);
        break;
    case NM_DEVICE_STATE_FAILED:
        /* Clear any critical protocol notification in the wifi stack.
         * At this point the IP device may have been removed already. */
        nm_supplicant_manager_set_wfd_ies(priv->sup_mgr, NULL);
        if (nm_device_get_ip_ifindex(device) > 0)
            nm_platform_wifi_indicate_addressing_running(nm_device_get_platform(device),
                                                         nm_device_get_ip_ifindex(device),
                                                         FALSE);
        break;
    case NM_DEVICE_STATE_DISCONNECTED:
        nm_supplicant_manager_set_wfd_ies(priv->sup_mgr, NULL);
        break;
    default:
        break;
    }
}

static void
p2p_start_find_auth_cb(NMDevice              *device,
                       GDBusMethodInvocation *invocation,
                       NMAuthSubject         *subject,
                       GError                *error,
                       gpointer               user_data)
{
    NMDeviceWifiP2P           *self    = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate    *priv    = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    gs_unref_variant GVariant *options = user_data;
    const char                *opts_key;
    GVariant                  *opts_val;
    GVariantIter               iter;
    gint32                     timeout = 30;

    if (error) {
        g_dbus_method_invocation_return_gerror(invocation, error);
        return;
    }

    g_variant_iter_init(&iter, options);
    while (g_variant_iter_next(&iter, "{&sv}", &opts_key, &opts_val)) {
        _nm_unused gs_unref_variant GVariant *opts_val_free = opts_val;

        if (nm_streq(opts_key, "timeout")) {
            if (!g_variant_is_of_type(opts_val, G_VARIANT_TYPE_INT32)) {
                g_dbus_method_invocation_return_error_literal(
                    invocation,
                    NM_DEVICE_ERROR,
                    NM_DEVICE_ERROR_INVALID_ARGUMENT,
                    "\"timeout\" must be an integer \"i\"");
                return;
            }

            timeout = g_variant_get_int32(opts_val);
            if (timeout <= 0 || timeout > 600) {
                g_dbus_method_invocation_return_error_literal(
                    invocation,
                    NM_DEVICE_ERROR,
                    NM_DEVICE_ERROR_NOT_ALLOWED,
                    "The timeout for a find operation needs to be in the range of 1-600s.");
                return;
            }

            continue;
        }

        g_dbus_method_invocation_return_error(invocation,
                                              NM_DEVICE_ERROR,
                                              NM_DEVICE_ERROR_INVALID_ARGUMENT,
                                              "Unsupported options key \"%s\"",
                                              opts_key);
        return;
    }

    if (!priv->mgmt_iface) {
        g_dbus_method_invocation_return_error_literal(
            invocation,
            NM_DEVICE_ERROR,
            NM_DEVICE_ERROR_NOT_ACTIVE,
            "WPA Supplicant management interface is currently unavailable.");
        return;
    }

    nm_supplicant_interface_p2p_start_find(priv->mgmt_iface, timeout);

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
impl_device_wifi_p2p_start_find(NMDBusObject                      *obj,
                                const NMDBusInterfaceInfoExtended *interface_info,
                                const NMDBusMethodInfoExtended    *method_info,
                                GDBusConnection                   *connection,
                                const char                        *sender,
                                GDBusMethodInvocation             *invocation,
                                GVariant                          *parameters)
{
    gs_unref_variant GVariant *options = NULL;

    g_variant_get(parameters, "(@a{sv})", &options);

    nm_device_auth_request(NM_DEVICE(obj),
                           invocation,
                           NULL,
                           NM_AUTH_PERMISSION_WIFI_SCAN,
                           TRUE,
                           NULL,
                           p2p_start_find_auth_cb,
                           g_steal_pointer(&options));
}

static void
p2p_stop_find_auth_cb(NMDevice              *device,
                      GDBusMethodInvocation *invocation,
                      NMAuthSubject         *subject,
                      GError                *error,
                      gpointer               user_data)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (error) {
        g_dbus_method_invocation_return_gerror(invocation, error);
        return;
    }

    if (!priv->mgmt_iface) {
        g_dbus_method_invocation_return_error_literal(
            invocation,
            NM_DEVICE_ERROR,
            NM_DEVICE_ERROR_NOT_ACTIVE,
            "WPA Supplicant management interface is currently unavailable.");
        return;
    }

    nm_supplicant_interface_p2p_stop_find(priv->mgmt_iface);

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
impl_device_wifi_p2p_stop_find(NMDBusObject                      *obj,
                               const NMDBusInterfaceInfoExtended *interface_info,
                               const NMDBusMethodInfoExtended    *method_info,
                               GDBusConnection                   *connection,
                               const char                        *sender,
                               GDBusMethodInvocation             *invocation,
                               GVariant                          *parameters)
{
    nm_device_auth_request(NM_DEVICE(obj),
                           invocation,
                           NULL,
                           NM_AUTH_PERMISSION_WIFI_SCAN,
                           TRUE,
                           NULL,
                           p2p_stop_find_auth_cb,
                           NULL);
}

static gboolean
get_enabled(NMDevice *device)
{
    return NM_DEVICE_WIFI_P2P_GET_PRIVATE(device)->enabled;
}

static void
set_enabled(NMDevice *device, gboolean enabled)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(device);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    enabled = !!enabled;

    if (priv->enabled == enabled)
        return;

    priv->enabled = enabled;

    _LOGD(LOGD_DEVICE | LOGD_WIFI, "device now %s", enabled ? "enabled" : "disabled");

    if (!enabled) {
        _set_is_waiting_for_supplicant(self, FALSE);
    }
}

/*****************************************************************************/

NMSupplicantInterface *
nm_device_wifi_p2p_get_mgmt_iface(NMDeviceWifiP2P *self)
{
    g_return_val_if_fail(NM_IS_DEVICE_WIFI_P2P(self), NULL);

    return NM_DEVICE_WIFI_P2P_GET_PRIVATE(self)->mgmt_iface;
}

void
nm_device_wifi_p2p_set_mgmt_iface(NMDeviceWifiP2P *self, NMSupplicantInterface *iface)
{
    NMDeviceWifiP2PPrivate *priv;

    g_return_if_fail(NM_IS_DEVICE_WIFI_P2P(self));
    g_return_if_fail(!iface || NM_IS_SUPPLICANT_INTERFACE(iface));

    priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    if (priv->mgmt_iface == iface)
        goto done;

    supplicant_interfaces_release(self, FALSE);

    if (!iface)
        goto done;

    _LOGD(LOGD_DEVICE | LOGD_WIFI,
          "P2P: WPA supplicant management interface changed to %s.",
          nm_ref_string_get_str(nm_supplicant_interface_get_object_path(iface)));

    priv->mgmt_iface = g_object_ref(iface);

    g_signal_connect(priv->mgmt_iface,
                     NM_SUPPLICANT_INTERFACE_STATE,
                     G_CALLBACK(supplicant_iface_state_cb),
                     self);
    g_signal_connect(priv->mgmt_iface,
                     NM_SUPPLICANT_INTERFACE_PEER_CHANGED,
                     G_CALLBACK(supplicant_iface_peer_changed_cb),
                     self);
    g_signal_connect(priv->mgmt_iface,
                     NM_SUPPLICANT_INTERFACE_GROUP_STARTED,
                     G_CALLBACK(supplicant_iface_group_started_cb),
                     self);
done:
    nm_device_queue_recheck_available(NM_DEVICE(self),
                                      NM_DEVICE_STATE_REASON_SUPPLICANT_AVAILABLE,
                                      NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
    _set_is_waiting_for_supplicant(
        self,
        priv->enabled
            && (!priv->mgmt_iface
                || !nm_supplicant_interface_state_is_operational(
                    nm_supplicant_interface_get_state(priv->mgmt_iface))));
}

void
nm_device_wifi_p2p_remove(NMDeviceWifiP2P *self)
{
    g_signal_emit_by_name(self, NM_DEVICE_REMOVED);
}

/*****************************************************************************/

static const char *
get_type_description(NMDevice *device)
{
    return "wifi-p2p";
}

/*****************************************************************************/

static const GDBusSignalInfo nm_signal_info_wifi_p2p_peer_added = NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
    "PeerAdded",
    .args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("peer", "o"), ), );

static const GDBusSignalInfo nm_signal_info_wifi_p2p_peer_removed =
    NM_DEFINE_GDBUS_SIGNAL_INFO_INIT(
        "PeerRemoved",
        .args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("peer", "o"), ), );

static const NMDBusInterfaceInfoExtended interface_info_device_wifi_p2p = {
    .parent = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
        NM_DBUS_INTERFACE_DEVICE_WIFI_P2P,
        .methods = NM_DEFINE_GDBUS_METHOD_INFOS(
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(
                NM_DEFINE_GDBUS_METHOD_INFO_INIT(
                    "StartFind",
                    .in_args = NM_DEFINE_GDBUS_ARG_INFOS(
                        NM_DEFINE_GDBUS_ARG_INFO("options", "a{sv}"), ), ),
                .handle = impl_device_wifi_p2p_start_find, ),
            NM_DEFINE_DBUS_METHOD_INFO_EXTENDED(NM_DEFINE_GDBUS_METHOD_INFO_INIT("StopFind", ),
                                                .handle = impl_device_wifi_p2p_stop_find, ), ),
        .signals    = NM_DEFINE_GDBUS_SIGNAL_INFOS(&nm_signal_info_wifi_p2p_peer_added,
                                                &nm_signal_info_wifi_p2p_peer_removed, ),
        .properties = NM_DEFINE_GDBUS_PROPERTY_INFOS(
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE(
                "HwAddress",
                "s",
                NM_DEVICE_HW_ADDRESS,
                .annotations = NM_GDBUS_ANNOTATION_INFO_LIST_DEPRECATED(), ),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Peers",
                                                           "ao",
                                                           NM_DEVICE_WIFI_P2P_PEERS), ), ),
};

/*****************************************************************************/

static void
get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(object);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);
    const char            **list;

    switch (prop_id) {
    case PROP_PEERS:
        list = nm_wifi_p2p_peers_get_paths(&priv->peers_lst_head);
        g_value_take_boxed(value, nm_strv_make_deep_copied(list));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

/*****************************************************************************/

static void
nm_device_wifi_p2p_init(NMDeviceWifiP2P *self)
{
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(self);

    c_list_init(&priv->peers_lst_head);

    priv->sup_mgr = g_object_ref(nm_supplicant_manager_get());
}

static void
constructed(GObject *object)
{
    NMDeviceWifiP2P *self = NM_DEVICE_WIFI_P2P(object);

    G_OBJECT_CLASS(nm_device_wifi_p2p_parent_class)->constructed(object);

    _set_is_waiting_for_supplicant(self, TRUE);
}

NMDeviceWifiP2P *
nm_device_wifi_p2p_new(const char *iface)
{
    return g_object_new(NM_TYPE_DEVICE_WIFI_P2P,
                        NM_DEVICE_IFACE,
                        iface,
                        NM_DEVICE_TYPE_DESC,
                        "802.11 Wi-Fi P2P",
                        NM_DEVICE_DEVICE_TYPE,
                        NM_DEVICE_TYPE_WIFI_P2P,
                        NM_DEVICE_LINK_TYPE,
                        NM_LINK_TYPE_WIFI,
                        NULL);
}

static void
dispose(GObject *object)
{
    NMDeviceWifiP2P        *self = NM_DEVICE_WIFI_P2P(object);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(object);

    g_clear_object(&priv->sup_mgr);

    supplicant_interfaces_release(self, FALSE);

    G_OBJECT_CLASS(nm_device_wifi_p2p_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
    NMDeviceWifiP2P        *peer = NM_DEVICE_WIFI_P2P(object);
    NMDeviceWifiP2PPrivate *priv = NM_DEVICE_WIFI_P2P_GET_PRIVATE(peer);

    nm_assert(c_list_is_empty(&priv->peers_lst_head));

    G_OBJECT_CLASS(nm_device_wifi_p2p_parent_class)->finalize(object);
}

static void
nm_device_wifi_p2p_class_init(NMDeviceWifiP2PClass *klass)
{
    GObjectClass      *object_class      = G_OBJECT_CLASS(klass);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(klass);
    NMDeviceClass     *device_class      = NM_DEVICE_CLASS(klass);

    object_class->constructed  = constructed;
    object_class->get_property = get_property;
    object_class->dispose      = dispose;
    object_class->finalize     = finalize;

    dbus_object_class->interface_infos = NM_DBUS_INTERFACE_INFOS(&interface_info_device_wifi_p2p);

    device_class->connection_type_supported        = NM_SETTING_WIFI_P2P_SETTING_NAME;
    device_class->connection_type_check_compatible = NM_SETTING_WIFI_P2P_SETTING_NAME;
    device_class->link_types           = NM_DEVICE_DEFINE_LINK_TYPES(NM_LINK_TYPE_WIFI_P2P);
    device_class->get_type_description = get_type_description;

    /* Do we need compatibility checking or is the default good enough? */
    device_class->is_available                = is_available;
    device_class->check_connection_compatible = check_connection_compatible;
    device_class->complete_connection         = complete_connection;

    device_class->act_stage1_prepare        = act_stage1_prepare;
    device_class->act_stage2_config         = act_stage2_config;
    device_class->get_configured_mtu        = get_configured_mtu;
    device_class->get_auto_ip_config_method = get_auto_ip_config_method;
    device_class->act_stage3_ip_config      = act_stage3_ip_config;
    device_class->set_enabled               = set_enabled;
    device_class->get_enabled               = get_enabled;

    device_class->deactivate        = deactivate;
    device_class->unmanaged_on_quit = unmanaged_on_quit;

    device_class->state_changed = device_state_changed;

    device_class->rfkill_type = NM_RFKILL_TYPE_WLAN;

    obj_properties[PROP_PEERS] = g_param_spec_boxed(NM_DEVICE_WIFI_P2P_PEERS,
                                                    "",
                                                    "",
                                                    G_TYPE_STRV,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);
}
