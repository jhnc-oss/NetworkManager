/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2024 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-dns-dnsconfd.h"

#include "libnm-glib-aux/nm-dbus-aux.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-platform/nm-platform.h"
#include "nm-utils.h"
#include "nm-dbus-manager.h"
#include "NetworkManagerUtils.h"
#include "nm-l3-config-data.h"
#include "nm-manager.h"
#include "devices/nm-device.h"
#include "nm-active-connection.h"

typedef struct {
    GDBusConnection *dbus_connection;
    GCancellable    *update_cancellable;
    char            *name_owner;
    guint            name_owner_changed_id;
    GCancellable    *name_owner_cancellable;
    GVariant        *latest_update_args;
} NMDnsDnsconfdPrivate;

struct _NMDnsDnsconfd {
    NMDnsPlugin          parent;
    NMDnsDnsconfdPrivate _priv;
};

struct _NMDnsDnsconfdClass {
    NMDnsPluginClass parent;
};

G_DEFINE_TYPE(NMDnsDnsconfd, nm_dns_dnsconfd, NM_TYPE_DNS_PLUGIN)

#define NM_DNS_DNSCONFD_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMDnsDnsconfd, NM_IS_DNS_DNSCONFD, NMDnsPlugin)

#define _NMLOG_DOMAIN      LOGD_DNS
#define _NMLOG(level, ...) __NMLOG_DEFAULT(level, _NMLOG_DOMAIN, "dnsconfd", __VA_ARGS__)

#define DNSCONFD_DBUS_SERVICE "com.redhat.dnsconfd"

/*****************************************************************************/

static void
dnsconfd_update_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    NMDnsDnsconfd             *self;
    NMDnsDnsconfdPrivate      *priv;
    gs_free_error GError      *error    = NULL;
    gs_unref_variant GVariant *response = NULL;
    gboolean                   all_ok   = FALSE;
    gchar                     *dnsconfd_message;

    response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &error);

    if (nm_utils_error_is_cancelled(error))
        return;

    self = user_data;
    priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);

    nm_clear_g_cancellable(&priv->update_cancellable);

    if (!response)
        _LOGW("dnsconfd update failed: %s", error->message);

    // By using &s we will get pointer to char data contained
    // in variant and thus no freing of dnsconfd_message is required
    g_variant_get(response, "(b&s)", &all_ok, &dnsconfd_message);
    if (all_ok) {
        _LOGD("dnsconfd update successful");
    } else {
        _LOGW("dnsconfd update failed: %s", dnsconfd_message);
    }
    _nm_dns_plugin_update_pending_maybe_changed(NM_DNS_PLUGIN(self));
}

static gboolean
is_default_interface_explicit(const CList *ip_data_lst_head)
{
    guint              n_domains;
    const char *const *strv_domains;
    NMDnsConfigIPData *ip_data;
    gboolean           is_routing;

    // if there is ~. specified in either searches or domains then default interface is explicit
    // AFAIK it should not be passible to pass "." through DHCP and thus we will check only
    // searches

    c_list_for_each_entry (ip_data, ip_data_lst_head, ip_data_lst) {
        strv_domains =
            nm_l3_config_data_get_searches(ip_data->l3cd, ip_data->addr_family, &n_domains);
        for (int i = 0; i < n_domains; i++) {
            if (nm_streq(nm_utils_parse_dns_domain(strv_domains[i], &is_routing), ".")) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void
gather_interface_domains(NMDnsConfigIPData *ip_data,
                         gboolean           is_default_explicit,
                         const char      ***routing_domains,
                         const char      ***search_domains)
{
    guint              n_domains;
    const char *const *strv_domains;
    gboolean           is_routing;
    const char        *cur_domain;
    GPtrArray         *routing_ptr_array = g_ptr_array_sized_new(5);
    GPtrArray         *search_ptr_array  = g_ptr_array_sized_new(5);

    // searches have higher priority than domains (dynamically retrieved)
    strv_domains = nm_l3_config_data_get_searches(ip_data->l3cd, ip_data->addr_family, &n_domains);
    if (!n_domains) {
        strv_domains =
            nm_l3_config_data_get_domains(ip_data->l3cd, ip_data->addr_family, &n_domains);
    }

    for (int i = 0; i < n_domains; i++) {
        cur_domain = nm_utils_parse_dns_domain(strv_domains[i], &is_routing);
        g_ptr_array_add(routing_ptr_array, (char *) cur_domain);
        if (!is_routing) {
            g_ptr_array_add(search_ptr_array, (char *) cur_domain);
        }
    }

    // if there has been specified search like ~. then we will not be adding . and respect
    // users wishes
    if (!is_default_explicit
        && nm_l3_config_data_get_best_default_route(ip_data->l3cd, ip_data->addr_family)) {
        g_ptr_array_add(routing_ptr_array, ".");
    }
    g_ptr_array_add(routing_ptr_array, NULL);
    g_ptr_array_add(search_ptr_array, NULL);

    // when array would be empty we will simply return NULL
    *routing_domains =
        (const char **) g_ptr_array_free(routing_ptr_array, (routing_ptr_array->len == 1));
    *search_domains =
        (const char **) g_ptr_array_free(search_ptr_array, (search_ptr_array->len == 1));
}

static void
get_networks(NMDnsConfigIPData *ip_data, char ***networks)
{
    NMDedupMultiIter ipconf_iter;
    const NMPObject *obj;
    char             s_address[INET6_ADDRSTRLEN];
    // +4 because INET6_ADDRSTRLEN already contains byte for end of string and we need 4 bytes
    // to store max 3 characters of mask and slash (/128 for example)
    char       network_buffer[INET6_ADDRSTRLEN + 4];
    GPtrArray *ptr_array = g_ptr_array_sized_new(5);
    guint      is_ipv4   = NM_IS_IPv4(ip_data->addr_family);

    // this is an ipv4 mapped to ipv6 and should be the longest possible address that can
    // be specified in ipv6 format
    NM_STATIC_ASSERT(sizeof(network_buffer)
                     >= sizeof("0000:0000:0000:0000:0000:ffff:192.168.100.228/128"));

    nm_l3_config_data_iter_obj_for_each (&ipconf_iter,
                                         ip_data->l3cd,
                                         &obj,
                                         NMP_OBJECT_TYPE_IP_ADDRESS(is_ipv4)) {
        inet_ntop(ip_data->addr_family,
                  NMP_OBJECT_CAST_IP_ADDRESS(obj)->address_ptr,
                  s_address,
                  INET6_ADDRSTRLEN);
        if (is_ipv4) {
            sprintf(network_buffer, "%s/32", s_address);
        } else {
            sprintf(network_buffer, "%s/128", s_address);
        }
        g_ptr_array_add(ptr_array, strdup(network_buffer));
    }

    nm_l3_config_data_iter_obj_for_each (&ipconf_iter,
                                         ip_data->l3cd,
                                         &obj,
                                         NMP_OBJECT_TYPE_IP_ROUTE(is_ipv4)) {
        const NMPlatformIPRoute *route = NMP_OBJECT_CAST_IP_ROUTE(obj);
        if (NM_PLATFORM_IP_ROUTE_IS_DEFAULT(route)) {
            continue;
        }
        inet_ntop(ip_data->addr_family, route->network_ptr, s_address, INET6_ADDRSTRLEN);
        sprintf(network_buffer, "%s/%u", s_address, route->plen);
        g_ptr_array_add(ptr_array, strdup(network_buffer));
    }

    g_ptr_array_add(ptr_array, NULL);
    // if array would be empty then return NULL
    *networks = (char **) g_ptr_array_free(ptr_array, ptr_array->len == 1);
}

static void
server_builder_append_interface_info(GVariantBuilder *argument_builder,
                                     const char      *interface,
                                     char           **networks,
                                     const char      *connection_id,
                                     const char      *connection_uuid,
                                     const char      *dbus_path)
{
    if (connection_id) {
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "connection-id",
                              g_variant_new("s", connection_id));
    }
    if (connection_uuid) {
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "connection-uuid",
                              g_variant_new("s", connection_uuid));
    }
    if (dbus_path) {
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "connection-object",
                              g_variant_new("s", dbus_path));
    }
    if (interface) {
        g_variant_builder_add(argument_builder, "{sv}", "interface", g_variant_new("s", interface));
    }
    if (networks) {
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "networks",
                              g_variant_new_strv((const char *const *) networks, -1));
    }
    g_variant_builder_close(argument_builder);
}

static gboolean
server_builder_append_base(GVariantBuilder   *argument_builder,
                           int                address_family,
                           const char        *address_string,
                           const char *const *routing_domains,
                           const char *const *search_domains,
                           const char        *ca)
{
    NMDnsServer dns_server;
    gsize       addr_size;

    if (!nm_dns_uri_parse(address_family, address_string, &dns_server))
        return FALSE;
    addr_size = nm_utils_addr_family_to_size(dns_server.addr_family);

    g_variant_builder_open(argument_builder, G_VARIANT_TYPE("a{sv}"));

    // no freeing needing in this section as builder takes ownership of all data

    g_variant_builder_add(argument_builder,
                          "{sv}",
                          "address",
                          nm_g_variant_new_ay((gconstpointer) &dns_server.addr, addr_size));
    if (dns_server.scheme == NM_DNS_URI_SCHEME_TLS)
        g_variant_builder_add(argument_builder, "{sv}", "protocol", g_variant_new("s", "dns+tls"));
    if (dns_server.servername)
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "name",
                              g_variant_new("s", dns_server.servername));
    if (routing_domains) {
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "routing_domains",
                              g_variant_new_strv(routing_domains, -1));
    }
    if (search_domains) {
        g_variant_builder_add(argument_builder,
                              "{sv}",
                              "search_domains",
                              g_variant_new_strv(search_domains, -1));
    }
    if (ca) {
        g_variant_builder_add(argument_builder, "{sv}", "ca", g_variant_new("s", ca));
    }
    return TRUE;
}

static void
parse_global_config(const NMGlobalDnsConfig *global_config,
                    GVariantBuilder         *argument_builder,
                    guint                   *resolving_mode)
{
    NMGlobalDnsDomain *domain;
    const char *const *servers;
    const char        *name;
    const char        *routing_domains[2] = {0};
    const char *const *searches           = nm_global_dns_config_get_searches(global_config);
    const char        *ca = nm_global_dns_config_get_certification_authority(global_config);

    *resolving_mode = nm_global_dns_config_get_resolving_mode(global_config);

    for (int i = 0; i < nm_global_dns_config_get_num_domains(global_config); i++) {
        domain  = nm_global_dns_config_get_domain(global_config, i);
        servers = nm_global_dns_domain_get_servers(domain);
        if (!servers) {
            continue;
        }
        name               = nm_global_dns_domain_get_name(domain);
        routing_domains[0] = nm_streq(name, "*") ? "." : name;

        for (int j = 0; servers[j]; j++) {
            if (server_builder_append_base(argument_builder,
                                           AF_UNSPEC,
                                           servers[j],
                                           routing_domains,
                                           searches,
                                           ca)) {
                g_variant_builder_close(argument_builder);
            }
        }
    }
}

static void
send_dnsconfd_update(NMDnsDnsconfd *self)
{
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);
    // it is safe to clear cancellable here even if it is not initialized,
    // as g_object_new initializes private part with zeros and nm_clear_g_cancellable
    // checks whether the variable != NULL
    nm_clear_g_cancellable(&priv->update_cancellable);
    priv->update_cancellable = g_cancellable_new();

    g_dbus_connection_call(priv->dbus_connection,
                           priv->name_owner,
                           "/com/redhat/dnsconfd",
                           "com.redhat.dnsconfd.Manager",
                           "Update",
                           priv->latest_update_args,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           20000,
                           priv->update_cancellable,
                           dnsconfd_update_done,
                           self);
    _nm_dns_plugin_update_pending_maybe_changed(NM_DNS_PLUGIN(self));
}

static void
name_owner_changed(NMDnsDnsconfd *self, const char *name_owner)
{
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);

    name_owner = nm_str_not_empty(name_owner);

    if (nm_streq0(priv->name_owner, name_owner))
        return;

    g_free(priv->name_owner);
    priv->name_owner = g_strdup(name_owner);

    if (!name_owner) {
        _LOGT("D-Bus name for dnsconfd disappeared");
        return;
    }

    _LOGT("D-Bus name for dnsconfd got owner %s", name_owner);
    send_dnsconfd_update(self);

    _nm_dns_plugin_update_pending_maybe_changed(NM_DNS_PLUGIN(self));
}

static void
name_owner_changed_cb(GDBusConnection *connection,
                      const char      *sender_name,
                      const char      *object_path,
                      const char      *interface_name,
                      const char      *signal_name,
                      GVariant        *parameters,
                      gpointer         user_data)
{
    NMDnsDnsconfd *self = user_data;
    const char    *new_owner;

    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)")))
        return;

    g_variant_get(parameters, "(&s&s&s)", NULL, NULL, &new_owner);

    name_owner_changed(self, new_owner);
}

static void
get_name_owner_cb(const char *name_owner, GError *error, gpointer user_data)
{
    if (!name_owner && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    name_owner_changed(user_data, name_owner);
}

static guint
ensure_all_connected(NMDnsDnsconfd *self)
{
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);

    if (!priv->dbus_connection) {
        priv->dbus_connection = nm_g_object_ref(NM_MAIN_DBUS_CONNECTION_GET);
        if (!priv->dbus_connection) {
            return 1;
        }
    }

    if (priv->name_owner) {
        return 0;
    }

    if (!priv->name_owner_changed_id) {
        priv->name_owner_changed_id =
            nm_dbus_connection_signal_subscribe_name_owner_changed(priv->dbus_connection,
                                                                   DNSCONFD_DBUS_SERVICE,
                                                                   name_owner_changed_cb,
                                                                   self,
                                                                   NULL);
    }

    if (!priv->name_owner_cancellable) {
        nm_clear_g_cancellable(&priv->name_owner_cancellable);
        priv->name_owner_cancellable = g_cancellable_new();

        nm_dbus_connection_call_get_name_owner(priv->dbus_connection,
                                               DNSCONFD_DBUS_SERVICE,
                                               -1,
                                               priv->name_owner_cancellable,
                                               get_name_owner_cb,
                                               self);
    }
    return 2;
}

static void
parse_all_interface_config(GVariantBuilder *argument_builder, const CList *ip_data_lst_head)
{
    NMDnsConfigIPData  *ip_data;
    const char *const  *dns_server_strings;
    guint               nameserver_count;
    const char         *ifname;
    const char        **routing_domains;
    const char        **search_domains;
    char              **networks;
    NMDevice           *device;
    NMActiveConnection *active_connection;
    const char         *connection_id;
    const char         *connection_uuid;
    const char         *dbus_path;
    gboolean            explicit_default = is_default_interface_explicit(ip_data_lst_head);

    c_list_for_each_entry (ip_data, ip_data_lst_head, ip_data_lst) {
        dns_server_strings = nm_l3_config_data_get_nameservers(ip_data->l3cd,
                                                               ip_data->addr_family,
                                                               &nameserver_count);
        if (!nameserver_count)
            continue;
        ifname = nm_platform_link_get_name(NM_PLATFORM_GET, ip_data->data->ifindex);
        device = nm_manager_get_device_by_ifindex(NM_MANAGER_GET, ip_data->data->ifindex);
        active_connection = nm_device_get_active_connection(device);

        // presume that when we have server of this interface then the interface has to have
        // an active connection
        nm_assert(active_connection);

        connection_id   = nm_active_connection_get_settings_connection_id(active_connection);
        connection_uuid = nm_active_connection_get_settings_connection_uuid(active_connection);
        dbus_path       = nm_device_get_active_connection_dbus_path(device);

        // dbus_path also should be set
        nm_assert(dbus_path && dbus_path[0] != 0);

        gather_interface_domains(ip_data, explicit_default, &routing_domains, &search_domains);
        get_networks(ip_data, &networks);

        for (int i = 0; i < nameserver_count; i++) {
            if (server_builder_append_base(argument_builder,
                                           ip_data->addr_family,
                                           dns_server_strings[i],
                                           routing_domains,
                                           search_domains,
                                           NULL)) {
                server_builder_append_interface_info(argument_builder,
                                                     ifname,
                                                     networks,
                                                     connection_id,
                                                     connection_uuid,
                                                     dbus_path);
            }
        }
        // no need to free insides of routing and search domains, as they point to data
        // owned elsewhere on the other hand networks are created by us and thus we need to also
        // free the data
        if (routing_domains) {
            g_free(routing_domains);
        }
        if (search_domains) {
            g_free(search_domains);
        }
        if (networks) {
            g_strfreev(networks);
        }
    }
}

static gboolean
update(NMDnsPlugin             *plugin,
       const NMGlobalDnsConfig *global_config,
       const CList             *ip_data_lst_head,
       const char              *hostdomain,
       GError                 **error)
{
    NMDnsDnsconfd        *self = NM_DNS_DNSCONFD(plugin);
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);

    GVariantBuilder argument_builder;
    GVariant       *args;
    gchar          *debug_string;

    guint all_connected;
    guint resolving_mode = 0;

    g_variant_builder_init(&argument_builder, G_VARIANT_TYPE("(aa{sv}u)"));
    g_variant_builder_open(&argument_builder, G_VARIANT_TYPE("aa{sv}"));

    if (global_config) {
        _LOGD("Parsing global configuration");
        parse_global_config(global_config, &argument_builder, &resolving_mode);
    }
    _LOGD("Parsing configuration of interfaces");
    parse_all_interface_config(&argument_builder, ip_data_lst_head);

    g_variant_builder_close(&argument_builder);
    g_variant_builder_add(&argument_builder, "u", resolving_mode);

    _LOGD("Converting builder of update arguments to GVariant");
    args = g_variant_builder_end(&argument_builder);
    if (nm_logging_get_level(LOGD_DNS) <= LOGL_DEBUG) {
        // knowing how the update looks will be immensely helpful during debugging
        debug_string = g_variant_print(args, TRUE);
        _LOGD("Arguments variant is composed like: %s", debug_string);
        g_free(debug_string);
    }

    nm_clear_pointer(&priv->latest_update_args, g_variant_unref);
    priv->latest_update_args = g_variant_ref_sink(args);

    all_connected = ensure_all_connected(self);

    if (all_connected == 1) {
        nm_utils_error_set(error,
                           NM_UTILS_ERROR_UNKNOWN,
                           "no D-Bus connection available to talk to dnsconfd");
        // not connected to dbus, can do nothing here
        return FALSE;
    } else if (all_connected == 2) {
        // we do not have name owner yet, and have to wait
        return TRUE;
    }

    send_dnsconfd_update(self);

    return TRUE;
}

static void
dispose(GObject *object)
{
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(NM_DNS_DNSCONFD(object));

    _LOGD("Disposing of Dnsconfd plugin");

    nm_clear_g_cancellable(&priv->update_cancellable);
    nm_clear_g_cancellable(&priv->name_owner_cancellable);
    nm_clear_g_dbus_connection_signal(priv->dbus_connection, &priv->name_owner_changed_id);
    if (priv->name_owner) {
        g_free(priv->name_owner);
    }
    if (priv->latest_update_args) {
        g_variant_unref(priv->latest_update_args);
    }

    G_OBJECT_CLASS(nm_dns_dnsconfd_parent_class)->dispose(object);

    g_clear_object(&priv->dbus_connection);
}

static void
stop(NMDnsPlugin *plugin)
{
    GVariantBuilder       stop_builder;
    NMDnsDnsconfd        *self = NM_DNS_DNSCONFD(plugin);
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);

    if (!priv->dbus_connection || !priv->name_owner) {
        return;
    }

    _LOGI("Sending empty update to stop Dnsconfd from resolving");

    // on stop we will send empty uncancellable update
    // which will cause Dnsconfd to stop all resolution

    g_variant_builder_init(&stop_builder, G_VARIANT_TYPE("(aa{sv}u)"));
    g_variant_builder_open(&stop_builder, G_VARIANT_TYPE("aa{sv}"));
    g_variant_builder_close(&stop_builder);
    g_variant_builder_add(&stop_builder, "u", 0);

    // stop any current updates from happening
    nm_clear_g_cancellable(&priv->update_cancellable);

    g_dbus_connection_call(priv->dbus_connection,
                           priv->name_owner,
                           "/com/redhat/dnsconfd",
                           "com.redhat.dnsconfd.Manager",
                           "Update",
                           g_variant_builder_end(&stop_builder),
                           NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START,
                           20000,
                           NULL,
                           NULL,
                           NULL);

    _nm_dns_plugin_update_pending_maybe_changed(NM_DNS_PLUGIN(self));
}

static gboolean
_update_pending_detect(NMDnsDnsconfd *self)
{
    NMDnsDnsconfdPrivate *priv = NM_DNS_DNSCONFD_GET_PRIVATE(self);

    if (priv->update_cancellable) {
        /* update in progress */
        return TRUE;
    }

    return FALSE;
}

static gboolean
get_update_pending(NMDnsPlugin *plugin)
{
    NMDnsDnsconfd *self = NM_DNS_DNSCONFD(plugin);
    return _update_pending_detect(self);
}

static void
nm_dns_dnsconfd_init(NMDnsDnsconfd *self)
{}

NMDnsPlugin *
nm_dns_dnsconfd_new(void)
{
    return g_object_new(NM_TYPE_DNS_DNSCONFD, NULL);
}

static void
nm_dns_dnsconfd_class_init(NMDnsDnsconfdClass *dns_class)
{
    NMDnsPluginClass *plugin_class = NM_DNS_PLUGIN_CLASS(dns_class);
    GObjectClass     *object_class = G_OBJECT_CLASS(dns_class);

    object_class->dispose = dispose;

    plugin_class->plugin_name        = "dnsconfd";
    plugin_class->is_caching         = TRUE;
    plugin_class->stop               = stop;
    plugin_class->update             = update;
    plugin_class->get_update_pending = get_update_pending;
}
