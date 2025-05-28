/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2004 - 2005 Colin Walters <walters@redhat.com>
 * Copyright (C) 2004 - 2017 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-dns-manager.h"

#include <fcntl.h>
#include <resolv.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/fs.h>

#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-glib-aux/nm-str-buf.h"

#include "NetworkManagerUtils.h"
#include "devices/nm-device.h"
#include "nm-config.h"
#include "nm-dbus-object.h"
#include "nm-dns-dnsmasq.h"
#include "nm-dns-dnsconfd.h"
#include "nm-dns-resolvconf.h"
#include "nm-dns-plugin.h"
#include "nm-dns-systemd-resolved.h"
#include "nm-ip-config.h"
#include "nm-l3-config-data.h"
#include "nm-manager.h"
#include "nm-utils.h"

#define HASH_LEN NM_UTILS_CHECKSUM_LENGTH_SHA1

#define UPDATE_PENDING_UNBLOCK_TIMEOUT_MSEC 5000

#ifndef RESOLVCONF_PATH
#define RESOLVCONF_PATH "/sbin/resolvconf"
#define HAS_RESOLVCONF  0
#else
#define HAS_RESOLVCONF 1
#endif

#ifndef NETCONFIG_PATH
#define NETCONFIG_PATH "/sbin/netconfig"
#define HAS_NETCONFIG  0
#else
#define HAS_NETCONFIG 1
#endif

enum {
    CONFIG_CHANGED,

    LAST_SIGNAL
};

NM_GOBJECT_PROPERTIES_DEFINE(NMDnsManager,
                             PROP_MODE,
                             PROP_RC_MANAGER,
                             PROP_CONFIGURATION,
                             PROP_UPDATE_PENDING, );

static guint signals[LAST_SIGNAL] = {0};

typedef struct {
    GHashTable *configs_dict;
    CList       configs_lst_head;

    CList     ip_data_lst_head;
    GVariant *config_variant;

    /* A DNS plugin should not be marked as pending indefinitely.
     * We are only blocked if "update_pending" is TRUE and we have
     * "update_pending_unblock" timer ticking. */
    GSource *update_pending_unblock;

    bool ip_data_lst_need_sort : 1;

    bool configs_lst_need_sort : 1;

    bool dns_touched : 1;
    bool is_stopped : 1;

    bool config_changed : 1;

    bool update_pending : 1;

    bool caching_was_successful : 1;

    bool custom_plugin_update_to_do : 1;
    bool sd_resolved_backup_plugin_update_to_do : 1;
    bool resolvconf_plugin_update_to_do : 1;

    char *hostdomain;
    guint updates_queue;

    NMDnsManagerResolvConfManager rc_manager;
    char                         *mode;
    NMDnsPlugin                  *custom_plugin;
    NMDnsPlugin                  *sd_resolved_backup_plugin;
    NMDnsPlugin                  *resolvconf_plugin;

    guint8 master_hash[HASH_LEN];

    gulong update_changed_signal_id_sd;
    gulong update_changed_signal_id;

    NMConfig *config;

    NMDnsConfigIPData *best_ip_config_4;
    NMDnsConfigIPData *best_ip_config_6;

    struct {
        guint64 ts;
        guint   num_restarts;
        guint   timer;
    } plugin_ratelimit;
} NMDnsManagerPrivate;

struct _NMDnsManager {
    NMDBusObject        parent;
    NMDnsManagerPrivate _priv;
};

struct _NMDnsManagerClass {
    NMDBusObjectClass parent;
};

G_DEFINE_TYPE(NMDnsManager, nm_dns_manager, NM_TYPE_DBUS_OBJECT)

#define NM_DNS_MANAGER_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMDnsManager, NM_IS_DNS_MANAGER)

NM_DEFINE_SINGLETON_GETTER(NMDnsManager, nm_dns_manager_get, NM_TYPE_DNS_MANAGER);

/*****************************************************************************/

#define _NMLOG_PREFIX_NAME "dns-mgr"
#define _NMLOG_DOMAIN      LOGD_DNS
#define _NMLOG(level, ...)                                                 \
    G_STMT_START                                                           \
    {                                                                      \
        const NMLogLevel __level = (level);                                \
                                                                           \
        if (nm_logging_enabled(__level, _NMLOG_DOMAIN)) {                  \
            _nm_unused const NMDnsManager *const __self = (self);          \
                                                                           \
            _nm_log(__level,                                               \
                    _NMLOG_DOMAIN,                                         \
                    0,                                                     \
                    NULL,                                                  \
                    NULL,                                                  \
                    "%s: " _NM_UTILS_MACRO_FIRST(__VA_ARGS__),             \
                    _NMLOG_PREFIX_NAME _NM_UTILS_MACRO_REST(__VA_ARGS__)); \
        }                                                                  \
    }                                                                      \
    G_STMT_END

/*****************************************************************************/

static gboolean
_update_pending_detect(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    return priv->custom_plugin ? nm_dns_plugin_get_update_pending(priv->custom_plugin) : FALSE;
}

static gboolean
_update_pending_unblock_cb(gpointer user_data)
{
    NMDnsManager        *self = user_data;
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    nm_assert(priv->update_pending);
    nm_assert(priv->update_pending_unblock);
    nm_assert(_update_pending_detect(self));

    nm_clear_g_source_inst(&priv->update_pending_unblock);

    _LOGW(
        "update-pending changed: DNS plugin did not become ready again. Assume something is wrong");

    _notify(self, PROP_UPDATE_PENDING);
    return G_SOURCE_CONTINUE;
}

static void
_update_pending_maybe_changed(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    gboolean             update_pending;

    update_pending = _update_pending_detect(self);
    if (priv->update_pending == update_pending)
        return;

    if (update_pending) {
        nm_assert(!priv->update_pending_unblock);
        priv->update_pending_unblock = nm_g_timeout_add_source(UPDATE_PENDING_UNBLOCK_TIMEOUT_MSEC,
                                                               _update_pending_unblock_cb,
                                                               self);
    } else
        nm_clear_g_source_inst(&priv->update_pending_unblock);

    priv->update_pending = update_pending;
    _LOGD("update-pending changed: %spending", update_pending ? "" : "not ");
    _notify(self, PROP_UPDATE_PENDING);
}

static void
_update_pending_changed_cb(NMDnsPlugin *plugin, gboolean update_pending, NMDnsManager *self)
{
    _update_pending_maybe_changed(self);
}

gboolean
nm_dns_manager_get_update_pending(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv;

    g_return_val_if_fail(NM_IS_DNS_MANAGER(self), FALSE);

    priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    nm_assert(priv->update_pending == _update_pending_detect(self));
    nm_assert(priv->update_pending || !priv->update_pending_unblock);

    /* update-pending can only be TRUE for a certain time (before we assume
     * something is really wrong with the plugin). That is, as long as
     * update_pending_unblock is ticking. */
    return !!priv->update_pending_unblock;
}

/*****************************************************************************/

static int
_dns_config_ip_data_get_dns_priority1(const NML3ConfigData *l3cd, int addr_family)
{
    int prio;

    if (!nm_l3_config_data_get_dns_priority(l3cd, addr_family, &prio))
        return 0;

    nm_assert(prio != 0);
    return prio;
}

static int
_dns_config_ip_data_get_dns_priority(const NMDnsConfigIPData *ip_data)
{
    return _dns_config_ip_data_get_dns_priority1(ip_data->l3cd, ip_data->addr_family);
}

static void
_ASSERT_dns_config_data(const NMDnsConfigData *data)
{
    nm_assert(data);
    nm_assert(NM_IS_DNS_MANAGER(data->self));
    nm_assert(data->ifindex > 0);
}

static void
_ASSERT_dns_config_ip_data(const NMDnsConfigIPData *ip_data)
{
    nm_assert(ip_data);
    _ASSERT_dns_config_data(ip_data->data);
    nm_assert(NM_IS_L3_CONFIG_DATA(ip_data->l3cd));
    nm_assert(c_list_contains(&ip_data->data->data_lst_head, &ip_data->data_lst));
    nm_assert(ip_data->data->ifindex == nm_l3_config_data_get_ifindex(ip_data->l3cd));
#if NM_MORE_ASSERTS > 5
    {
        gboolean has_default = FALSE;
        gsize    i;

        for (i = 0; ip_data->domains.search && ip_data->domains.search; i++) {
            const char *d = ip_data->domains.search[i];

            d = nm_utils_parse_dns_domain(d, NULL);
            nm_assert(d);
            if (d[0] == '\0')
                has_default = TRUE;
        }
        nm_assert(has_default == ip_data->domains.has_default_route_explicit);
        if (ip_data->domains.has_default_route_explicit)
            nm_assert(ip_data->domains.has_default_route_exclusive);
        if (ip_data->domains.has_default_route_exclusive)
            nm_assert(ip_data->domains.has_default_route);
    }
    nm_assert(_dns_config_ip_data_get_dns_priority(ip_data) != 0);
#endif
}

static NMDnsConfigIPData *
_dns_config_ip_data_new(NMDnsConfigData      *data,
                        int                   addr_family,
                        gconstpointer         source_tag,
                        const NML3ConfigData *l3cd,
                        NMDnsIPConfigType     ip_config_type)
{
    NMDnsConfigIPData *ip_data;

    _ASSERT_dns_config_data(data);
    nm_assert(NM_IS_L3_CONFIG_DATA(l3cd));
    nm_assert(ip_config_type != NM_DNS_IP_CONFIG_TYPE_REMOVED);

    ip_data  = g_slice_new(NMDnsConfigIPData);
    *ip_data = (NMDnsConfigIPData) {
        .data           = data,
        .source_tag     = source_tag,
        .l3cd           = nm_l3_config_data_ref_and_seal(l3cd),
        .ip_config_type = ip_config_type,
        .addr_family    = addr_family,
    };
    c_list_link_tail(&data->data_lst_head, &ip_data->data_lst);
    c_list_link_tail(&NM_DNS_MANAGER_GET_PRIVATE(data->self)->ip_data_lst_head,
                     &ip_data->ip_data_lst);

    /* We also need to set priv->ip_data_lst_need_sort, but the caller will do that! */

    _ASSERT_dns_config_ip_data(ip_data);
    return ip_data;
}

static void
_dns_config_ip_data_free(NMDnsConfigIPData *ip_data)
{
    _ASSERT_dns_config_ip_data(ip_data);

    c_list_unlink_stale(&ip_data->data_lst);
    c_list_unlink_stale(&ip_data->ip_data_lst);

    g_free(ip_data->domains.search);
    g_strfreev(ip_data->domains.reverse);

    nm_l3_config_data_unref(ip_data->l3cd);
    nm_g_slice_free(ip_data);
}

static void
_dns_config_data_free(NMDnsConfigData *data)
{
    _ASSERT_dns_config_data(data);

    nm_assert(c_list_is_empty(&data->data_lst_head));
    c_list_unlink_stale(&data->configs_lst);
    nm_g_slice_free(data);
}

static int
_mgr_get_ip_data_lst_cmp(const CList *a_lst, const CList *b_lst, const void *user_data)
{
    const NMDnsConfigIPData *a = c_list_entry(a_lst, NMDnsConfigIPData, ip_data_lst);
    const NMDnsConfigIPData *b = c_list_entry(b_lst, NMDnsConfigIPData, ip_data_lst);

    /* Configurations with lower priority value first */
    NM_CMP_DIRECT(_dns_config_ip_data_get_dns_priority(a), _dns_config_ip_data_get_dns_priority(b));

    /* Sort according to type (descendingly) */
    NM_CMP_FIELD(b, a, ip_config_type);

    return 0;
}

static CList *
_mgr_get_ip_data_lst_head(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    if (G_UNLIKELY(priv->ip_data_lst_need_sort)) {
        priv->ip_data_lst_need_sort = FALSE;
        c_list_sort(&priv->ip_data_lst_head, _mgr_get_ip_data_lst_cmp, NULL);
    }

    return &priv->ip_data_lst_head;
}

static int
_mgr_get_configs_lst_cmp(const CList *a_lst, const CList *b_lst, const void *user_data)
{
    const NMDnsConfigData *a = c_list_entry(a_lst, NMDnsConfigData, configs_lst);
    const NMDnsConfigData *b = c_list_entry(b_lst, NMDnsConfigData, configs_lst);

    NM_CMP_FIELD(b, a, ifindex);
    return nm_assert_unreachable_val(0);
}

_nm_unused static CList *
_mgr_get_configs_lst_head(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    if (G_UNLIKELY(priv->configs_lst_need_sort)) {
        priv->configs_lst_need_sort = FALSE;
        c_list_sort(&priv->configs_lst_head, _mgr_get_configs_lst_cmp, NULL);
    }

    return &priv->configs_lst_head;
}

/*****************************************************************************/

NMDnsPlugin *
nm_dns_manager_get_systemd_resolved(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv;
    NMDnsPlugin         *resolved_plugin;

    g_return_val_if_fail(NM_IS_DNS_MANAGER(self), FALSE);

    priv            = NM_DNS_MANAGER_GET_PRIVATE(self);
    resolved_plugin = priv->sd_resolved_backup_plugin;

    if (resolved_plugin
        && nm_dns_systemd_resolved_is_running(NM_DNS_SYSTEMD_RESOLVED(resolved_plugin))) {
        nm_assert(NM_IS_DNS_SYSTEMD_RESOLVED(resolved_plugin));
        return resolved_plugin;
    }

    return NULL;
}

/*****************************************************************************/

static void
compute_master_hash(NMDnsManager *self, guint8 buffer[static HASH_LEN])
{
    NMDnsManagerPrivate             *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    nm_auto_free_checksum GChecksum *sum  = NULL;

    sum = g_checksum_new(G_CHECKSUM_SHA1);
    nm_assert(HASH_LEN == g_checksum_type_get_length(G_CHECKSUM_SHA1));

    /* presence of these two is not certain but resolvconf is always used */
    if (priv->custom_plugin)
        g_checksum_update(sum, nm_dns_plugin_get_hash(priv->custom_plugin), HASH_LEN);
    if (priv->sd_resolved_backup_plugin)
        g_checksum_update(sum, nm_dns_plugin_get_hash(priv->sd_resolved_backup_plugin), HASH_LEN);

    g_checksum_update(sum, nm_dns_plugin_get_hash(priv->resolvconf_plugin), HASH_LEN);

    nm_utils_checksum_get_digest_len(sum, buffer, HASH_LEN);
}

static gboolean
compute_hash(NMDnsManager *self, NMDnsPlugin *plugin, GChecksum *global_sum)
{
    nm_auto_free_checksum GChecksum *sum = NULL;
    NMDnsConfigIPData               *ip_data;
    guint8 new[HASH_LEN];

    sum = global_sum ? g_checksum_copy(global_sum) : g_checksum_new(G_CHECKSUM_SHA1);

    nm_assert(HASH_LEN == g_checksum_type_get_length(G_CHECKSUM_SHA1));

    c_list_for_each_entry (ip_data, _mgr_get_ip_data_lst_head(self), ip_data_lst) {
        nm_dns_plugin_checksum(plugin,
                               ip_data->l3cd,
                               sum,
                               ip_data->addr_family,
                               ip_data->ip_config_type);
    }

    nm_utils_checksum_get_digest_len(sum, new, HASH_LEN);

    if (memcmp(new, nm_dns_plugin_get_hash(plugin), sizeof(new)) != 0) {
        nm_dns_plugin_set_hash(plugin, new);
        return TRUE;
    }
    return FALSE;
}

static gboolean
update_plugin_hashes(NMDnsManager *self, gboolean set_update_flags)
{
    NMDnsManagerPrivate             *priv       = NM_DNS_MANAGER_GET_PRIVATE(self);
    nm_auto_free_checksum GChecksum *global_sum = NULL;
    const NMGlobalDnsConfig         *global =
        nm_config_data_get_global_dns_config(nm_config_get_data(priv->config));

    if (global) {
        global_sum = g_checksum_new(G_CHECKSUM_SHA1);
        nm_global_dns_config_update_checksum(global, global_sum);
    }

    /* presence of these two is not certain but resolvconf is always used */
    if (priv->custom_plugin && compute_hash(self, priv->custom_plugin, global_sum))
        priv->custom_plugin_update_to_do = priv->custom_plugin_update_to_do || set_update_flags;

    if (priv->sd_resolved_backup_plugin
        && compute_hash(self, priv->sd_resolved_backup_plugin, global_sum))
        priv->sd_resolved_backup_plugin_update_to_do =
            priv->sd_resolved_backup_plugin_update_to_do || set_update_flags;

    if (compute_hash(self, priv->resolvconf_plugin, global_sum)) {
        priv->resolvconf_plugin_update_to_do =
            priv->resolvconf_plugin_update_to_do || set_update_flags;
        return TRUE;
    }

    return priv->custom_plugin_update_to_do || priv->sd_resolved_backup_plugin_update_to_do;
}

/*****************************************************************************/

static char **
get_ip_rdns_domains(int addr_family, const NML3ConfigData *l3cd)
{
    const int        IS_IPv4 = NM_IS_IPv4(addr_family);
    char           **strv;
    GPtrArray       *domains;
    NMDedupMultiIter ipconf_iter;
    const NMPObject *obj;

    domains = g_ptr_array_sized_new(5);

    nm_l3_config_data_iter_obj_for_each (&ipconf_iter,
                                         l3cd,
                                         &obj,
                                         NMP_OBJECT_TYPE_IP_ADDRESS(IS_IPv4)) {
        nm_utils_get_reverse_dns_domains_ip(addr_family,
                                            NMP_OBJECT_CAST_IP_ADDRESS(obj)->address_ptr,
                                            NMP_OBJECT_CAST_IP_ADDRESS(obj)->plen,
                                            domains);
    }

    nm_l3_config_data_iter_obj_for_each (&ipconf_iter,
                                         l3cd,
                                         &obj,
                                         NMP_OBJECT_TYPE_IP_ROUTE(IS_IPv4)) {
        const NMPlatformIPRoute *route = NMP_OBJECT_CAST_IP_ROUTE(obj);

        if (!NM_PLATFORM_IP_ROUTE_IS_DEFAULT(route)) {
            nm_utils_get_reverse_dns_domains_ip(addr_family,
                                                route->network_ptr,
                                                route->plen,
                                                domains);
        }
    }

    /* Terminating NULL so we can use g_strfreev() to free it */
    g_ptr_array_add(domains, NULL);

    /* Free the array and return NULL if the only element was the ending NULL */
    strv = (char **) g_ptr_array_free(domains, (domains->len == 1));

    return nm_strv_cleanup(strv, FALSE, FALSE, TRUE);
}

static gboolean
_domain_track_get_priority(GHashTable *ht, const char *domain, int *out_priority)
{
    gpointer ptr;

    if (!ht || !g_hash_table_lookup_extended(ht, domain, NULL, &ptr)) {
        *out_priority = 0;
        return FALSE;
    }
    *out_priority = GPOINTER_TO_INT(ptr);
    return TRUE;
}

/* Check if the domain is shadowed by a parent domain with more negative priority */
static gboolean
_domain_track_is_shadowed(GHashTable  *ht,
                          const char  *domain,
                          int          priority,
                          const char **out_parent,
                          int         *out_parent_priority)
{
    char *parent;
    int   parent_priority;

    if (!ht)
        return FALSE;

    nm_assert(!g_hash_table_contains(ht, domain));

    if (_domain_track_get_priority(ht, "", &parent_priority)) {
        nm_assert(parent_priority <= priority);
        if (parent_priority < 0 && parent_priority < priority) {
            *out_parent          = "";
            *out_parent_priority = parent_priority;
            return TRUE;
        }
    }

    parent = strchr(domain, '.');
    while (parent && parent[1]) {
        parent++;
        if (_domain_track_get_priority(ht, parent, &parent_priority)) {
            nm_assert(parent_priority <= priority);
            if (parent_priority < 0 && parent_priority < priority) {
                *out_parent          = parent;
                *out_parent_priority = parent_priority;
                return TRUE;
            }
        }
        parent = strchr(parent, '.');
    }

    return FALSE;
}

static void
_mgr_configs_data_construct(NMDnsManager *self)
{
    NMDnsConfigIPData             *ip_data;
    gs_unref_hashtable GHashTable *ht               = NULL;
    gs_unref_hashtable GHashTable *wildcard_entries = NULL;
    CList                         *head;
    int                            prev_priority = G_MININT;

    head = _mgr_get_ip_data_lst_head(self);

#if NM_MORE_ASSERTS
    /* we call _mgr_configs_data_clear() at the end of update. We
     * don't expect any domain settings here. */
    c_list_for_each_entry (ip_data, head, ip_data_lst) {
        nm_assert(!ip_data->domains.search);
        nm_assert(!ip_data->domains.reverse);
        nm_assert(!ip_data->domains.has_default_route_explicit);
        nm_assert(!ip_data->domains.has_default_route_exclusive);
        nm_assert(!ip_data->domains.has_default_route);
    }
#endif

    c_list_for_each_entry (ip_data, head, ip_data_lst) {
        gboolean add_wildcard = FALSE;
        guint    num;

        nm_l3_config_data_get_nameservers(ip_data->l3cd, ip_data->addr_family, &num);
        if (num == 0)
            continue;
        if (nm_l3_config_data_get_best_default_route(ip_data->l3cd, ip_data->addr_family)) {
            /* FIXME(l3cfg): the best-default route of a l3cd is not significant! */
            add_wildcard = TRUE;
        } else {
            /* If a VPN has never-default=no but doesn't get a default
             * route (this can happen for example when the server
             * pushes routes with openconnect), and there are no
             * search or routing domains, then the name servers pushed
             * by the server would be unused. It is preferable in this
             * case to use the VPN DNS server for all queries. */
            if (ip_data->ip_config_type == NM_DNS_IP_CONFIG_TYPE_VPN
                && nm_l3_config_data_get_never_default(ip_data->l3cd, ip_data->addr_family)
                       == NM_TERNARY_FALSE
                && !nm_l3_config_data_get_searches(ip_data->l3cd, ip_data->addr_family, &num)
                && !nm_l3_config_data_get_domains(ip_data->l3cd, ip_data->addr_family, &num))
                add_wildcard = TRUE;
        }

        if (add_wildcard) {
            if (!wildcard_entries)
                wildcard_entries = g_hash_table_new(nm_direct_hash, NULL);
            g_hash_table_add(wildcard_entries, ip_data);
        }
    }

    c_list_for_each_entry (ip_data, head, ip_data_lst) {
        int                priority;
        const char       **domains;
        const char *const *strv_searches;
        const char *const *strv_domains;
        guint              n_searches;
        guint              n_domains;
        guint              num_dom1;
        guint              num_dom2;
        guint              n_domains_allocated;
        guint              i;
        gboolean           has_default_route_maybe    = FALSE;
        gboolean           has_default_route_explicit = FALSE;
        gboolean           has_default_route_auto     = FALSE;
        guint              num;

        nm_l3_config_data_get_nameservers(ip_data->l3cd, ip_data->addr_family, &num);
        if (num == 0)
            continue;

        strv_searches =
            nm_l3_config_data_get_searches(ip_data->l3cd, ip_data->addr_family, &n_searches);
        strv_domains =
            nm_l3_config_data_get_domains(ip_data->l3cd, ip_data->addr_family, &n_domains);

        priority = _dns_config_ip_data_get_dns_priority(ip_data);

        nm_assert(prev_priority <= priority);
        prev_priority = priority;

        /* Add wildcard lookup domain to connections with the default route.
         * If there is no default route, add the wildcard domain to all non-VPN
         * connections */
        if (wildcard_entries) {
            /* FIXME: this heuristic of which device has a default route does
             * not work with policy routing (as used by default with WireGuard).
             * We should have a more stable mechanism where an NMIPConfig indicates
             * whether it is suitable for certain operations (like having an automatically
             * added "~" domain). */
            if (g_hash_table_contains(wildcard_entries, ip_data))
                has_default_route_maybe = TRUE;
        } else {
            if (ip_data->ip_config_type != NM_DNS_IP_CONFIG_TYPE_VPN)
                has_default_route_maybe = TRUE;
        }

        n_domains_allocated = (n_searches > 0 ? n_searches : n_domains) + 1u;
        domains             = g_new(const char *, n_domains_allocated);

        num_dom1 = 0;

        /* searches are preferred over domains */
        if (n_searches > 0) {
            for (i = 0; i < n_searches; i++)
                domains[num_dom1++] = strv_searches[i];
        } else {
            for (i = 0; i < n_domains; i++)
                domains[num_dom1++] = strv_domains[i];
        }

        nm_assert(num_dom1 < n_domains_allocated);

        num_dom2 = 0;
        for (i = 0; TRUE; i++) {
            const char *domain_full;
            const char *domain_clean;
            const char *parent;
            int         old_priority;
            int         parent_priority;
            gboolean    check_default_route;

            if (i < num_dom1) {
                check_default_route = FALSE;
                domain_full         = domains[i];
                domain_clean        = nm_utils_parse_dns_domain(domains[i], NULL);
            } else if (i == num_dom1) {
                if (!has_default_route_maybe)
                    continue;
                if (has_default_route_explicit)
                    continue;
                check_default_route = TRUE;
                domain_full         = "~";
                domain_clean        = "";
            } else
                break;

            /* Remove domains with lower priority */
            if (_domain_track_get_priority(ht, domain_clean, &old_priority)) {
                nm_assert(old_priority <= priority);
                if (old_priority < priority) {
                    _LOGT("plugin: drop domain %s%s%s (i=%d, p=%d) because it already exists "
                          "with p=%d",
                          NM_PRINT_FMT_QUOTED(!check_default_route,
                                              "'",
                                              domain_full,
                                              "'",
                                              "<auto-default>"),
                          ip_data->data->ifindex,
                          priority,
                          old_priority);
                    continue;
                }
            } else if (_domain_track_is_shadowed(ht,
                                                 domain_clean,
                                                 priority,
                                                 &parent,
                                                 &parent_priority)) {
                _LOGT("plugin: drop domain %s%s%s (i=%d, p=%d) shadowed by '%s' (p=%d)",
                      NM_PRINT_FMT_QUOTED(!check_default_route,
                                          "'",
                                          domain_full,
                                          "'",
                                          "<auto-default>"),
                      ip_data->data->ifindex,
                      priority,
                      parent,
                      parent_priority);
                continue;
            }

            _LOGT(
                "plugin: add domain %s%s%s (i=%d, p=%d)",
                NM_PRINT_FMT_QUOTED(!check_default_route, "'", domain_full, "'", "<auto-default>"),
                ip_data->data->ifindex,
                priority);

            if (!ht)
                ht = g_hash_table_new(nm_str_hash, g_str_equal);
            g_hash_table_insert(ht, (gpointer) domain_clean, GINT_TO_POINTER(priority));

            if (check_default_route)
                has_default_route_auto = TRUE;
            else {
                nm_assert(num_dom2 <= num_dom1);
                nm_assert(num_dom2 < n_domains_allocated);
                domains[num_dom2++] = domain_full;
                if (domain_clean[0] == '\0')
                    has_default_route_explicit = TRUE;
            }
        }
        nm_assert(num_dom2 < n_domains_allocated);
        domains[num_dom2] = NULL;

        nm_assert(!ip_data->domains.search);
        nm_assert(!ip_data->domains.reverse);
        ip_data->domains.search  = domains;
        ip_data->domains.reverse = get_ip_rdns_domains(ip_data->addr_family, ip_data->l3cd);
        ip_data->domains.has_default_route_explicit = has_default_route_explicit;
        ip_data->domains.has_default_route_exclusive =
            has_default_route_explicit || (priority < 0 && has_default_route_auto);
        ip_data->domains.has_default_route =
            ip_data->domains.has_default_route_exclusive || has_default_route_auto;

        {
            gs_free char *str1 = NULL;
            gs_free char *str2 = NULL;

            _LOGT("plugin: settings: ifindex=%d, priority=%d, default-route=%d%s, search=%s, "
                  "reverse=%s",
                  ip_data->data->ifindex,
                  priority,
                  ip_data->domains.has_default_route,
                  ip_data->domains.has_default_route_explicit
                      ? " (explicit)"
                      : (ip_data->domains.has_default_route_exclusive ? " (exclusive)" : ""),
                  (str1 = g_strjoinv(",", (char **) ip_data->domains.search)),
                  (ip_data->domains.reverse ? (str2 = g_strjoinv(",", ip_data->domains.reverse))
                                            : ""));
        }
    }
}

static void
_mgr_configs_data_clear(NMDnsManager *self)
{
    NMDnsConfigIPData *ip_data;
    CList             *head;

    head = _mgr_get_ip_data_lst_head(self);
    c_list_for_each_entry (ip_data, head, ip_data_lst) {
        nm_clear_g_free(&ip_data->domains.search);
        nm_clear_pointer(&ip_data->domains.reverse, g_strfreev);
        ip_data->domains.has_default_route_explicit  = FALSE;
        ip_data->domains.has_default_route_exclusive = FALSE;
        ip_data->domains.has_default_route           = FALSE;
    }
}

/*****************************************************************************/

static void
update_custom_plugin(NMDnsManager    *self,
                     gboolean         no_caching,
                     NMDnsUpdateData *update_data,
                     GError         **local_error)
{
    NMDnsManagerPrivate *priv        = NM_DNS_MANAGER_GET_PRIVATE(self);
    const char          *plugin_name = nm_dns_plugin_get_name(priv->custom_plugin);

    priv->custom_plugin_update_to_do = FALSE;
    if (nm_dns_plugin_is_caching(priv->custom_plugin) && !no_caching) {
        _LOGD("update-dns: updating plugin %s", plugin_name);
        if (!nm_dns_plugin_update(priv->custom_plugin, update_data, local_error)) {
            _LOGW("update-dns: plugin %s update failed: %s", plugin_name, (*local_error)->message);
        } else {
            update_data->caching_successful = nm_dns_plugin_is_caching(priv->custom_plugin);
            priv->caching_was_successful    = update_data->caching_successful;
        }
    } else {
        _LOGD("update-dns: plugin %s ignored (caching disabled)", plugin_name);
    }
}

static gboolean
update_dns(NMDnsManager *self, gboolean no_caching, gboolean force_emit, GError **error)
{
    NMDnsManagerPrivate  *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    NMConfigData         *data;
    NMGlobalDnsConfig    *global_config;
    gs_free_error GError *local_error = NULL;
    NMDnsUpdateData       update_data;

    nm_assert(!error || !*error);

    priv->config_changed = FALSE;

    if (priv->is_stopped) {
        _LOGD("update-dns: not updating resolv.conf (is stopped)");
        return TRUE;
    }

    nm_clear_g_source(&priv->plugin_ratelimit.timer);

    data          = nm_config_get_data(priv->config);
    global_config = nm_config_data_get_global_dns_config(data);

    _mgr_configs_data_construct(self);

    update_data = (NMDnsUpdateData) {
        .ip_data_lst_head       = _mgr_get_ip_data_lst_head(self),
        .caching_successful     = FALSE,
        .resolved_used          = FALSE,
        .rc_manager             = priv->rc_manager,
        .hostdomain             = priv->hostdomain,
        .global_config          = global_config,
        .resolver_depends_on_nm = FALSE,
    };

    if (priv->custom_plugin) {
        if (NM_IS_DNS_SYSTEMD_RESOLVED(priv->custom_plugin)) {
            update_data.resolved_used = TRUE;
        } else if (NM_IS_DNS_DNSMASQ(priv->custom_plugin)) {
            update_data.resolver_depends_on_nm = TRUE;
        }

        if (priv->custom_plugin_update_to_do) {
            update_custom_plugin(self, no_caching, &update_data, &local_error);
        } else if (nm_dns_plugin_is_caching(priv->custom_plugin) && !no_caching) {
            /* If caching was succesfull before and we have no update, we have to
             * consider it still being successful */
            update_data.caching_successful = priv->caching_was_successful;
        }
    }

    if (priv->sd_resolved_backup_plugin_update_to_do && priv->sd_resolved_backup_plugin) {
        nm_dns_plugin_update(priv->sd_resolved_backup_plugin, &update_data, NULL);
        priv->sd_resolved_backup_plugin_update_to_do = FALSE;
    }

    if (priv->resolvconf_plugin_update_to_do) {
        priv->resolvconf_plugin_update_to_do = FALSE;
        if (!nm_dns_plugin_update(priv->resolvconf_plugin, &update_data, &local_error)) {
            _LOGW("update-dns: plugin resolvconf update failed: %s", local_error->message);
        }
    }

    /* Clear the generated search list as it points to
     * strings owned by IP configurations and we can't
     * guarantee they stay alive. */
    _mgr_configs_data_clear(self);

    /* signal that DNS resolution configs were changed */
    if (update_data.caching_successful || force_emit)
        g_signal_emit(self, signals[CONFIG_CHANGED], 0);

    nm_clear_pointer(&priv->config_variant, g_variant_unref);
    _notify(self, PROP_CONFIGURATION);

    if (local_error) {
        g_propagate_error(error, g_steal_pointer(&local_error));
        return FALSE;
    }

    nm_assert(!local_error);
    return TRUE;
}

gboolean
nm_dns_manager_is_unmanaged(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    return NM_IN_SET(priv->rc_manager,
                     NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED,
                     NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE);
}

/*****************************************************************************/

gboolean
nm_dns_manager_set_ip_config(NMDnsManager         *self,
                             int                   addr_family,
                             gconstpointer         source_tag,
                             const NML3ConfigData *l3cd,
                             NMDnsIPConfigType     ip_config_type,
                             gboolean              replace_all)
{
    NMDnsManagerPrivate *priv;
    NMDnsConfigData     *data;
    int                  ifindex;
    gboolean             changed = FALSE;
    NMDnsConfigIPData   *ip_data = NULL;
    int                  dns_priority;
    gboolean             any_removed = FALSE;
    NMDnsConfigIPData  **p_best;

    g_return_val_if_fail(NM_IS_DNS_MANAGER(self), FALSE);
    g_return_val_if_fail(!l3cd || NM_IS_L3_CONFIG_DATA(l3cd), FALSE);
    g_return_val_if_fail(source_tag, FALSE);

    if (addr_family == AF_UNSPEC) {
        /* Setting AF_UNSPEC is a shortcut for calling this function twice for AF_INET and
         * AF_INET6. */
        if (nm_dns_manager_set_ip_config(self,
                                         AF_INET,
                                         source_tag,
                                         l3cd,
                                         ip_config_type,
                                         replace_all))
            changed = TRUE;
        if (nm_dns_manager_set_ip_config(self,
                                         AF_INET6,
                                         source_tag,
                                         l3cd,
                                         ip_config_type,
                                         replace_all))
            changed = TRUE;
        return changed;
    }

    nm_assert_addr_family(addr_family);

    priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    data = NULL;
    if (l3cd) {
        ifindex = nm_l3_config_data_get_ifindex(l3cd);
        nm_assert(ifindex > 0);
        data = g_hash_table_lookup(priv->configs_dict, &ifindex);
    }

    if (data) {
        NMDnsConfigIPData *ip_data_iter;
        NMDnsConfigIPData *ip_data_safe;

        c_list_for_each_entry_safe (ip_data_iter, ip_data_safe, &data->data_lst_head, data_lst) {
            _ASSERT_dns_config_ip_data(ip_data_iter);

            if (ip_data_iter->source_tag != source_tag)
                continue;
            if (ip_data_iter->addr_family != addr_family)
                continue;

            if (ip_config_type != NM_DNS_IP_CONFIG_TYPE_REMOVED && ip_data_iter->l3cd == l3cd) {
                nm_assert(!ip_data);
                ip_data = ip_data_iter;
                continue;
            }

            if (!replace_all && l3cd && ip_data_iter->l3cd != l3cd)
                continue;

            if (!l3cd || ip_config_type == NM_DNS_IP_CONFIG_TYPE_REMOVED
                || nm_l3_config_data_cmp_full(l3cd,
                                              ip_data_iter->l3cd,
                                              NM_L3_CONFIG_CMP_FLAGS_DNS
                                                  | NM_L3_CONFIG_CMP_FLAGS_ROUTES_ID)
                       != 0) {
                changed = TRUE;
            }

            any_removed = TRUE;

            if (priv->best_ip_config_4 == ip_data_iter)
                priv->best_ip_config_4 = NULL;
            if (priv->best_ip_config_6 == ip_data_iter)
                priv->best_ip_config_6 = NULL;

            _dns_config_ip_data_free(ip_data_iter);
        }
    }

    if (ip_config_type == NM_DNS_IP_CONFIG_TYPE_REMOVED)
        goto done;

    if (!l3cd)
        goto done;

    if (ip_data && ip_data->ip_config_type == ip_config_type) {
        /* nothing to do. */
        goto done;
    }

    dns_priority = _dns_config_ip_data_get_dns_priority1(l3cd, addr_family);
    if (dns_priority == 0) {
        /* no DNS priority for this address family. Skip it! */
        goto done;
    }

    if (!data) {
        data  = g_slice_new(NMDnsConfigData);
        *data = (NMDnsConfigData) {
            .ifindex       = ifindex,
            .self          = self,
            .data_lst_head = C_LIST_INIT(data->data_lst_head),
        };
        _ASSERT_dns_config_data(data);
        g_hash_table_add(priv->configs_dict, data);
        c_list_link_tail(&priv->configs_lst_head, &data->configs_lst);
        priv->configs_lst_need_sort = TRUE;
    }

    if (!ip_data) {
        ip_data = _dns_config_ip_data_new(data, addr_family, source_tag, l3cd, ip_config_type);
        priv->ip_data_lst_need_sort = TRUE;
        if (!any_removed) {
            /* `any_removed` tracks whether we deleted any ip_data. If that happened,
             * we already compared the old and new l3cds and set `changed` accordingly.
             * Here we only need to set `changed` if we are adding a new ip_data without
             * removing the old one.
             */
            changed = TRUE;
        }
    } else {
        ip_data->ip_config_type = ip_config_type;
        changed                 = TRUE;
    }

    p_best = NM_IS_IPv4(addr_family) ? &priv->best_ip_config_4 : &priv->best_ip_config_6;
    if (ip_config_type == NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE) {
        /* Only one best-device per IP version is allowed */
        if (*p_best != ip_data) {
            if (*p_best)
                (*p_best)->ip_config_type = NM_DNS_IP_CONFIG_TYPE_DEFAULT;
            *p_best = ip_data;
        }
    } else {
        if (*p_best == ip_data)
            *p_best = NULL;
    }

    if (changed)
        priv->ip_data_lst_need_sort = TRUE;

    nm_assert(l3cd);
    nm_assert(ip_config_type != NM_DNS_IP_CONFIG_TYPE_REMOVED);
    nm_assert(ip_data->addr_family == addr_family);
    nm_assert(ip_data->source_tag == source_tag);
    nm_assert(ip_data->l3cd == l3cd);
    nm_assert(ip_data->ip_config_type == ip_config_type);

done:
    if (!changed)
        return FALSE;

    priv->config_changed = TRUE;

    if (data && c_list_is_empty(&data->data_lst_head))
        g_hash_table_remove(priv->configs_dict, data);

    if (!priv->updates_queue) {
        gs_free_error GError *error = NULL;
        update_plugin_hashes(self, TRUE);
        compute_master_hash(self, priv->master_hash);

        if (!update_dns(self, FALSE, FALSE, &error))
            _LOGW("could not commit DNS changes: %s", error->message);
    }

    return TRUE;
}

void
nm_dns_manager_set_hostname(NMDnsManager *self, const char *hostname, gboolean skip_update)
{
    NMDnsManagerPrivate *priv   = NM_DNS_MANAGER_GET_PRIVATE(self);
    const char          *domain = NULL;

    /* Certain hostnames we don't want to include in resolv.conf 'searches' */
    if (hostname && nm_utils_is_specific_hostname(hostname)
        && !NM_STR_HAS_SUFFIX(hostname, ".in-addr.arpa")
        && !nm_inet_is_valid(AF_UNSPEC, hostname)) {
        domain = strchr(hostname, '.');
        if (domain) {
            domain++;
            /* If the hostname is a FQDN ("dcbw.example.com"), then add
             * the domain part of it ("example.com") to the searches list,
             * to ensure that we can still resolve its non-FQ form
             * ("dcbw") too. (Also, if there are no other search domains
             * specified, this makes a good default.) However, if the
             * hostname is the top level of a domain (eg, "example.com"),
             * then use the hostname itself as the search (since the user
             * is unlikely to want "com" as a search domain).
             *
             * Because that logic only applies to public domains, the
             * "assume_any_tld_is_public" parameter is FALSE. For
             * example, it is likely that the user *does* want "local"
             * or "localdomain" as a search domain.
             */
            if (domain_is_valid(domain, TRUE, FALSE)) {
                /* pass */
            } else if (domain_is_valid(hostname, TRUE, FALSE)) {
                domain = hostname;
            }

            if (!nm_hostname_is_valid(domain, FALSE))
                domain = NULL;
        }
    }

    if (!nm_strdup_reset(&priv->hostdomain, domain))
        return;

    _LOGT("set host domain to %s%s%s", NM_PRINT_FMT_QUOTE_STRING(priv->hostdomain));

    if (skip_update)
        return;

    if (!priv->updates_queue) {
        gs_free_error GError *error = NULL;

        update_plugin_hashes(self, TRUE);
        compute_master_hash(self, priv->master_hash);

        if (!update_dns(self, FALSE, FALSE, &error))
            _LOGW("could not commit DNS changes: %s", error->message);
    }
}

void
nm_dns_manager_begin_updates(NMDnsManager *self, const char *func)
{
    NMDnsManagerPrivate *priv;

    g_return_if_fail(self != NULL);

    priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    priv->updates_queue++;

    _LOGD("(%s): queueing DNS updates (%d)", func, priv->updates_queue);
}

void
nm_dns_manager_end_updates(NMDnsManager *self, const char *func)
{
    NMDnsManagerPrivate  *priv;
    gs_free_error GError *error = NULL;

    g_return_if_fail(self != NULL);

    priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    g_return_if_fail(priv->updates_queue > 0);

    priv->config_changed = update_plugin_hashes(self, TRUE);

    compute_master_hash(self, priv->master_hash);

    _LOGD("(%s): DNS configuration %s", func, priv->config_changed ? "changed" : "did not change");

    priv->updates_queue--;
    if ((priv->updates_queue > 0) || !priv->config_changed) {
        _LOGD("(%s): no DNS changes to commit (%d)", func, priv->updates_queue);
        return;
    }

    /* Commit all the outstanding changes */
    _LOGD("(%s): committing DNS changes (%d)", func, priv->updates_queue);
    if (!update_dns(self, FALSE, FALSE, &error))
        _LOGW("could not commit DNS changes: %s", error->message);
}

void
nm_dns_manager_stop(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv;

    priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    if (priv->is_stopped)
        g_return_if_reached();

    _LOGT("stopping...");

    priv->is_stopped = TRUE;
}

/*****************************************************************************/

static gboolean
_clear_plugin(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    priv->plugin_ratelimit.ts = 0;
    nm_clear_g_source(&priv->plugin_ratelimit.timer);

    if (priv->custom_plugin) {
        nm_clear_g_signal_handler(priv->custom_plugin, &priv->update_changed_signal_id);
        nm_dns_plugin_stop(priv->custom_plugin);
        g_clear_object(&priv->custom_plugin);
        return TRUE;
    }

    return FALSE;
}

static gboolean
_clear_sd_resolved_plugin(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    if (priv->sd_resolved_backup_plugin) {
        nm_clear_g_signal_handler(priv->sd_resolved_backup_plugin,
                                  &priv->update_changed_signal_id_sd);
        nm_dns_plugin_stop(priv->sd_resolved_backup_plugin);
        g_clear_object(&priv->sd_resolved_backup_plugin);
        return TRUE;
    }

    return FALSE;
}

static gboolean
_clear_resolvconf_plugin(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    nm_dns_plugin_stop(priv->resolvconf_plugin);
    g_clear_object(&priv->resolvconf_plugin);

    return FALSE;
}

static NMDnsManagerResolvConfManager
_check_resconf_immutable(NMDnsManagerResolvConfManager rc_manager)
{
    struct stat st;
    int         fd, flags;
    bool        immutable = FALSE;

    switch (rc_manager) {
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN:
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE:
        nm_assert_not_reached();
        /* fall-through */
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED:
        return NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED;
    default:

        if (lstat(_PATH_RESCONF, &st) != 0)
            return rc_manager;

        if (S_ISLNK(st.st_mode)) {
            /* only regular files and directories can have extended file attributes. */
            switch (rc_manager) {
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK:
                /* we don't care whether the link-target is immutable.
                 * If the symlink points to another file, rc-manager=symlink anyway backs off.
                 * Otherwise, we would only check whether our internal resolv.conf is immutable. */
                return NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK;
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN:
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED:
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE:
                nm_assert_not_reached();
                /* fall-through */
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE:
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF:
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG:
            case NM_DNS_MANAGER_RESOLV_CONF_MAN_AUTO:
                break;
            }
        }

        fd = open(_PATH_RESCONF, O_RDONLY | O_CLOEXEC);
        if (fd != -1) {
            if (ioctl(fd, FS_IOC_GETFLAGS, &flags) != -1)
                immutable = NM_FLAGS_HAS(flags, FS_IMMUTABLE_FL);
            nm_close(fd);
        }
        return immutable ? NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE : rc_manager;
    }
}

static gboolean
_resolvconf_resolved_managed(void)
{
    static const char *const RESOLVED_PATHS[] = {
        "../run/systemd/resolve/stub-resolv.conf",
        "../run/systemd/resolve/resolv.conf",
        "../lib/systemd/resolv.conf",
        "../usr/lib/systemd/resolv.conf",
        "/run/systemd/resolve/stub-resolv.conf",
        "/run/systemd/resolve/resolv.conf",
        "/lib/systemd/resolv.conf",
        "/usr/lib/systemd/resolv.conf",
    };
    struct stat st, st_test;
    guint       i;

    if (lstat(_PATH_RESCONF, &st) != 0)
        return FALSE;

    if (S_ISLNK(st.st_mode)) {
        gs_free char      *full_path = NULL;
        nm_auto_free char *real_path = NULL;

        /* see if resolv.conf is a symlink with a target that is
         * exactly like one of the candidates.
         *
         * This check will work for symlinks, even if the target
         * does not exist and realpath() cannot resolve anything.
         *
         * We want to handle that, because systemd-resolved might not
         * have started yet. */
        full_path = g_file_read_link(_PATH_RESCONF, NULL);
        if (nm_strv_contains(RESOLVED_PATHS, G_N_ELEMENTS(RESOLVED_PATHS), full_path))
            return TRUE;

        /* see if resolv.conf is a symlink that resolves exactly one
         * of the candidate paths.
         *
         * This check will work for symlinks that can be resolved
         * to a realpath, but the actual file might not exist.
         *
         * We want to handle that, because systemd-resolved might not
         * have started yet. */
        real_path = realpath(_PATH_RESCONF, NULL);
        if (nm_strv_contains(RESOLVED_PATHS, G_N_ELEMENTS(RESOLVED_PATHS), real_path))
            return TRUE;

        /* fall-through and resolve the symlink, to check the file
         * it points to (below).
         *
         * This check is the most reliable, but it only works if
         * systemd-resolved already started and created the file. */
        if (stat(_PATH_RESCONF, &st) != 0)
            return FALSE;
    }

    /* see if resolv.conf resolves to one of the candidate
     * paths (or whether it is hard-linked). */
    for (i = 0; i < G_N_ELEMENTS(RESOLVED_PATHS); i++) {
        const char *p = RESOLVED_PATHS[i];

        if (p[0] == '/' && stat(p, &st_test) == 0 && st.st_dev == st_test.st_dev
            && st.st_ino == st_test.st_ino)
            return TRUE;
    }

    return FALSE;
}

static void
init_resolv_conf_mode(NMDnsManager *self, gboolean force_reload_plugin)
{
    NMDnsManagerPrivate          *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    NMDnsManagerResolvConfManager rc_manager;
    const char                   *mode;
    gboolean                      systemd_resolved;
    gboolean                      param_changed            = FALSE;
    gboolean                      plugin_changed           = FALSE;
    gboolean                      systemd_resolved_changed = FALSE;
    gboolean                      rc_manager_was_auto      = FALSE;

    mode             = nm_config_data_get_dns_mode(nm_config_get_data(priv->config));
    systemd_resolved = nm_config_data_get_systemd_resolved(nm_config_get_data(priv->config));

    if (nm_streq0(mode, "none"))
        rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED;
    else {
        const char *man;

        rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN;
        man        = nm_config_data_get_rc_manager(nm_config_get_data(priv->config));

again:
        if (!man) {
            /* nop */
        } else if (nm_streq(man, "auto"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_AUTO;
        else if (NM_IN_STRSET(man, "symlink", "none"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK;
        else if (nm_streq(man, "file"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE;
        else if (nm_streq(man, "resolvconf"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF;
        else if (nm_streq(man, "netconfig"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG;
        else if (nm_streq(man, "unmanaged"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED;

        if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN) {
            if (man) {
                _LOGW("init: unknown resolv.conf manager \"%s\", fallback to \"%s\"",
                      man,
                      "" NM_CONFIG_DEFAULT_MAIN_RC_MANAGER);
            }
            man        = "" NM_CONFIG_DEFAULT_MAIN_RC_MANAGER;
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_AUTO;
            goto again;
        }
    }

    rc_manager = _check_resconf_immutable(rc_manager);

    /* This means we are initializing for the first time and there is no
     * resolvconf plugin yet, thus initialize it */
    if (!priv->resolvconf_plugin) {
        priv->resolvconf_plugin = nm_dns_resolvconf_new();
    }

    if ((!mode && _resolvconf_resolved_managed()) || nm_streq0(mode, "systemd-resolved")) {
        if (force_reload_plugin || !NM_IS_DNS_SYSTEMD_RESOLVED(priv->custom_plugin)) {
            _clear_plugin(self);
            priv->custom_plugin = nm_dns_systemd_resolved_new();
            /* If we were using sd_resolved as backup plugin, the hash of config
             * is already in place and the new() call got us reference to that
             * already existing instance, but we did not require that update to
             * be succesfull, thus force sending the update again to find out
             * whether resolved resolution is truly in place */
            priv->custom_plugin_update_to_do = priv->sd_resolved_backup_plugin ? TRUE : FALSE;
            plugin_changed                   = TRUE;
        }
        mode             = "systemd-resolved";
        systemd_resolved = FALSE;
    } else if (nm_streq0(mode, "dnsmasq")) {
        if (force_reload_plugin || !NM_IS_DNS_DNSMASQ(priv->custom_plugin)) {
            _clear_plugin(self);
            priv->custom_plugin = nm_dns_dnsmasq_new();
            plugin_changed      = TRUE;
        }
    } else if (nm_streq0(mode, "dnsconfd")) {
        if (force_reload_plugin || !NM_IS_DNS_DNSCONFD(priv->custom_plugin)) {
            _clear_plugin(self);
            priv->custom_plugin = nm_dns_dnsconfd_new();
            plugin_changed      = TRUE;
        }
    } else {
        if (!NM_IN_STRSET(mode, "none", "default")) {
            if (mode) {
                if (nm_streq(mode, "unbound"))
                    _LOGW("init: ns mode 'unbound' was removed. Update your configuration");
                else
                    _LOGW("init: unknown dns mode '%s'", mode);
            }
            mode = "default";
        }
        if (_clear_plugin(self))
            plugin_changed = TRUE;
    }

    if (plugin_changed) {
        /* We need to force regeneration of resolvconf in case switch to local
         * caching service would be needed */
        priv->resolvconf_plugin_update_to_do = TRUE;
    }

    if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_AUTO) {
        rc_manager_was_auto = TRUE;
        if (nm_streq(mode, "systemd-resolved") || nm_streq(mode, "dnsconfd"))
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED;
        else if (HAS_RESOLVCONF && g_file_test(RESOLVCONF_PATH, G_FILE_TEST_IS_EXECUTABLE)) {
            /* We detect /sbin/resolvconf only at this stage. That means, if you install
             * or uninstall openresolv afterwards, you need to reload the DNS settings
             * (with SIGHUP or `systemctl reload NetworkManager.service`).
             *
             * We only accept resolvconf if NetworkManager was built with --with-resolvconf.
             * For example, on Fedora the systemd package provides a compat resolvconf
             * implementation for systemd-resolved. But using that never makes sense, because
             * there we either use full systemd-resolved mode or not. In no case does it
             * make sense to call that resolvconf implementation. */
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF;
        } else if (HAS_NETCONFIG && g_file_test(NETCONFIG_PATH, G_FILE_TEST_IS_EXECUTABLE)) {
            /* Like for resolvconf, we detect only once. We only autoenable this
             * option, if NetworkManager was built with netconfig explicitly enabled. */
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG;
        } else
            rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK;
    }

    /* The systemd-resolved plugin is special. We typically always want to keep
     * systemd-resolved up to date even if the configured plugin is different. */
    if (systemd_resolved) {
        if (!priv->sd_resolved_backup_plugin) {
            priv->sd_resolved_backup_plugin = nm_dns_systemd_resolved_new();
            systemd_resolved_changed        = TRUE;
        }
    } else if (_clear_sd_resolved_plugin(self))
        systemd_resolved_changed = TRUE;

    g_object_freeze_notify(G_OBJECT(self));

    if (!nm_streq0(priv->mode, mode)) {
        g_free(priv->mode);
        priv->mode    = g_strdup(mode);
        param_changed = TRUE;
        _notify(self, PROP_MODE);
    }

    if (priv->rc_manager != rc_manager) {
        priv->rc_manager = rc_manager;
        param_changed    = TRUE;
        _notify(self, PROP_RC_MANAGER);
    }

    if (param_changed || plugin_changed || systemd_resolved_changed) {
        _LOGI("init: dns=%s%s rc-manager=%s%s%s%s%s",
              mode,
              (systemd_resolved ? ",systemd-resolved" : ""),
              _rc_manager_to_string(rc_manager),
              rc_manager_was_auto ? " (auto)" : "",
              NM_PRINT_FMT_QUOTED(priv->custom_plugin,
                                  ", plugin=",
                                  nm_dns_plugin_get_name(priv->custom_plugin),
                                  "",
                                  ""));
    }

    if (plugin_changed && priv->custom_plugin && priv->update_changed_signal_id == 0) {
        priv->update_changed_signal_id = g_signal_connect(priv->custom_plugin,
                                                          NM_DNS_PLUGIN_UPDATE_PENDING_CHANGED,
                                                          G_CALLBACK(_update_pending_changed_cb),
                                                          self);
    }

    if (systemd_resolved_changed && priv->sd_resolved_backup_plugin
        && priv->update_changed_signal_id_sd == 0) {
        priv->update_changed_signal_id_sd = g_signal_connect(priv->sd_resolved_backup_plugin,
                                                             NM_DNS_PLUGIN_UPDATE_PENDING_CHANGED,
                                                             G_CALLBACK(_update_pending_changed_cb),
                                                             self);
    }

    if (!NM_IS_DNS_DNSMASQ(priv->custom_plugin))
        nm_dnsmasq_kill_external();

    _update_pending_maybe_changed(self);

    g_object_thaw_notify(G_OBJECT(self));
}

static void
config_changed_cb(NMConfig           *config,
                  NMConfigData       *config_data,
                  NMConfigChangeFlags changes,
                  NMConfigData       *old_data,
                  NMDnsManager       *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    if (NM_FLAGS_ANY(changes,
                     NM_CONFIG_CHANGE_DNS_MODE | NM_CONFIG_CHANGE_RC_MANAGER
                         | NM_CONFIG_CHANGE_CAUSE_SIGHUP | NM_CONFIG_CHANGE_CAUSE_DNS_FULL)) {
        /* reload the resolv-conf mode also on SIGHUP (when DNS_MODE didn't change).
         * The reason is, that the configuration also depends on whether resolv.conf
         * is immutable, thus, without the configuration changing, we always want to
         * re-configure the mode. */
        init_resolv_conf_mode(
            self,
            NM_FLAGS_ANY(changes, NM_CONFIG_CHANGE_CAUSE_SIGHUP | NM_CONFIG_CHANGE_CAUSE_DNS_FULL));
    }

    if (NM_FLAGS_ANY(changes,
                     NM_CONFIG_CHANGE_CAUSE_SIGHUP | NM_CONFIG_CHANGE_CAUSE_SIGUSR1
                         | NM_CONFIG_CHANGE_CAUSE_DNS_RC | NM_CONFIG_CHANGE_CAUSE_DNS_FULL
                         | NM_CONFIG_CHANGE_DNS_MODE | NM_CONFIG_CHANGE_RC_MANAGER
                         | NM_CONFIG_CHANGE_GLOBAL_DNS_CONFIG)) {
        gs_free_error GError *error = NULL;

        priv->config_changed = TRUE;

        if (NM_FLAGS_ANY(changes, NM_CONFIG_CHANGE_CAUSE_SIGUSR1)) {
            /* We need to force regeneration of plugin config as SIGUSR1 is meant to do that */
            priv->resolvconf_plugin_update_to_do = TRUE;
            if (priv->custom_plugin)
                priv->custom_plugin_update_to_do = TRUE;
            if (priv->sd_resolved_backup_plugin)
                priv->sd_resolved_backup_plugin_update_to_do = TRUE;
        }

        update_plugin_hashes(self, TRUE);
        compute_master_hash(self, priv->master_hash);

        if (!update_dns(self, FALSE, TRUE, &error))
            _LOGW("could not commit DNS changes: %s", error->message);
    }
}

static void
_get_global_config_variant(GVariantBuilder *builder, NMGlobalDnsConfig *global)
{
    NMGlobalDnsDomain *domain;
    guint              i, num;

    num = nm_global_dns_config_get_num_domains(global);
    for (i = 0; i < num; i++) {
        GVariantBuilder    conf_builder;
        GVariantBuilder    item_builder;
        const char        *domain_name;
        const char *const *servers;

        g_variant_builder_init(&conf_builder, G_VARIANT_TYPE("a{sv}"));

        domain      = nm_global_dns_config_get_domain(global, i);
        domain_name = nm_global_dns_domain_get_name(domain);

        if (domain_name && !nm_streq0(domain_name, "*")) {
            g_variant_builder_init(&item_builder, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&item_builder, "s", domain_name);
            g_variant_builder_add(&conf_builder,
                                  "{sv}",
                                  "domains",
                                  g_variant_builder_end(&item_builder));
        }

        g_variant_builder_init(&item_builder, G_VARIANT_TYPE("as"));
        for (servers = nm_global_dns_domain_get_servers(domain); *servers; servers++) {
            g_variant_builder_add(&item_builder, "s", *servers);
        }
        g_variant_builder_add(&conf_builder,
                              "{sv}",
                              "nameservers",
                              g_variant_builder_end(&item_builder));

        g_variant_builder_add(&conf_builder,
                              "{sv}",
                              "priority",
                              g_variant_new_int32(NM_DNS_PRIORITY_DEFAULT_NORMAL));

        g_variant_builder_add(builder, "a{sv}", &conf_builder);
    }
}

static GVariant *
_get_config_variant(NMDnsManager *self)
{
    NMDnsManagerPrivate         *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    NMGlobalDnsConfig           *global_config;
    gs_free char                *str = NULL;
    GVariantBuilder              builder;
    NMDnsConfigIPData           *ip_data;
    const CList                 *head;
    gs_unref_ptrarray GPtrArray *array_domains = NULL;

    if (priv->config_variant)
        return priv->config_variant;

    g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));

    global_config = nm_config_data_get_global_dns_config(nm_config_get_data(priv->config));
    if (global_config)
        _get_global_config_variant(&builder, global_config);

    head = _mgr_get_ip_data_lst_head(self);
    c_list_for_each_entry (ip_data, head, ip_data_lst) {
        GVariantBuilder    entry_builder;
        GVariantBuilder    strv_builder;
        guint              num;
        guint              num_domains;
        guint              num_searches;
        guint              i;
        const char        *ifname;
        const char *const *strarr;

        strarr = nm_l3_config_data_get_nameservers(ip_data->l3cd, ip_data->addr_family, &num);
        if (num == 0)
            continue;

        g_variant_builder_init(&entry_builder, G_VARIANT_TYPE("a{sv}"));

        g_variant_builder_init(&strv_builder, G_VARIANT_TYPE("as"));
        for (i = 0; i < num; i++) {
            g_variant_builder_add(&strv_builder, "s", strarr[i]);
        }
        g_variant_builder_add(&entry_builder,
                              "{sv}",
                              "nameservers",
                              g_variant_builder_end(&strv_builder));

        nm_l3_config_data_get_domains(ip_data->l3cd, ip_data->addr_family, &num_domains);
        nm_l3_config_data_get_searches(ip_data->l3cd, ip_data->addr_family, &num_searches);
        num = num_domains + num_searches;
        if (num > 0) {
            if (!array_domains)
                array_domains = g_ptr_array_sized_new(num);
            else
                g_ptr_array_set_size(array_domains, 0);

            nm_l3_config_data_get_dns_domains(array_domains,
                                              ip_data->addr_family,
                                              ip_data->l3cd,
                                              TRUE,
                                              FALSE);
            if (array_domains->len) {
                g_variant_builder_add(&entry_builder,
                                      "{sv}",
                                      "domains",
                                      g_variant_new_strv((const char *const *) array_domains->pdata,
                                                         array_domains->len));
            }
        }

        ifname = nm_platform_link_get_name(NM_PLATFORM_GET, ip_data->data->ifindex);
        if (ifname) {
            g_variant_builder_add(&entry_builder,
                                  "{sv}",
                                  "interface",
                                  g_variant_new_string(ifname));
        }

        g_variant_builder_add(&entry_builder,
                              "{sv}",
                              "priority",
                              g_variant_new_int32(_dns_config_ip_data_get_dns_priority(ip_data)));

        g_variant_builder_add(
            &entry_builder,
            "{sv}",
            "vpn",
            g_variant_new_boolean(ip_data->ip_config_type == NM_DNS_IP_CONFIG_TYPE_VPN));

        g_variant_builder_add(&builder, "a{sv}", &entry_builder);
    }

    priv->config_variant = g_variant_ref_sink(g_variant_builder_end(&builder));
    _LOGT("current configuration: %s", (str = g_variant_print(priv->config_variant, TRUE)));

    return priv->config_variant;
}

static void
get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMDnsManager        *self = NM_DNS_MANAGER(object);
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    switch (prop_id) {
    case PROP_MODE:
        g_value_set_string(value, priv->mode);
        break;
    case PROP_RC_MANAGER:
        g_value_set_string(value, _rc_manager_to_string(priv->rc_manager));
        break;
    case PROP_CONFIGURATION:
        g_value_set_variant(value, _get_config_variant(self));
        break;
    case PROP_UPDATE_PENDING:
        g_value_set_boolean(value, nm_dns_manager_get_update_pending(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
nm_dns_manager_init(NMDnsManager *self)
{
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    _LOGT("creating...");

    c_list_init(&priv->configs_lst_head);
    c_list_init(&priv->ip_data_lst_head);

    priv->config = g_object_ref(nm_config_get());

    G_STATIC_ASSERT_EXPR(G_STRUCT_OFFSET(NMDnsConfigData, ifindex) == 0);
    priv->configs_dict = g_hash_table_new_full(nm_pint_hash,
                                               nm_pint_equal,
                                               (GDestroyNotify) _dns_config_data_free,
                                               NULL);
    init_resolv_conf_mode(self, TRUE);

    /* We init plugin hashes with defaults that exist but won't set update flags
     * so there are not any unnecessary writes to resolvconf on startup etc. */
    update_plugin_hashes(self, FALSE);
    compute_master_hash(self, priv->master_hash);

    g_signal_connect(G_OBJECT(priv->config),
                     NM_CONFIG_SIGNAL_CONFIG_CHANGED,
                     G_CALLBACK(config_changed_cb),
                     self);
}

static void
dispose(GObject *object)
{
    NMDnsManager        *self = NM_DNS_MANAGER(object);
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);
    NMDnsConfigIPData   *ip_data, *ip_data_safe;

    _LOGT("disposing");

    if (!priv->is_stopped)
        nm_dns_manager_stop(self);

    if (priv->config)
        g_signal_handlers_disconnect_by_func(priv->config, config_changed_cb, self);

    _clear_sd_resolved_plugin(self);
    _clear_plugin(self);
    _clear_resolvconf_plugin(self);

    nm_clear_g_source_inst(&priv->update_pending_unblock);

    priv->best_ip_config_4 = NULL;
    priv->best_ip_config_6 = NULL;

    c_list_for_each_entry_safe (ip_data, ip_data_safe, &priv->ip_data_lst_head, ip_data_lst)
        _dns_config_ip_data_free(ip_data);

    nm_clear_pointer(&priv->configs_dict, g_hash_table_destroy);
    nm_assert(c_list_is_empty(&priv->configs_lst_head));

    nm_clear_g_source(&priv->plugin_ratelimit.timer);

    g_clear_object(&priv->config);

    G_OBJECT_CLASS(nm_dns_manager_parent_class)->dispose(object);

    nm_clear_pointer(&priv->config_variant, g_variant_unref);
}

static void
finalize(GObject *object)
{
    NMDnsManager        *self = NM_DNS_MANAGER(object);
    NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE(self);

    g_free(priv->hostdomain);
    g_free(priv->mode);

    G_OBJECT_CLASS(nm_dns_manager_parent_class)->finalize(object);
}

static const NMDBusInterfaceInfoExtended interface_info_dns_manager = {
    .parent = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
        NM_DBUS_INTERFACE_DNS_MANAGER,
        .properties = NM_DEFINE_GDBUS_PROPERTY_INFOS(
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Mode", "s", NM_DNS_MANAGER_MODE),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("RcManager",
                                                           "s",
                                                           NM_DNS_MANAGER_RC_MANAGER),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Configuration",
                                                           "aa{sv}",
                                                           NM_DNS_MANAGER_CONFIGURATION), ), ),
};

static void
nm_dns_manager_class_init(NMDnsManagerClass *klass)
{
    GObjectClass      *object_class      = G_OBJECT_CLASS(klass);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(klass);

    object_class->dispose      = dispose;
    object_class->finalize     = finalize;
    object_class->get_property = get_property;

    dbus_object_class->export_path     = NM_DBUS_EXPORT_PATH_STATIC(NM_DBUS_PATH "/DnsManager");
    dbus_object_class->interface_infos = NM_DBUS_INTERFACE_INFOS(&interface_info_dns_manager);
    dbus_object_class->export_on_construction = TRUE;

    obj_properties[PROP_MODE] = g_param_spec_string(NM_DNS_MANAGER_MODE,
                                                    "",
                                                    "",
                                                    NULL,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_RC_MANAGER] =
        g_param_spec_string(NM_DNS_MANAGER_RC_MANAGER,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONFIGURATION] =
        g_param_spec_variant(NM_DNS_MANAGER_CONFIGURATION,
                             "",
                             "",
                             G_VARIANT_TYPE("aa{sv}"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_UPDATE_PENDING] =
        g_param_spec_boolean(NM_DNS_MANAGER_UPDATE_PENDING,
                             "",
                             "",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);

    signals[CONFIG_CHANGED] = g_signal_new(NM_DNS_MANAGER_CONFIG_CHANGED,
                                           G_OBJECT_CLASS_TYPE(object_class),
                                           G_SIGNAL_RUN_FIRST,
                                           0,
                                           NULL,
                                           NULL,
                                           g_cclosure_marshal_VOID__VOID,
                                           G_TYPE_NONE,
                                           0);
}
