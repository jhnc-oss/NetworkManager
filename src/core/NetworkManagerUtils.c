/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2004 - 2016 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "NetworkManagerUtils.h"

#include <linux/fib_rules.h>
#include <linux/pkt_sched.h>
#include <linux/if_ether.h>

#include "libnm-glib-aux/nm-c-list.h"

#include "libnm-glib-aux/nm-uuid.h"
#include "libnm-glib-aux/nm-str-buf.h"
#include "libnm-base/nm-net-aux.h"
#include "libnm-core-aux-intern/nm-libnm-core-utils.h"
#include "libnm-core-aux-intern/nm-common-macros.h"
#include "nm-utils.h"
#include "nm-setting-connection.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-ip6-config.h"
#include "settings/nm-settings.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-platform/nmp-object.h"

#include "libnm-platform/nm-platform.h"
#include "libnm-platform/nm-linux-platform.h"
#include "libnm-platform/nm-platform-utils.h"
#include "nm-auth-utils.h"
#include "devices/nm-device.h"

/*****************************************************************************/

G_STATIC_ASSERT(NM_SHUTDOWN_TIMEOUT_1500_MSEC <= NM_SHUTDOWN_TIMEOUT_MAX_MSEC);
G_STATIC_ASSERT(NM_SHUTDOWN_TIMEOUT_5000_MSEC <= NM_SHUTDOWN_TIMEOUT_MAX_MSEC);

/*****************************************************************************/

/**
 * nm_utils_get_shared_wifi_permission:
 * @connection: the NMConnection to lookup the permission.
 *
 * Returns: a static string of the wifi-permission (if any) or %NULL.
 */
const char *
nm_utils_get_shared_wifi_permission(NMConnection *connection)
{
    NMSettingWireless         *s_wifi;
    NMSettingWirelessSecurity *s_wsec;
    const char                *method;

    method = nm_utils_get_ip_config_method(connection, AF_INET);
    if (!nm_streq(method, NM_SETTING_IP4_CONFIG_METHOD_SHARED))
        return NULL;

    s_wifi = nm_connection_get_setting_wireless(connection);
    if (s_wifi) {
        s_wsec = nm_connection_get_setting_wireless_security(connection);
        if (s_wsec)
            return NM_AUTH_PERMISSION_WIFI_SHARE_PROTECTED;
        else
            return NM_AUTH_PERMISSION_WIFI_SHARE_OPEN;
    }

    return NULL;
}

/*****************************************************************************/

static char *
get_new_connection_name(NMConnection *const *existing_connections,
                        const char          *preferred,
                        const char          *fallback_prefix)
{
    gs_free const char **existing_names = NULL;
    guint                i, existing_len = 0;

    g_assert(fallback_prefix);

    if (existing_connections) {
        existing_len   = NM_PTRARRAY_LEN(existing_connections);
        existing_names = g_new(const char *, existing_len);
        for (i = 0; i < existing_len; i++) {
            NMConnection *candidate;
            const char   *id;

            candidate = existing_connections[i];
            nm_assert(NM_IS_CONNECTION(candidate));

            id = nm_connection_get_id(candidate);
            nm_assert(id);

            existing_names[i] = id;

            if (preferred && nm_streq(preferred, id)) {
                /* the preferred name is already taken. Forget about it. */
                preferred = NULL;
            }
        }
        nm_assert(!existing_connections[i]);
    }

    /* Return the preferred name if it was unique */
    if (preferred)
        return g_strdup(preferred);

    /* Otherwise, find the next available unique connection name using the given
     * connection name template.
     */
    for (i = 1; TRUE; i++) {
        char *temp;

        /* TRANSLATORS: the first %s is a prefix for the connection id, such
         * as "Wired Connection" or "VPN Connection". The %d is a number
         * that is combined with the first argument to create a unique
         * connection id. */
        temp = g_strdup_printf(C_("connection id fallback", "%s %u"), fallback_prefix, i);

        if (!nm_strv_contains(existing_names, existing_len, temp))
            return temp;

        g_free(temp);
    }
}

static char *
get_new_connection_ifname(NMPlatform          *platform,
                          NMConnection *const *existing_connections,
                          const char          *prefix)
{
    guint i, j;

    for (i = 0; TRUE; i++) {
        char *name;

        name = g_strdup_printf("%s%d", prefix, i);

        if (nm_platform_link_get_by_ifname(platform, name))
            goto next;

        if (existing_connections) {
            for (j = 0; existing_connections[j]; j++) {
                if (nm_streq0(nm_connection_get_interface_name(existing_connections[j]), name))
                    goto next;
            }
        }

        return name;

next:
        g_free(name);
    }
}

const char *
nm_utils_get_ip_config_method(NMConnection *connection, int addr_family)
{
    NMSettingConnection *s_con;
    NMSettingIPConfig   *s_ip;
    const char          *method;

    s_con = nm_connection_get_setting_connection(connection);

    if (NM_IS_IPv4(addr_family)) {
        g_return_val_if_fail(s_con != NULL, NM_SETTING_IP4_CONFIG_METHOD_AUTO);

        s_ip = nm_connection_get_setting_ip4_config(connection);
        if (!s_ip)
            return NM_SETTING_IP4_CONFIG_METHOD_DISABLED;

        method = nm_setting_ip_config_get_method(s_ip);
        g_return_val_if_fail(method != NULL, NM_SETTING_IP4_CONFIG_METHOD_AUTO);
        return method;
    }

    g_return_val_if_fail(s_con != NULL, NM_SETTING_IP6_CONFIG_METHOD_AUTO);

    s_ip = nm_connection_get_setting_ip6_config(connection);
    if (!s_ip)
        return NM_SETTING_IP6_CONFIG_METHOD_IGNORE;

    method = nm_setting_ip_config_get_method(s_ip);
    g_return_val_if_fail(method != NULL, NM_SETTING_IP6_CONFIG_METHOD_AUTO);
    return method;
}

gboolean
nm_utils_connection_has_default_route(NMConnection *connection,
                                      int           addr_family,
                                      gboolean     *out_is_never_default)
{
    const char        *method;
    NMSettingIPConfig *s_ip;
    gboolean           is_never_default  = FALSE;
    gboolean           has_default_route = FALSE;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(NM_IN_SET(addr_family, AF_INET, AF_INET6), FALSE);

    if (!connection)
        goto out;

    s_ip = nm_connection_get_setting_ip_config(connection, addr_family);
    if (!s_ip)
        goto out;
    if (nm_setting_ip_config_get_never_default(s_ip)) {
        is_never_default = TRUE;
        goto out;
    }

    method = nm_utils_get_ip_config_method(connection, addr_family);
    if (NM_IS_IPv4(addr_family)) {
        if (NM_IN_STRSET(method,
                         NM_SETTING_IP4_CONFIG_METHOD_DISABLED,
                         NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL))
            goto out;
    } else {
        if (NM_IN_STRSET(method,
                         NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
                         NM_SETTING_IP6_CONFIG_METHOD_DISABLED,
                         NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL))
            goto out;
    }

    has_default_route = TRUE;
out:
    NM_SET_OUT(out_is_never_default, is_never_default);
    return has_default_route;
}

/*****************************************************************************/

void
nm_utils_ppp_ip_methods_enabled(NMConnection *connection,
                                gboolean     *out_ip4_enabled,
                                gboolean     *out_ip6_enabled)
{
    NM_SET_OUT(out_ip4_enabled,
               nm_streq0(nm_utils_get_ip_config_method(connection, AF_INET),
                         NM_SETTING_IP4_CONFIG_METHOD_AUTO));
    NM_SET_OUT(out_ip6_enabled,
               nm_streq0(nm_utils_get_ip_config_method(connection, AF_INET6),
                         NM_SETTING_IP6_CONFIG_METHOD_AUTO));
}

/*****************************************************************************/

void
nm_utils_complete_generic(NMPlatform          *platform,
                          NMConnection        *connection,
                          const char          *ctype,
                          NMConnection *const *existing_connections,
                          const char          *preferred_id,
                          const char          *fallback_id_prefix,
                          const char          *ifname_prefix,
                          const char          *ifname)
{
    NMSettingConnection           *s_con;
    char                          *id;
    char                          *generated_ifname;
    gs_unref_hashtable GHashTable *parameters = NULL;

    g_assert(fallback_id_prefix);
    g_return_if_fail(ifname_prefix == NULL || ifname == NULL);

    s_con = _nm_connection_ensure_setting(connection, NM_TYPE_SETTING_CONNECTION);
    g_object_set(G_OBJECT(s_con), NM_SETTING_CONNECTION_TYPE, ctype, NULL);

    if (!nm_setting_connection_get_uuid(s_con)) {
        char uuid[37];

        g_object_set(G_OBJECT(s_con),
                     NM_SETTING_CONNECTION_UUID,
                     nm_uuid_generate_random_str_arr(uuid),
                     NULL);
    }

    /* Add a connection ID if absent */
    if (!nm_setting_connection_get_id(s_con)) {
        id = get_new_connection_name(existing_connections, preferred_id, fallback_id_prefix);
        g_object_set(G_OBJECT(s_con), NM_SETTING_CONNECTION_ID, id, NULL);
        g_free(id);
    }

    /* Add an interface name, if requested */
    if (nm_setting_connection_get_interface_name(s_con)) {
        /* pass */
    } else if (ifname) {
        g_object_set(G_OBJECT(s_con), NM_SETTING_CONNECTION_INTERFACE_NAME, ifname, NULL);
    } else if (ifname_prefix && !nm_setting_connection_get_interface_name(s_con)) {
        generated_ifname = get_new_connection_ifname(platform, existing_connections, ifname_prefix);
        g_object_set(G_OBJECT(s_con), NM_SETTING_CONNECTION_INTERFACE_NAME, generated_ifname, NULL);
        g_free(generated_ifname);
    }

    if (nm_connection_get_setting_adsl(connection) || nm_connection_get_setting_cdma(connection)
        || nm_connection_get_setting_olpc_mesh(connection)
        || nm_connection_get_setting_pppoe(connection)
        || nm_connection_get_setting_vpn(connection)) {
        parameters = g_hash_table_new(nm_str_hash, g_str_equal);
        g_hash_table_insert(parameters,
                            NM_CONNECTION_NORMALIZE_PARAM_IP6_CONFIG_METHOD,
                            NM_SETTING_IP6_CONFIG_METHOD_IGNORE);
    } else {
        parameters = NULL;
    }

    /* We ignore the result, because the caller validates the connection.
     * The only reason we do a normalization attempt here is
     * NM_CONNECTION_NORMALIZE_PARAM_IP6_CONFIG_METHOD.
     * Could we perhaps, one day, get rid of it? */
    nm_connection_normalize(connection, parameters, NULL, NULL);
}

/*****************************************************************************/

static GHashTable *
check_property_in_hash(GHashTable *hash, const char *s_name, const char *p_name)
{
    GHashTable *props;

    props = g_hash_table_lookup(hash, s_name);
    if (!props || !g_hash_table_lookup(props, p_name)) {
        return NULL;
    }
    return props;
}

static void
remove_from_hash(GHashTable *s_hash, GHashTable *p_hash, const char *s_name, const char *p_name)
{
    if (!p_hash)
        return;

    g_hash_table_remove(p_hash, p_name);
    if (g_hash_table_size(p_hash) == 0)
        g_hash_table_remove(s_hash, s_name);
}

static gboolean
check_ip6_method(NMConnection *orig, NMConnection *candidate, GHashTable *settings)
{
    GHashTable        *props;
    const char        *orig_ip6_method, *candidate_ip6_method;
    NMSettingIPConfig *candidate_ip6;
    gboolean           allow = FALSE;

    props = check_property_in_hash(settings,
                                   NM_SETTING_IP6_CONFIG_SETTING_NAME,
                                   NM_SETTING_IP_CONFIG_METHOD);
    if (!props)
        return TRUE;

    orig_ip6_method      = nm_utils_get_ip_config_method(orig, AF_INET6);
    candidate_ip6_method = nm_utils_get_ip_config_method(candidate, AF_INET6);
    candidate_ip6        = nm_connection_get_setting_ip6_config(candidate);

    if (nm_streq(orig_ip6_method, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL)
        && nm_streq(candidate_ip6_method, NM_SETTING_IP6_CONFIG_METHOD_AUTO)
        && (!candidate_ip6 || nm_setting_ip_config_get_may_fail(candidate_ip6))) {
        /* If the generated connection is 'link-local' and the candidate is both 'auto'
         * and may-fail=TRUE, then the candidate is OK to use.  may-fail is included
         * in the decision because if the candidate is 'auto' but may-fail=FALSE, then
         * the connection could not possibly have been previously activated on the
         * device if the device has no non-link-local IPv6 address.
         */
        allow = TRUE;
    } else if (NM_IN_STRSET(orig_ip6_method,
                            NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL,
                            NM_SETTING_IP6_CONFIG_METHOD_DISABLED,
                            NM_SETTING_IP6_CONFIG_METHOD_AUTO)
               && nm_streq0(candidate_ip6_method, NM_SETTING_IP6_CONFIG_METHOD_IGNORE)) {
        /* If the generated connection method is 'link-local', disabled' or 'auto' and
         * the candidate method is 'ignore' we can take the connection, because NM didn't
         * simply take care of IPv6.
         */
        allow = TRUE;
    }

    if (allow) {
        remove_from_hash(settings,
                         props,
                         NM_SETTING_IP6_CONFIG_SETTING_NAME,
                         NM_SETTING_IP_CONFIG_METHOD);
    }

    return allow;
}

static int
route_compare(NMIPRoute *route1, NMIPRoute *route2, gint64 default_metric)
{
    NMIPAddr a1;
    NMIPAddr a2;
    guint64  m1;
    guint64  m2;
    int      family;
    guint    plen;

    family = nm_ip_route_get_family(route1);
    NM_CMP_DIRECT(family, nm_ip_route_get_family(route2));

    nm_assert_addr_family(family);

    plen = nm_ip_route_get_prefix(route1);
    NM_CMP_DIRECT(plen, nm_ip_route_get_prefix(route2));

    m1 = nm_ip_route_get_metric(route1);
    m2 = nm_ip_route_get_metric(route2);
    NM_CMP_DIRECT(m1 == -1 ? default_metric : m1, m2 == -1 ? default_metric : m2);

    NM_CMP_DIRECT_STRCMP0(nm_ip_route_get_next_hop(route1), nm_ip_route_get_next_hop(route2));

    if (!inet_pton(family, nm_ip_route_get_dest(route1), &a1))
        nm_assert_not_reached();
    if (!inet_pton(family, nm_ip_route_get_dest(route2), &a2))
        nm_assert_not_reached();
    nm_ip_addr_clear_host_address(family, &a1, NULL, plen);
    nm_ip_addr_clear_host_address(family, &a2, NULL, plen);
    NM_CMP_DIRECT_MEMCMP(&a1, &a2, nm_utils_addr_family_to_size(family));

    return 0;
}

static int
route_ptr_compare(const void *a, const void *b, gpointer metric)
{
    return route_compare(*(NMIPRoute **) a, *(NMIPRoute **) b, *((gint64 *) metric));
}

static gboolean
check_ip_routes(NMConnection *orig,
                NMConnection *candidate,
                GHashTable   *settings,
                gint64        default_metric,
                gboolean      v4)
{
    gs_free NMIPRoute **routes1 = NULL;
    NMIPRoute         **routes2;
    NMSettingIPConfig  *s_ip1, *s_ip2;
    gint64              m;
    const char         *s_name;
    GHashTable         *props;
    guint               i, i1, i2, num1, num2;
    const guint8        PLEN = v4 ? 32 : 128;

    s_name = v4 ? NM_SETTING_IP4_CONFIG_SETTING_NAME : NM_SETTING_IP6_CONFIG_SETTING_NAME;

    props = check_property_in_hash(settings, s_name, NM_SETTING_IP_CONFIG_ROUTES);
    if (!props)
        return TRUE;

    s_ip1 = (NMSettingIPConfig *) nm_connection_get_setting_by_name(orig, s_name);
    s_ip2 = (NMSettingIPConfig *) nm_connection_get_setting_by_name(candidate, s_name);

    if (!s_ip1 || !s_ip2)
        return FALSE;

    num1 = nm_setting_ip_config_get_num_routes(s_ip1);
    num2 = nm_setting_ip_config_get_num_routes(s_ip2);

    routes1 = g_new(NMIPRoute *, (gsize) num1 + num2);
    routes2 = &routes1[num1];

    for (i = 0; i < num1; i++)
        routes1[i] = nm_setting_ip_config_get_route(s_ip1, i);
    for (i = 0; i < num2; i++)
        routes2[i] = nm_setting_ip_config_get_route(s_ip2, i);

    m = nm_setting_ip_config_get_route_metric(s_ip2);
    if (m != -1)
        default_metric = m;

    g_qsort_with_data(routes1, num1, sizeof(NMIPRoute *), route_ptr_compare, &default_metric);
    g_qsort_with_data(routes2, num2, sizeof(NMIPRoute *), route_ptr_compare, &default_metric);

    for (i1 = 0, i2 = 0; i2 < num2; i1++) {
        if (i1 >= num1)
            return FALSE;
        if (route_compare(routes1[i1], routes2[i2], default_metric) == 0) {
            i2++;
            continue;
        }

        /* if @orig (@routes1) contains /32 routes that are missing in @candidate,
         * we accept that.
         *
         * A /32 may have been added automatically, as a direct-route to the gateway.
         * The generated connection (@orig) would contain that route, so we shall ignore
         * it.
         *
         * Likeweise for /128 for IPv6. */
        if (nm_ip_route_get_prefix(routes1[i1]) == PLEN)
            continue;

        return FALSE;
    }

    /* check that @orig has no left-over (except host routes that we ignore). */
    for (; i1 < num1; i1++) {
        if (nm_ip_route_get_prefix(routes1[i1]) != PLEN)
            return FALSE;
    }

    remove_from_hash(settings, props, s_name, NM_SETTING_IP_CONFIG_ROUTES);
    return TRUE;
}

static gboolean
check_ip4_method(NMConnection *orig,
                 NMConnection *candidate,
                 GHashTable   *settings,
                 gboolean      device_has_carrier)
{
    GHashTable        *props;
    const char        *orig_ip4_method, *candidate_ip4_method;
    NMSettingIPConfig *candidate_ip4;

    props = check_property_in_hash(settings,
                                   NM_SETTING_IP4_CONFIG_SETTING_NAME,
                                   NM_SETTING_IP_CONFIG_METHOD);
    if (!props)
        return TRUE;

    orig_ip4_method      = nm_utils_get_ip_config_method(orig, AF_INET);
    candidate_ip4_method = nm_utils_get_ip_config_method(candidate, AF_INET);
    candidate_ip4        = nm_connection_get_setting_ip4_config(candidate);

    if (nm_streq(orig_ip4_method, NM_SETTING_IP4_CONFIG_METHOD_DISABLED)
        && nm_streq(candidate_ip4_method, NM_SETTING_IP4_CONFIG_METHOD_AUTO)
        && (!candidate_ip4 || nm_setting_ip_config_get_may_fail(candidate_ip4))
        && !device_has_carrier) {
        /* If the generated connection is 'disabled' (device had no IP addresses)
         * but it has no carrier, that most likely means that IP addressing could
         * not complete and thus no IP addresses were assigned.  In that case, allow
         * matching to the "auto" method.
         */
        remove_from_hash(settings,
                         props,
                         NM_SETTING_IP4_CONFIG_SETTING_NAME,
                         NM_SETTING_IP_CONFIG_METHOD);
        return TRUE;
    }
    return FALSE;
}

static gboolean
check_connection_interface_name(NMConnection *orig, NMConnection *candidate, GHashTable *settings)
{
    GHashTable          *props;
    const char          *orig_ifname, *cand_ifname;
    NMSettingConnection *s_con_orig, *s_con_cand;

    props = check_property_in_hash(settings,
                                   NM_SETTING_CONNECTION_SETTING_NAME,
                                   NM_SETTING_CONNECTION_INTERFACE_NAME);
    if (!props)
        return TRUE;

    /* If one of the interface names is NULL, we accept that connection */
    s_con_orig  = nm_connection_get_setting_connection(orig);
    s_con_cand  = nm_connection_get_setting_connection(candidate);
    orig_ifname = nm_setting_connection_get_interface_name(s_con_orig);
    cand_ifname = nm_setting_connection_get_interface_name(s_con_cand);

    if (!orig_ifname || !cand_ifname) {
        remove_from_hash(settings,
                         props,
                         NM_SETTING_CONNECTION_SETTING_NAME,
                         NM_SETTING_CONNECTION_INTERFACE_NAME);
        return TRUE;
    }
    return FALSE;
}

static gboolean
check_connection_mac_address(NMConnection *orig, NMConnection *candidate, GHashTable *settings)
{
    GHashTable     *props;
    const char     *orig_mac = NULL, *cand_mac = NULL;
    NMSettingWired *s_wired_orig, *s_wired_cand;

    props = check_property_in_hash(settings,
                                   NM_SETTING_WIRED_SETTING_NAME,
                                   NM_SETTING_WIRED_MAC_ADDRESS);
    if (!props)
        return TRUE;

    /* If one of the MAC addresses is NULL, we accept that connection */
    s_wired_orig = nm_connection_get_setting_wired(orig);
    if (s_wired_orig)
        orig_mac = nm_setting_wired_get_mac_address(s_wired_orig);

    s_wired_cand = nm_connection_get_setting_wired(candidate);
    if (s_wired_cand)
        cand_mac = nm_setting_wired_get_mac_address(s_wired_cand);

    if (!orig_mac || !cand_mac) {
        remove_from_hash(settings,
                         props,
                         NM_SETTING_WIRED_SETTING_NAME,
                         NM_SETTING_WIRED_MAC_ADDRESS);
        return TRUE;
    }
    return FALSE;
}

static gboolean
check_connection_infiniband_mac_address(NMConnection *orig,
                                        NMConnection *candidate,
                                        GHashTable   *settings)
{
    GHashTable          *props;
    const char          *orig_mac = NULL, *cand_mac = NULL;
    NMSettingInfiniband *s_infiniband_orig, *s_infiniband_cand;

    props = check_property_in_hash(settings,
                                   NM_SETTING_INFINIBAND_SETTING_NAME,
                                   NM_SETTING_INFINIBAND_MAC_ADDRESS);
    if (!props)
        return TRUE;

    /* If one of the MAC addresses is NULL, we accept that connection */
    s_infiniband_orig = nm_connection_get_setting_infiniband(orig);
    if (s_infiniband_orig)
        orig_mac = nm_setting_infiniband_get_mac_address(s_infiniband_orig);

    s_infiniband_cand = nm_connection_get_setting_infiniband(candidate);
    if (s_infiniband_cand)
        cand_mac = nm_setting_infiniband_get_mac_address(s_infiniband_cand);

    if (!orig_mac || !cand_mac) {
        remove_from_hash(settings,
                         props,
                         NM_SETTING_INFINIBAND_SETTING_NAME,
                         NM_SETTING_INFINIBAND_MAC_ADDRESS);
        return TRUE;
    }
    return FALSE;
}

static gboolean
check_connection_cloned_mac_address(NMConnection *orig,
                                    NMConnection *candidate,
                                    GHashTable   *settings)
{
    GHashTable     *props;
    const char     *orig_mac = NULL, *cand_mac = NULL;
    NMSettingWired *s_wired_orig, *s_wired_cand;

    props = check_property_in_hash(settings,
                                   NM_SETTING_WIRED_SETTING_NAME,
                                   NM_SETTING_WIRED_CLONED_MAC_ADDRESS);
    if (!props)
        return TRUE;

    /* If one of the MAC addresses is NULL, we accept that connection */
    s_wired_orig = nm_connection_get_setting_wired(orig);
    if (s_wired_orig)
        orig_mac = nm_setting_wired_get_cloned_mac_address(s_wired_orig);

    s_wired_cand = nm_connection_get_setting_wired(candidate);
    if (s_wired_cand)
        cand_mac = nm_setting_wired_get_cloned_mac_address(s_wired_cand);

    /* special cloned mac address entries are accepted. */
    if (NM_CLONED_MAC_IS_SPECIAL(orig_mac, FALSE))
        orig_mac = NULL;
    if (NM_CLONED_MAC_IS_SPECIAL(cand_mac, FALSE))
        cand_mac = NULL;

    if (!orig_mac || !cand_mac) {
        remove_from_hash(settings,
                         props,
                         NM_SETTING_WIRED_SETTING_NAME,
                         NM_SETTING_WIRED_CLONED_MAC_ADDRESS);
        return TRUE;
    }
    return FALSE;
}

static gboolean
check_connection_controller(NMConnection *orig, NMConnection *candidate, GHashTable *settings)
{
    GHashTable           *props;
    const char           *orig_controller = NULL, *cand_controller = NULL;
    NMSettingConnection  *s_con_orig, *s_con_cand, *s_con_controller;
    NMSettingsConnection *con_controller;

    props = check_property_in_hash(settings,
                                   NM_SETTING_CONNECTION_SETTING_NAME,
                                   NM_SETTING_CONNECTION_CONTROLLER);

    if (!props)
        return TRUE;

    s_con_orig      = nm_connection_get_setting_connection(orig);
    s_con_cand      = nm_connection_get_setting_connection(candidate);
    orig_controller = nm_setting_connection_get_controller(s_con_orig);
    cand_controller = nm_setting_connection_get_controller(s_con_cand);

    /* A generated connection uses the UUID to specify the controller. Accept
     * candidates that specify as controller an interface name matching that
     * UUID */
    if (orig_controller && cand_controller) {
        if (nm_utils_is_uuid(orig_controller)) {
            con_controller = nm_settings_get_connection_by_uuid(NM_SETTINGS_GET, orig_controller);
            /* no connection found for that uuid */
            if (!con_controller)
                return FALSE;

            s_con_controller =
                nm_settings_connection_get_setting(con_controller, NM_META_SETTING_TYPE_CONNECTION);
            if (nm_streq0(nm_setting_connection_get_interface_name(s_con_controller),
                          cand_controller)) {
                remove_from_hash(settings,
                                 props,
                                 NM_SETTING_CONNECTION_SETTING_NAME,
                                 NM_SETTING_CONNECTION_MASTER);
                remove_from_hash(settings,
                                 props,
                                 NM_SETTING_CONNECTION_SETTING_NAME,
                                 NM_SETTING_CONNECTION_CONTROLLER);
                return TRUE;
            } else {
                return FALSE;
            }
        }
    }

    return FALSE;
}

static gboolean
check_connection_s390_props(NMConnection *orig, NMConnection *candidate, GHashTable *settings)
{
    GHashTable     *props1, *props2, *props3;
    NMSettingWired *s_wired_orig, *s_wired_cand;

    props1 = check_property_in_hash(settings,
                                    NM_SETTING_WIRED_SETTING_NAME,
                                    NM_SETTING_WIRED_S390_SUBCHANNELS);
    props2 = check_property_in_hash(settings,
                                    NM_SETTING_WIRED_SETTING_NAME,
                                    NM_SETTING_WIRED_S390_NETTYPE);
    props3 = check_property_in_hash(settings,
                                    NM_SETTING_WIRED_SETTING_NAME,
                                    NM_SETTING_WIRED_S390_OPTIONS);
    if (!props1 && !props2 && !props3)
        return TRUE;

    /* If the generated connection did not contain wired setting,
     * allow it to match to a connection with a wired setting,
     * but default (empty) s390-* properties */
    s_wired_orig = nm_connection_get_setting_wired(orig);
    s_wired_cand = nm_connection_get_setting_wired(candidate);
    if (!s_wired_orig && s_wired_cand) {
        const char *const *subchans    = nm_setting_wired_get_s390_subchannels(s_wired_cand);
        const char        *nettype     = nm_setting_wired_get_s390_nettype(s_wired_cand);
        guint32            num_options = nm_setting_wired_get_num_s390_options(s_wired_cand);

        if ((!subchans || !*subchans) && !nettype && num_options == 0) {
            remove_from_hash(settings,
                             props1,
                             NM_SETTING_WIRED_SETTING_NAME,
                             NM_SETTING_WIRED_S390_SUBCHANNELS);
            remove_from_hash(settings,
                             props2,
                             NM_SETTING_WIRED_SETTING_NAME,
                             NM_SETTING_WIRED_S390_NETTYPE);
            remove_from_hash(settings,
                             props3,
                             NM_SETTING_WIRED_SETTING_NAME,
                             NM_SETTING_WIRED_S390_OPTIONS);
            return TRUE;
        }
    }
    return FALSE;
}

static NMConnection *
check_possible_match(NMConnection *orig,
                     NMConnection *candidate,
                     GHashTable   *settings,
                     gboolean      device_has_carrier,
                     gint64        default_v4_metric,
                     gint64        default_v6_metric)
{
    g_return_val_if_fail(settings != NULL, NULL);

    if (!check_ip6_method(orig, candidate, settings))
        return NULL;

    if (!check_ip4_method(orig, candidate, settings, device_has_carrier))
        return NULL;

    if (!check_ip_routes(orig, candidate, settings, default_v4_metric, TRUE))
        return NULL;

    if (!check_ip_routes(orig, candidate, settings, default_v6_metric, FALSE))
        return NULL;

    if (!check_connection_interface_name(orig, candidate, settings))
        return NULL;

    if (!check_connection_mac_address(orig, candidate, settings))
        return NULL;

    if (!check_connection_infiniband_mac_address(orig, candidate, settings))
        return NULL;

    if (!check_connection_cloned_mac_address(orig, candidate, settings))
        return NULL;

    if (!check_connection_controller(orig, candidate, settings))
        return NULL;

    if (!check_connection_s390_props(orig, candidate, settings))
        return NULL;

    /* match properties are for matching from static to generated connections,
     * so they are not really part of the difference. */
    g_hash_table_remove(settings, NM_SETTING_MATCH_SETTING_NAME);

    if (g_hash_table_size(settings) == 0)
        return candidate;
    else
        return NULL;
}

/**
 * nm_utils_match_connection:
 * @connections: a (optionally pre-sorted) list of connections from which to
 * find a matching connection to @original based on "inferrable" properties
 * @original: the #NMConnection to find a match for from @connections
 * @indicated: whether the match is already hinted/indicated. That is the
 *   case when we found the connection in the state file from a previous run.
 *   In this case, we perform a relexed check, as we have a good hint
 *   that the connection actually matches.
 * @device_has_carrier: pass %TRUE if the device that generated @original has
 * a carrier, %FALSE if not
 * @match_filter_func: a function to check whether each connection from @connections
 * should be considered for matching.  This function should return %TRUE if the
 * connection should be considered, %FALSE if the connection should be ignored
 * @match_compat_data: data pointer passed to @match_filter_func
 *
 * Checks each connection from @connections until a matching connection is found
 * considering only setting properties marked with %NM_SETTING_PARAM_INFERRABLE
 * and checking a few other characteristics like IPv6 method.  If the caller
 * desires some priority order of the connections, @connections should be
 * sorted before calling this function.
 *
 * Returns: the best #NMConnection matching @original, or %NULL if no connection
 * matches well enough.
 */
NMConnection *
nm_utils_match_connection(NMConnection *const   *connections,
                          NMConnection          *original,
                          gboolean               indicated,
                          gboolean               device_has_carrier,
                          gint64                 default_v4_metric,
                          gint64                 default_v6_metric,
                          NMUtilsMatchFilterFunc match_filter_func,
                          gpointer               match_filter_data)
{
    NMConnection *best_match = NULL;

    if (!connections)
        return NULL;

    for (; *connections; connections++) {
        NMConnection *candidate = *connections;
        GHashTable   *diffs     = NULL;

        nm_assert(NM_IS_CONNECTION(candidate));

        if (match_filter_func) {
            if (!match_filter_func(candidate, match_filter_data))
                continue;
        }

        if (indicated) {
            NMSettingConnection *s_orig, *s_cand;

            s_orig = nm_connection_get_setting_connection(original);
            s_cand = nm_connection_get_setting_connection(candidate);

            /* It is indicated that this connection matches. Assume we have
             * a match, but check for particular differences that let us
             * reject the candidate. */
            if (!nm_streq0(nm_setting_connection_get_connection_type(s_orig),
                           nm_setting_connection_get_connection_type(s_cand)))
                continue;
            if (!nm_streq0(nm_setting_connection_get_port_type(s_orig),
                           nm_setting_connection_get_port_type(s_cand)))
                continue;

            /* this is good enough for a match */
        } else if (!nm_connection_diff(original,
                                       candidate,
                                       NM_SETTING_COMPARE_FLAG_INFERRABLE,
                                       &diffs)) {
            if (!best_match) {
                best_match = check_possible_match(original,
                                                  candidate,
                                                  diffs,
                                                  device_has_carrier,
                                                  default_v4_metric,
                                                  default_v6_metric);
            }

            if (!best_match && nm_logging_enabled(LOGL_DEBUG, LOGD_CORE)) {
                GString       *diff_string;
                GHashTableIter s_iter, p_iter;
                gpointer       setting_name, setting;
                gpointer       property_name, value;

                diff_string = g_string_new(NULL);
                g_hash_table_iter_init(&s_iter, diffs);
                while (g_hash_table_iter_next(&s_iter, &setting_name, &setting)) {
                    g_hash_table_iter_init(&p_iter, setting);
                    while (g_hash_table_iter_next(&p_iter, &property_name, &value)) {
                        if (diff_string->len)
                            g_string_append(diff_string, ", ");
                        g_string_append_printf(diff_string,
                                               "%s.%s",
                                               (char *) setting_name,
                                               (char *) property_name);
                    }
                }

                nm_log_dbg(LOGD_CORE,
                           "Connection '%s' differs from candidate '%s' in %s",
                           nm_connection_get_id(original),
                           nm_connection_get_id(candidate),
                           diff_string->str);
                g_string_free(diff_string, TRUE);
            }

            g_hash_table_unref(diffs);
            continue;
        }

        /* Exact match */
        return candidate;
    }

    /* Best match (if any) */
    return best_match;
}

/*****************************************************************************/

const struct _NMMatchSpecDeviceData *
nm_match_spec_device_data_init_from_device(struct _NMMatchSpecDeviceData *out_data,
                                           NMDevice                      *device)
{
    const char *hw_address;
    gboolean    is_fake;

    nm_assert(out_data);

    if (!device) {
        *out_data = (NMMatchSpecDeviceData) {};
        return out_data;
    }

    nm_assert(NM_IS_DEVICE(device));

    hw_address = nm_device_get_permanent_hw_address_full(
        device,
        !nm_device_get_unmanaged_flags(device, NM_UNMANAGED_PLATFORM_INIT),
        &is_fake);

    /* Note that here we access various getters on @device, without cloning
     * or taking ownership and return it to the caller.
     *
     * The returned data is only valid, until NMDevice gets modified again. */

    *out_data = (NMMatchSpecDeviceData) {
        .interface_name   = nm_device_get_iface(device),
        .device_type      = nm_device_get_type_description(device),
        .driver           = nm_device_get_driver(device),
        .driver_version   = nm_device_get_driver_version(device),
        .hwaddr           = is_fake ? NULL : hw_address,
        .s390_subchannels = nm_device_get_s390_subchannels(device),
        .dhcp_plugin      = nm_dhcp_manager_get_config(nm_dhcp_manager_get()),
    };

    return out_data;
}

const NMMatchSpecDeviceData *
nm_match_spec_device_data_init_from_platform(NMMatchSpecDeviceData *out_data,
                                             const NMPlatformLink  *pllink,
                                             const char            *match_device_type,
                                             const char            *match_dhcp_plugin)
{
    nm_assert(out_data);

    /* we can only match by certain properties that are available on the
     * platform link (and even @pllink might be missing.
     *
     * It's still useful because of specs like "*" and "except:interface-name:eth0",
     * which match even in that case. */

    *out_data = (NMMatchSpecDeviceData) {
        .interface_name   = pllink ? pllink->name : NULL,
        .device_type      = match_device_type,
        .driver           = pllink ? pllink->driver : NULL,
        .driver_version   = NULL,
        .hwaddr           = NULL,
        .s390_subchannels = NULL,
        .dhcp_plugin      = match_dhcp_plugin,
    };
    return out_data;
}

/*****************************************************************************/

int
nm_match_spec_device_by_pllink(const NMPlatformLink *pllink,
                               const char           *match_device_type,
                               const char           *match_dhcp_plugin,
                               const GSList         *specs,
                               int                   no_match_value)
{
    NMMatchSpecMatchType  m;
    NMMatchSpecDeviceData data;

    m = nm_match_spec_device(specs,
                             nm_match_spec_device_data_init_from_platform(&data,
                                                                          pllink,
                                                                          match_device_type,
                                                                          match_dhcp_plugin));
    return nm_match_spec_match_type_to_bool(m, no_match_value);
}

/*****************************************************************************/

NMPlatformRoutingRule *
nm_ip_routing_rule_to_platform(const NMIPRoutingRule *rule, NMPlatformRoutingRule *out_pl)
{
    gboolean uid_range_has;
    guint32  uid_range_start = 0;
    guint32  uid_range_end   = 0;

    nm_assert(rule);
    nm_assert(nm_ip_routing_rule_validate(rule, NULL));
    nm_assert(out_pl);

    uid_range_has = nm_ip_routing_rule_get_uid_range(rule, &uid_range_start, &uid_range_end);

    *out_pl = (NMPlatformRoutingRule) {
        .addr_family = nm_ip_routing_rule_get_addr_family(rule),
        .flags       = (nm_ip_routing_rule_get_invert(rule) ? FIB_RULE_INVERT : 0),
        .priority    = nm_ip_routing_rule_get_priority(rule),
        .tos         = nm_ip_routing_rule_get_tos(rule),
        .ip_proto    = nm_ip_routing_rule_get_ipproto(rule),
        .fwmark      = nm_ip_routing_rule_get_fwmark(rule),
        .fwmask      = nm_ip_routing_rule_get_fwmask(rule),
        .sport_range =
            {
                .start = nm_ip_routing_rule_get_source_port_start(rule),
                .end   = nm_ip_routing_rule_get_source_port_end(rule),
            },
        .dport_range =
            {
                .start = nm_ip_routing_rule_get_destination_port_start(rule),
                .end   = nm_ip_routing_rule_get_destination_port_end(rule),
            },
        .src     = *(nm_ip_routing_rule_get_from_bin(rule) ?: &nm_ip_addr_zero),
        .dst     = *(nm_ip_routing_rule_get_to_bin(rule) ?: &nm_ip_addr_zero),
        .src_len = nm_ip_routing_rule_get_from_len(rule),
        .dst_len = nm_ip_routing_rule_get_to_len(rule),
        .action  = nm_ip_routing_rule_get_action(rule),
        .table   = nm_ip_routing_rule_get_table(rule),
        .suppress_prefixlen_inverse =
            ~((guint32) nm_ip_routing_rule_get_suppress_prefixlength(rule)),
        .uid_range_has = uid_range_has,
        .uid_range =
            {
                .start = uid_range_start,
                .end   = uid_range_end,
            },
        .protocol = RTPROT_STATIC,
    };

    nm_ip_routing_rule_get_xifname_bin(rule, TRUE, out_pl->iifname);
    nm_ip_routing_rule_get_xifname_bin(rule, FALSE, out_pl->oifname);

    return out_pl;
}

/*****************************************************************************/

struct _NMShutdownWaitObjHandle {
    CList    lst;
    gpointer watched_obj;
    char    *msg_reason;
    bool     free_msg_reason : 1;
    bool     is_cancellable : 1;
};

static CList _shutdown_waitobj_lst_head;

static void
_shutdown_waitobj_unregister(NMShutdownWaitObjHandle *handle)
{
    c_list_unlink_stale(&handle->lst);
    if (handle->free_msg_reason)
        g_free(handle->msg_reason);
    g_slice_free(NMShutdownWaitObjHandle, handle);

    /* FIXME(shutdown): check whether the object list is empty, and
     * signal shutdown-complete */
}

static void
_shutdown_waitobj_cb(gpointer user_data, GObject *where_the_object_was)
{
    NMShutdownWaitObjHandle *handle = user_data;

    nm_assert(handle);
    nm_assert(handle->watched_obj == where_the_object_was);
    _shutdown_waitobj_unregister(handle);
}

/**
 * nm_shutdown_wait_obj_register_full:
 * @watched_obj: the object to watch. Takes a weak reference on the object
 *   to be notified when it gets destroyed.
 *   If wait_type is %NM_SHUTDOWN_WAIT_TYPE_HANDLE, this must be %NULL.
 * @wait_type: whether @watched_obj is just a plain GObject or a GCancellable
 *   that should be cancelled.
 * @msg_reason: a reason message, for debugging and logging purposes.
 * @free_msg_reason: if %TRUE, then ownership of @msg_reason will be taken
 *   and the string will be freed with g_free() afterwards. If %FALSE,
 *   the caller must ensure that @msg_reason string outlives the watched
 *   objects (e.g. being a static strings).
 *
 * Keep track of @watched_obj until it gets destroyed. During shutdown,
 * we wait until all watched objects are destroyed. This is useful, if
 * this object still conducts some asynchronous action, which needs to
 * complete before NetworkManager is allowed to terminate. We re-use
 * the reference-counter of @watched_obj as signal, that the object
 * is still used.
 *
 * If @wait_type is %NM_SHUTDOWN_WAIT_TYPE_CANCELLABLE, then during shutdown
 * (after %NM_SHUTDOWN_TIMEOUT_MAX_MSEC), the cancellable will be cancelled to notify
 * the source of the shutdown. Note that otherwise, in this mode also @watched_obj
 * is only tracked with a weak-pointer. Especially, it does not register to the
 * "cancelled" signal to automatically unregister (otherwise, you would never
 * know whether the returned NMShutdownWaitObjHandle is still valid.
 *
 * FIXME(shutdown): proper shutdown is not yet implemented, and registering
 *   an object (currently) has no effect.
 *
 * FIXME(shutdown): during shutdown, after %NM_SHUTDOWN_TIMEOUT_MAX_MSEC timeout, cancel
 *   all remaining %NM_SHUTDOWN_WAIT_TYPE_CANCELLABLE instances. Also, when somebody
 *   enqueues a cancellable after that point, cancel it right away on an idle handler.
 *
 * Returns: a handle to unregister the object. The caller may choose to ignore
 *   the handle, in which case, the object will be automatically unregistered,
 *   once it gets destroyed.
 *   Note that the handle is only valid as long as @watched_obj exists. If
 *   you plan to use it, ensure that you take care of not using it after
 *   destroying @watched_obj.
 */
NMShutdownWaitObjHandle *
nm_shutdown_wait_obj_register_full(gpointer           watched_obj,
                                   NMShutdownWaitType wait_type,
                                   char              *msg_reason,
                                   gboolean           free_msg_reason)
{
    NMShutdownWaitObjHandle *handle;

    if (wait_type == NM_SHUTDOWN_WAIT_TYPE_OBJECT)
        g_return_val_if_fail(G_IS_OBJECT(watched_obj), NULL);
    else if (wait_type == NM_SHUTDOWN_WAIT_TYPE_CANCELLABLE)
        g_return_val_if_fail(G_IS_CANCELLABLE(watched_obj), NULL);
    else if (wait_type == NM_SHUTDOWN_WAIT_TYPE_HANDLE)
        g_return_val_if_fail(!watched_obj, NULL);
    else
        g_return_val_if_reached(NULL);

    if (G_UNLIKELY(!_shutdown_waitobj_lst_head.next))
        c_list_init(&_shutdown_waitobj_lst_head);

    /* Beware: there are callers with g_main_context_get_thread_default()
     * not being g_main_context_get_default(). For example _fw_nft_call().
     *
     * If you schedule any sources or async operations, you probably need to
     * make sure to use the default context. */

    handle  = g_slice_new(NMShutdownWaitObjHandle);
    *handle = (NMShutdownWaitObjHandle) {
        /* depending on @free_msg_reason, we take ownership of @msg_reason.
         * In either case, we just reference the string without cloning
         * it. */
        .watched_obj     = watched_obj,
        .msg_reason      = msg_reason,
        .free_msg_reason = free_msg_reason,
        .is_cancellable  = (wait_type == NM_SHUTDOWN_WAIT_TYPE_CANCELLABLE),
    };
    c_list_link_tail(&_shutdown_waitobj_lst_head, &handle->lst);
    if (watched_obj)
        g_object_weak_ref(watched_obj, _shutdown_waitobj_cb, handle);
    return handle;
}

void
nm_shutdown_wait_obj_unregister(NMShutdownWaitObjHandle *handle)
{
    g_return_if_fail(handle);

    nm_assert(!handle->watched_obj || G_IS_OBJECT(handle->watched_obj));
    nm_assert(nm_c_list_contains_entry(&_shutdown_waitobj_lst_head, handle, lst));

    if (handle->watched_obj)
        g_object_weak_unref(handle->watched_obj, _shutdown_waitobj_cb, handle);
    _shutdown_waitobj_unregister(handle);
}

/*****************************************************************************/

/**
 * nm_utils_file_is_in_path:
 * @abs_filename: the absolute filename to test
 * @abs_path: the absolute path, to check whether filename is in.
 *
 * This tests, whether @abs_filename is a file which lies inside @abs_path.
 * Basically, this checks whether @abs_filename is the same as @abs_path +
 * basename(@abs_filename). It allows simple normalizations, like coalescing
 * multiple "//".
 *
 * However, beware that this function is purely filename based. That means,
 * it will reject files that reference the same file (i.e. inode) via
 * symlinks or bind mounts. Maybe one would like to check for file (inode)
 * identity, but that is not really possible based on the file name alone.
 *
 * This means, that nm_utils_file_is_in_path("/var/run/some-file", "/var/run")
 * will succeed, but nm_utils_file_is_in_path("/run/some-file", "/var/run")
 * will not (although, it's well known that they reference the same path).
 *
 * Also, note that @abs_filename must not have trailing slashes itself.
 * So, this will reject nm_utils_file_is_in_path("/usr/lib/", "/usr") as
 * invalid, because the function searches for file names (and "lib/" is
 * clearly a directory).
 *
 * Returns: if @abs_filename is a file inside @abs_path, returns the
 *   trailing part of @abs_filename which is the filename. Otherwise,
 *   %NULL.
 */
const char *
nm_utils_file_is_in_path(const char *abs_filename, const char *abs_path)
{
    const char *path;

    g_return_val_if_fail(abs_filename && abs_filename[0] == '/', NULL);
    g_return_val_if_fail(abs_path && abs_path[0] == '/', NULL);

    path = nm_path_startswith(abs_filename, abs_path);
    if (!path)
        return NULL;

    nm_assert(path[0] != '/');
    nm_assert(path > abs_filename);
    nm_assert(path <= &abs_filename[strlen(abs_filename)]);

    /* we require a non-empty remainder with no slashes. That is, only a filename.
     *
     * Note this will reject "/var/run/" as not being in "/var",
     * while "/var/run" would pass. The function searches for files
     * only, so a trailing slash (indicating a directory) is not allowed).
     * This is despite that the function cannot determine whether "/var/run"
     * is itself a file or a directory. "*/
    return path[0] && !strchr(path, '/') ? path : NULL;
}

/* The returned qdisc array is valid as long as s_tc is not modified */
GPtrArray *
nm_utils_qdiscs_from_tc_setting(NMPlatform *platform, NMSettingTCConfig *s_tc, int ip_ifindex)
{
    GPtrArray *qdiscs;
    guint      nqdiscs;
    guint      i;

    nqdiscs = nm_setting_tc_config_get_num_qdiscs(s_tc);
    qdiscs  = g_ptr_array_new_full(nqdiscs, (GDestroyNotify) nmp_object_unref);

    for (i = 0; i < nqdiscs; i++) {
        NMTCQdisc       *s_qdisc = nm_setting_tc_config_get_qdisc(s_tc, i);
        NMPObject       *q       = nmp_object_new(NMP_OBJECT_TYPE_QDISC, NULL);
        NMPlatformQdisc *qdisc   = NMP_OBJECT_CAST_QDISC(q);

        qdisc->ifindex = ip_ifindex;
        qdisc->kind    = nm_tc_qdisc_get_kind(s_qdisc);

        qdisc->addr_family = AF_UNSPEC;
        qdisc->handle      = nm_tc_qdisc_get_handle(s_qdisc);
        qdisc->parent      = nm_tc_qdisc_get_parent(s_qdisc);
        qdisc->info        = 0;

#define GET_ATTR(name, dst, variant_type, type, dflt)                                  \
    G_STMT_START                                                                       \
    {                                                                                  \
        GVariant *_variant = nm_tc_qdisc_get_attribute(s_qdisc, "" name "");           \
                                                                                       \
        if (_variant && g_variant_is_of_type(_variant, G_VARIANT_TYPE_##variant_type)) \
            (dst) = g_variant_get_##type(_variant);                                    \
        else                                                                           \
            (dst) = (dflt);                                                            \
    }                                                                                  \
    G_STMT_END

        if (strcmp(qdisc->kind, "fq_codel") == 0) {
            GET_ATTR("limit", qdisc->fq_codel.limit, UINT32, uint32, 0);
            GET_ATTR("flows", qdisc->fq_codel.flows, UINT32, uint32, 0);
            GET_ATTR("target", qdisc->fq_codel.target, UINT32, uint32, 0);
            GET_ATTR("interval", qdisc->fq_codel.interval, UINT32, uint32, 0);
            GET_ATTR("quantum", qdisc->fq_codel.quantum, UINT32, uint32, 0);
            GET_ATTR("ce_threshold",
                     qdisc->fq_codel.ce_threshold,
                     UINT32,
                     uint32,
                     NM_PLATFORM_FQ_CODEL_CE_THRESHOLD_DISABLED);
            GET_ATTR("memory_limit",
                     qdisc->fq_codel.memory_limit,
                     UINT32,
                     uint32,
                     NM_PLATFORM_FQ_CODEL_MEMORY_LIMIT_UNSET);
            GET_ATTR("ecn", qdisc->fq_codel.ecn, BOOLEAN, boolean, FALSE);
        } else if (nm_streq(qdisc->kind, "sfq")) {
            GET_ATTR("limit", qdisc->sfq.limit, UINT32, uint32, 0);
            GET_ATTR("flows", qdisc->sfq.flows, UINT32, uint32, 0);
            GET_ATTR("divisor", qdisc->sfq.divisor, UINT32, uint32, 0);
            GET_ATTR("perturb", qdisc->sfq.perturb_period, INT32, int32, 0);
            GET_ATTR("quantum", qdisc->sfq.quantum, UINT32, uint32, 0);
            GET_ATTR("depth", qdisc->sfq.depth, UINT32, uint32, 0);
        } else if (nm_streq(qdisc->kind, "tbf")) {
            GET_ATTR("rate", qdisc->tbf.rate, UINT64, uint64, 0);
            GET_ATTR("burst", qdisc->tbf.burst, UINT32, uint32, 0);
            GET_ATTR("limit", qdisc->tbf.limit, UINT32, uint32, 0);
            GET_ATTR("latency", qdisc->tbf.latency, UINT32, uint32, 0);
        }
#undef GET_ATTR

        g_ptr_array_add(qdiscs, q);
    }

    return qdiscs;
}

/* The returned tfilter array is valid as long as s_tc is not modified */
GPtrArray *
nm_utils_tfilters_from_tc_setting(NMPlatform *platform, NMSettingTCConfig *s_tc, int ip_ifindex)
{
    GPtrArray *tfilters;
    guint      ntfilters;
    guint      i;

    ntfilters = nm_setting_tc_config_get_num_tfilters(s_tc);
    tfilters  = g_ptr_array_new_full(ntfilters, (GDestroyNotify) nmp_object_unref);

    for (i = 0; i < ntfilters; i++) {
        NMTCTfilter       *s_tfilter = nm_setting_tc_config_get_tfilter(s_tc, i);
        NMTCAction        *action;
        NMPObject         *t       = nmp_object_new(NMP_OBJECT_TYPE_TFILTER, NULL);
        NMPlatformTfilter *tfilter = NMP_OBJECT_CAST_TFILTER(t);

        tfilter->ifindex     = ip_ifindex;
        tfilter->kind        = nm_tc_tfilter_get_kind(s_tfilter);
        tfilter->addr_family = AF_UNSPEC;
        tfilter->handle      = nm_tc_tfilter_get_handle(s_tfilter);
        tfilter->parent      = nm_tc_tfilter_get_parent(s_tfilter);
        tfilter->info        = TC_H_MAKE(0, htons(ETH_P_ALL));

        action = nm_tc_tfilter_get_action(s_tfilter);
        if (action) {
            GVariant *var;

            tfilter->action.kind = nm_tc_action_get_kind(action);

            if (strcmp(tfilter->action.kind, "simple") == 0) {
                var = nm_tc_action_get_attribute(action, "sdata");
                if (var && g_variant_is_of_type(var, G_VARIANT_TYPE_BYTESTRING)) {
                    g_strlcpy(tfilter->action.simple.sdata,
                              g_variant_get_bytestring(var),
                              sizeof(tfilter->action.simple.sdata));
                }
            } else if (strcmp(tfilter->action.kind, "mirred") == 0) {
                if (nm_tc_action_get_attribute(action, "egress"))
                    tfilter->action.mirred.egress = TRUE;

                if (nm_tc_action_get_attribute(action, "ingress"))
                    tfilter->action.mirred.ingress = TRUE;

                if (nm_tc_action_get_attribute(action, "mirror"))
                    tfilter->action.mirred.mirror = TRUE;

                if (nm_tc_action_get_attribute(action, "redirect"))
                    tfilter->action.mirred.redirect = TRUE;

                var = nm_tc_action_get_attribute(action, "dev");
                if (var && g_variant_is_of_type(var, G_VARIANT_TYPE_STRING)) {
                    int ifindex;

                    ifindex =
                        nm_platform_link_get_ifindex(platform, g_variant_get_string(var, NULL));
                    if (ifindex > 0)
                        tfilter->action.mirred.ifindex = ifindex;
                }
            }
        }

        g_ptr_array_add(tfilters, t);
    }

    return tfilters;
}

void
nm_utils_ip_route_attribute_to_platform(int                addr_family,
                                        NMIPRoute         *s_route,
                                        NMPlatformIPRoute *r,
                                        gint64             route_table)
{
    const int           IS_IPv4 = NM_IS_IPv4(addr_family);
    GVariant           *variant;
    guint32             table;
    NMIPAddr            addr;
    NMPlatformIP4Route *r4 = (NMPlatformIP4Route *) r;
    NMPlatformIP6Route *r6 = (NMPlatformIP6Route *) r;
    gboolean            onlink;

    nm_assert(s_route);
    nm_assert(addr_family == nm_ip_route_get_family(s_route));
    nm_assert(r);
    nm_assert(route_table >= -1);
    nm_assert(route_table <= (gint64) G_MAXUINT32);

#define GET_ATTR(name, dst, variant_type, type, dflt)                                  \
    G_STMT_START                                                                       \
    {                                                                                  \
        GVariant *_variant = nm_ip_route_get_attribute(s_route, "" name "");           \
                                                                                       \
        if (_variant && g_variant_is_of_type(_variant, G_VARIANT_TYPE_##variant_type)) \
            (dst) = g_variant_get_##type(_variant);                                    \
        else                                                                           \
            (dst) = (dflt);                                                            \
    }                                                                                  \
    G_STMT_END

    if ((variant = nm_ip_route_get_attribute(s_route, NM_IP_ROUTE_ATTRIBUTE_TYPE))
        && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)) {
        int type;

        type = nm_net_aux_rtnl_rtntype_a2n(g_variant_get_string(variant, NULL));
        nm_assert(NM_IN_SET(type,
                            RTN_UNICAST,
                            RTN_LOCAL,
                            RTN_BLACKHOLE,
                            RTN_UNREACHABLE,
                            RTN_PROHIBIT,
                            RTN_THROW));

        r->type_coerced = nm_platform_route_type_coerce(type);
    } else
        r->type_coerced = nm_platform_route_type_coerce(RTN_UNICAST);

    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_TABLE, table, UINT32, uint32, 0);

    if (table != 0)
        r->table_coerced = nm_platform_route_table_coerce(table);
    else if (r->type_coerced == nm_platform_route_type_coerce(RTN_LOCAL))
        r->table_coerced = nm_platform_route_table_coerce(RT_TABLE_LOCAL);
    else if (route_table == 0)
        r->table_coerced = nm_platform_route_table_coerce(RT_TABLE_MAIN);
    else if (route_table > 0)
        r->table_coerced = nm_platform_route_table_coerce(route_table);
    else
        r->table_any = TRUE;

    if (IS_IPv4) {
        guint8 scope;

        GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_TOS, r4->tos, BYTE, byte, 0);
        GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_SCOPE, scope, BYTE, byte, RT_SCOPE_NOWHERE);
        GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_WEIGHT, r4->weight, UINT32, uint32, 0);
        r4->scope_inv = nm_platform_route_scope_inv(scope);
    }

    /* Note that for IPv4 routes in kernel, the onlink flag can be set for
     * each next hop separately (rtnh_flags). Not for NetworkManager. We can
     * only merge routes as ECMP routes (when setting a weight) if they all
     * share the same onlink flag. See NM_PLATFORM_IP_ROUTE_CMP_TYPE_ECMP_ID.
     * That simplifies the code. */
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_ONLINK, onlink, BOOLEAN, boolean, FALSE);
    r->r_rtm_flags = ((onlink) ? (unsigned) RTNH_F_ONLINK : 0u);

    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_ADVMSS, r->mss, UINT32, uint32, 0);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_WINDOW, r->window, UINT32, uint32, 0);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_CWND, r->cwnd, UINT32, uint32, 0);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_INITCWND, r->initcwnd, UINT32, uint32, 0);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_INITRWND, r->initrwnd, UINT32, uint32, 0);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_MTU, r->mtu, UINT32, uint32, 0);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_QUICKACK, r->quickack, BOOLEAN, boolean, FALSE);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_LOCK_WINDOW, r->lock_window, BOOLEAN, boolean, FALSE);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_LOCK_CWND, r->lock_cwnd, BOOLEAN, boolean, FALSE);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_LOCK_INITCWND, r->lock_initcwnd, BOOLEAN, boolean, FALSE);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_LOCK_INITRWND, r->lock_initrwnd, BOOLEAN, boolean, FALSE);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_LOCK_MTU, r->lock_mtu, BOOLEAN, boolean, FALSE);
    GET_ATTR(NM_IP_ROUTE_ATTRIBUTE_LOCK_ADVMSS, r->lock_mss, BOOLEAN, boolean, FALSE);

    {
        GVariant *_variant = nm_ip_route_get_attribute(s_route, NM_IP_ROUTE_ATTRIBUTE_RTO_MIN);

        if (_variant && g_variant_is_of_type(_variant, G_VARIANT_TYPE_UINT32)) {
            r->rto_min     = g_variant_get_uint32(_variant);
            r->rto_min_set = TRUE;
        } else {
            r->rto_min     = 0;
            r->rto_min_set = FALSE;
        }
    }

    if ((variant = nm_ip_route_get_attribute(s_route, NM_IP_ROUTE_ATTRIBUTE_SRC))
        && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)) {
        if (inet_pton(addr_family, g_variant_get_string(variant, NULL), &addr) == 1) {
            if (IS_IPv4)
                r4->pref_src = addr.addr4;
            else
                r6->pref_src = addr.addr6;
        }
    }

    if (!IS_IPv4 && (variant = nm_ip_route_get_attribute(s_route, NM_IP_ROUTE_ATTRIBUTE_FROM))
        && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)) {
        int prefix;

        if (nm_inet_parse_with_prefix_bin(addr_family,
                                          g_variant_get_string(variant, NULL),
                                          NULL,
                                          &addr,
                                          &prefix)) {
            if (prefix < 0)
                prefix = 128;
            r6->src      = addr.addr6;
            r6->src_plen = prefix;
        }
    }
#undef GET_ATTR
}

/*****************************************************************************/

void
nm_utils_ip_addresses_to_dbus(int                          addr_family,
                              const NMDedupMultiHeadEntry *head_entry,
                              const NMPObject             *best_default_route,
                              GVariant                   **out_address_data,
                              GVariant                   **out_addresses)
{
    const int        IS_IPv4 = NM_IS_IPv4(addr_family);
    GVariantBuilder  builder_data;
    GVariantBuilder  builder_legacy;
    char             addr_str[NM_INET_ADDRSTRLEN];
    NMDedupMultiIter iter;
    const NMPObject *obj;
    const gsize      MAX_ADDRESSES = 100;
    gsize            i;

    nm_assert_addr_family(addr_family);

    if (out_address_data)
        g_variant_builder_init(&builder_data, G_VARIANT_TYPE("aa{sv}"));
    if (out_addresses) {
        if (IS_IPv4)
            g_variant_builder_init(&builder_legacy, G_VARIANT_TYPE("aau"));
        else
            g_variant_builder_init(&builder_legacy, G_VARIANT_TYPE("a(ayuay)"));
    }

    if (!head_entry)
        goto out;

    i = 0;
    nm_dedup_multi_iter_init(&iter, head_entry);
    while (
        nm_platform_dedup_multi_iter_next_obj(&iter, &obj, NMP_OBJECT_TYPE_IP_ADDRESS(IS_IPv4))) {
        const NMPlatformIPXAddress *address = NMP_OBJECT_CAST_IPX_ADDRESS(obj);

        if (i > MAX_ADDRESSES) {
            /* Limited. The rest is hidden. */
            break;
        }

        if (out_address_data) {
            GVariantBuilder addr_builder;
            gconstpointer   p;

            g_variant_builder_init(&addr_builder, G_VARIANT_TYPE("a{sv}"));

            g_variant_builder_add(
                &addr_builder,
                "{sv}",
                "address",
                g_variant_new_string(nm_inet_ntop(addr_family, address->ax.address_ptr, addr_str)));

            g_variant_builder_add(&addr_builder,
                                  "{sv}",
                                  "prefix",
                                  g_variant_new_uint32(address->ax.plen));

            p = NULL;
            if (IS_IPv4) {
                if (address->a4.peer_address != address->a4.address)
                    p = &address->a4.peer_address;
            } else {
                if (!IN6_IS_ADDR_UNSPECIFIED(&address->a6.peer_address)
                    && !IN6_ARE_ADDR_EQUAL(&address->a6.peer_address, &address->a6.address))
                    p = &address->a6.peer_address;
            }
            if (p) {
                g_variant_builder_add(&addr_builder,
                                      "{sv}",
                                      "peer",
                                      g_variant_new_string(nm_inet_ntop(addr_family, p, addr_str)));
            }

            if (IS_IPv4) {
                if (*address->a4.label) {
                    g_variant_builder_add(&addr_builder,
                                          "{sv}",
                                          NM_IP_ADDRESS_ATTRIBUTE_LABEL,
                                          g_variant_new_string(address->a4.label));
                }
            }

            g_variant_builder_add(&builder_data, "a{sv}", &addr_builder);
        }

        if (out_addresses) {
            if (IS_IPv4) {
                const guint32 dbus_addr[3] = {
                    address->a4.address,
                    address->a4.plen,
                    (i == 0 && best_default_route)
                        ? NMP_OBJECT_CAST_IP4_ROUTE(best_default_route)->gateway
                        : (guint32) 0,
                };

                g_variant_builder_add(&builder_legacy, "@au", nm_g_variant_new_au(dbus_addr, 3));
            } else {
                g_variant_builder_add(
                    &builder_legacy,
                    "(@ayu@ay)",
                    nm_g_variant_new_ay_in6addr(&address->a6.address),
                    address->a6.plen,
                    nm_g_variant_new_ay_in6addr(
                        (i == 0 && best_default_route)
                            ? &NMP_OBJECT_CAST_IP6_ROUTE(best_default_route)->gateway
                            : &in6addr_any));
            }
        }

        i++;
    }

out:
    NM_SET_OUT(out_address_data, g_variant_builder_end(&builder_data));
    NM_SET_OUT(out_addresses, g_variant_builder_end(&builder_legacy));
}

void
nm_utils_ip_routes_to_dbus(int                          addr_family,
                           const NMDedupMultiHeadEntry *head_entry,
                           GVariant                   **out_route_data,
                           GVariant                   **out_routes)
{
    const int        IS_IPv4 = NM_IS_IPv4(addr_family);
    NMDedupMultiIter iter;
    const NMPObject *obj;
    GVariantBuilder  builder_data;
    GVariantBuilder  builder_legacy;
    char             addr_str[NM_INET_ADDRSTRLEN];
    const gsize      MAX_ROUTES = 100;
    gsize            i;

    nm_assert_addr_family(addr_family);

    if (out_route_data)
        g_variant_builder_init(&builder_data, G_VARIANT_TYPE("aa{sv}"));
    if (out_routes) {
        if (IS_IPv4)
            g_variant_builder_init(&builder_legacy, G_VARIANT_TYPE("aau"));
        else
            g_variant_builder_init(&builder_legacy, G_VARIANT_TYPE("a(ayuayu)"));
    }

    i = 0;
    nm_dedup_multi_iter_init(&iter, head_entry);
    while (nm_platform_dedup_multi_iter_next_obj(&iter, &obj, NMP_OBJECT_TYPE_IP_ROUTE(IS_IPv4))) {
        const NMPlatformIPXRoute *r = NMP_OBJECT_CAST_IPX_ROUTE(obj);
        struct in6_addr           n;

        nm_assert(r);
        nm_assert(r->rx.plen <= 8u * nm_utils_addr_family_to_size(addr_family));
        nm_assert(!IS_IPv4
                  || r->r4.network == nm_ip4_addr_clear_host_address(r->r4.network, r->r4.plen));
        nm_assert(IS_IPv4
                  || (memcmp(&r->r6.network,
                             nm_ip6_addr_clear_host_address(&n, &r->r6.network, r->r6.plen),
                             sizeof(n))
                      == 0));

        if (r->rx.type_coerced != nm_platform_route_type_coerce(RTN_UNICAST))
            continue;

        if (i >= MAX_ROUTES) {
            /* Limited. The rest is hidden. */
            break;
        }

        i++;

        if (out_route_data) {
            GVariantBuilder route_builder;
            gconstpointer   gateway;

            g_variant_builder_init(&route_builder, G_VARIANT_TYPE("a{sv}"));

            g_variant_builder_add(
                &route_builder,
                "{sv}",
                "dest",
                g_variant_new_string(nm_inet_ntop(addr_family, r->rx.network_ptr, addr_str)));

            g_variant_builder_add(&route_builder,
                                  "{sv}",
                                  "prefix",
                                  g_variant_new_uint32(r->rx.plen));

            gateway = nm_platform_ip_route_get_gateway(addr_family, &r->rx);
            if (!nm_ip_addr_is_null(addr_family, gateway)) {
                g_variant_builder_add(
                    &route_builder,
                    "{sv}",
                    "next-hop",
                    g_variant_new_string(nm_inet_ntop(addr_family, gateway, addr_str)));
            }

            g_variant_builder_add(&route_builder,
                                  "{sv}",
                                  "metric",
                                  g_variant_new_uint32(r->rx.metric));

            if (!nm_platform_route_table_is_main(r->rx.table_coerced)) {
                g_variant_builder_add(
                    &route_builder,
                    "{sv}",
                    "table",
                    g_variant_new_uint32(
                        nm_platform_route_table_uncoerce(r->rx.table_coerced, TRUE)));
            }

            g_variant_builder_add(&builder_data, "a{sv}", &route_builder);
        }

        if (out_routes) {
            /* legacy versions of nm_ip[46]_route_set_prefix() in libnm-util assert that the
             * plen is positive. Skip the default routes not to break older clients. */
            if (!nm_platform_route_table_is_main(r->rx.table_coerced)
                || NM_PLATFORM_IP_ROUTE_IS_DEFAULT(r))
                continue;

            if (IS_IPv4) {
                const guint32 dbus_route[4] = {
                    r->r4.network,
                    r->r4.plen,
                    r->r4.gateway,
                    r->r4.metric,
                };

                g_variant_builder_add(&builder_legacy, "@au", nm_g_variant_new_au(dbus_route, 4));
            } else {
                g_variant_builder_add(&builder_legacy,
                                      "(@ayu@ayu)",
                                      nm_g_variant_new_ay_in6addr(&r->r6.network),
                                      (guint32) r->r6.plen,
                                      nm_g_variant_new_ay_in6addr(&r->r6.gateway),
                                      (guint32) r->r6.metric);
            }
        }
    }

    NM_SET_OUT(out_route_data, g_variant_builder_end(&builder_data));
    NM_SET_OUT(out_routes, g_variant_builder_end(&builder_legacy));
}

/*****************************************************************************/

NMSetting *
nm_utils_platform_capture_ip_setting(NMPlatform *platform,
                                     int         addr_family,
                                     int         ifindex,
                                     gboolean    maybe_ipv6_disabled)
{
    const int                          IS_IPv4 = NM_IS_IPv4(addr_family);
    gs_unref_object NMSettingIPConfig *s_ip    = NULL;
    NMPLookup                          lookup;
    NMDedupMultiIter                   iter;
    const NMPObject                   *obj;
    const char                        *method = NULL;
    char                               sbuf[NM_INET_ADDRSTRLEN];
    const NMPlatformIPXRoute          *best_default_route = NULL;

    s_ip =
        NM_SETTING_IP_CONFIG(IS_IPv4 ? nm_setting_ip4_config_new() : nm_setting_ip6_config_new());

    if (ifindex <= 0 || !nm_platform_link_get(platform, ifindex)) {
        g_object_set(s_ip,
                     NM_SETTING_IP_CONFIG_METHOD,
                     IS_IPv4 ? NM_SETTING_IP4_CONFIG_METHOD_DISABLED
                             : NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
                     NULL);
        return NM_SETTING(g_steal_pointer(&s_ip));
    }

    nmp_lookup_init_object_by_ifindex(&lookup, NMP_OBJECT_TYPE_IP_ADDRESS(IS_IPv4), ifindex);
    nm_platform_iter_obj_for_each (&iter, platform, &lookup, &obj) {
        const NMPlatformIPXAddress           *address = NMP_OBJECT_CAST_IPX_ADDRESS(obj);
        nm_auto_unref_ip_address NMIPAddress *s_addr  = NULL;

        if (!IS_IPv4) {
            /* Ignore link-local address. */
            if (IN6_IS_ADDR_LINKLOCAL(address->ax.address_ptr)) {
                if (!method)
                    method = NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL;
                continue;
            }
        }

        /* Detect dynamic address */
        if (address->ax.lifetime != NM_PLATFORM_LIFETIME_PERMANENT) {
            method =
                IS_IPv4 ? NM_SETTING_IP4_CONFIG_METHOD_AUTO : NM_SETTING_IP6_CONFIG_METHOD_AUTO;
            continue;
        }

        /* Static address found. */
        if (IS_IPv4) {
            if (!method)
                method = NM_SETTING_IP4_CONFIG_METHOD_MANUAL;
        } else {
            if (NM_IN_STRSET(method, NULL, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL))
                method = NM_SETTING_IP6_CONFIG_METHOD_MANUAL;
        }

        s_addr =
            nm_ip_address_new_binary(addr_family, address->ax.address_ptr, address->ax.plen, NULL);

        if (IS_IPv4) {
            if (address->a4.label[0]) {
                nm_ip_address_set_attribute(s_addr,
                                            NM_IP_ADDRESS_ATTRIBUTE_LABEL,
                                            g_variant_new_string(address->a4.label));
            }
        }

        nm_setting_ip_config_add_address(s_ip, s_addr);
    }

    if (!method) {
        /* Use 'disabled/ignore' if the method wasn't previously set */
        if (IS_IPv4)
            method = NM_SETTING_IP4_CONFIG_METHOD_DISABLED;
        else
            method = maybe_ipv6_disabled ? NM_SETTING_IP6_CONFIG_METHOD_DISABLED
                                         : NM_SETTING_IP6_CONFIG_METHOD_IGNORE;
    }

    /* The IPv6 method "ignore" and "disabled" are not supported for loopback */
    if (ifindex == 1
        && NM_IN_STRSET(method,
                        NM_SETTING_IP6_CONFIG_METHOD_DISABLED,
                        NM_SETTING_IP6_CONFIG_METHOD_IGNORE))
        method = NM_SETTING_IP6_CONFIG_METHOD_AUTO;
    g_object_set(s_ip, NM_SETTING_IP_CONFIG_METHOD, method, NULL);

    nmp_lookup_init_object_by_ifindex(&lookup, NMP_OBJECT_TYPE_IP_ROUTE(IS_IPv4), ifindex);
    nm_platform_iter_obj_for_each (&iter, platform, &lookup, &obj) {
        const NMPlatformIPXRoute         *route   = NMP_OBJECT_CAST_IPX_ROUTE(obj);
        nm_auto_unref_ip_route NMIPRoute *s_route = NULL;

        if (!IS_IPv4) {
            /* Ignore link-local route. */
            if (IN6_IS_ADDR_LINKLOCAL(route->rx.network_ptr))
                continue;
        }

        if (NM_PLATFORM_IP_ROUTE_IS_DEFAULT(route)) {
            if (!best_default_route)
                best_default_route = route;
            continue;
        }

        if (route->rx.rt_source
            != nmp_utils_ip_config_source_round_trip_rtprot(NM_IP_CONFIG_SOURCE_USER)) {
            /* Ignore routes provided by external sources */
            continue;
        }

        s_route = nm_ip_route_new_binary(addr_family,
                                         route->rx.network_ptr,
                                         route->rx.plen,
                                         nm_platform_ip_route_get_gateway(addr_family, &route->rx),
                                         route->rx.metric,
                                         NULL);
        nm_setting_ip_config_add_route(s_ip, s_route);
    }

    if (best_default_route && nm_setting_ip_config_get_num_addresses(s_ip) > 0) {
        g_object_set(
            s_ip,
            NM_SETTING_IP_CONFIG_GATEWAY,
            nm_inet_ntop(addr_family,
                         nm_platform_ip_route_get_gateway(addr_family, &best_default_route->rx),
                         sbuf),
            NULL);
    }

    return NM_SETTING(g_steal_pointer(&s_ip));
}

/*****************************************************************************/

/* Singleton NMPlatform subclass instance and cached class object */
NM_DEFINE_SINGLETON_INSTANCE(NMPlatform);

NM_DEFINE_SINGLETON_REGISTER(NMPlatform);

/**
 * nm_platform_setup:
 * @instance: the #NMPlatform instance
 *
 * Failing to set up #NMPlatform singleton results in a fatal error,
 * as well as trying to initialize it multiple times without freeing
 * it.
 *
 * NetworkManager will typically use only one platform object during
 * its run. Test programs might want to switch platform implementations,
 * though.
 */
void
nm_platform_setup(NMPlatform *instance)
{
    g_return_if_fail(NM_IS_PLATFORM(instance));
    g_return_if_fail(!singleton_instance);

    singleton_instance = instance;

    nm_singleton_instance_register();

    nm_log_dbg(LOGD_CORE,
               "setup %s singleton (" NM_HASH_OBFUSCATE_PTR_FMT ")",
               "NMPlatform",
               NM_HASH_OBFUSCATE_PTR(instance));
}

/**
 * nm_platform_get:
 * @self: platform instance
 *
 * Retrieve #NMPlatform singleton. Use this whenever you want to connect to
 * #NMPlatform signals. It is an error to call it before nm_platform_setup().
 *
 * Returns: (transfer none): The #NMPlatform singleton reference.
 */
NMPlatform *
nm_platform_get(void)
{
    g_assert(singleton_instance);

    return singleton_instance;
}

/*****************************************************************************/

void
nm_linux_platform_setup(void)
{
    nm_platform_setup(nm_linux_platform_new(NULL, FALSE, FALSE, FALSE));
}

void
nm_linux_platform_setup_with_tc_cache(void)
{
    nm_platform_setup(nm_linux_platform_new(NULL, FALSE, FALSE, TRUE));
}

/*****************************************************************************/

NM_UTILS_FLAGS2STR_DEFINE(
    nm_settings_autoconnect_blocked_reason_to_string,
    NMSettingsAutoconnectBlockedReason,
    NM_UTILS_FLAGS2STR(NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NONE, "none"),
    NM_UTILS_FLAGS2STR(NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_USER_REQUEST, "user-request"),
    NM_UTILS_FLAGS2STR(NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_FAILED, "failed"),
    NM_UTILS_FLAGS2STR(NM_SETTINGS_AUTOCONNECT_BLOCKED_REASON_NO_SECRETS, "no-secrets"), );
