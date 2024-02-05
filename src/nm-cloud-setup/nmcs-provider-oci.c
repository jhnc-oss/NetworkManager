/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "libnm-client-aux-extern/nm-default-client.h"
#include "nmcs-provider-oci.h"
#include "nm-cloud-setup-utils.h"
#include "ctype.h"
#include "netinet/in.h"

/**********************************************************************/

#define HTTP_TIMEOUT_MS      3000
#define HTTP_REQ_MAX_DATA    512 * 1024
#define HTTP_POLL_TIMEOUT_MS 10000
#define HTTP_RATE_LIMIT_MS   1000

#define NM_OCI_HOST             "169.254.169.254"
#define NM_OCI_BASE             "https://" NM_OCI_HOST
#define NM_OCI_API_VERSION      "/opc/v2"
#define NM_OCI_METADATA_URL_NET /* $NM_OCI_BASE/$NM_OCI_API_VERSION */ "/vnics/"

#define NM_OCI_METADATA_HEADER "Authorization: Bearer Oracle"

NMCS_DEFINE_HOST_BASE(_oci_base, NMCS_ENV_NM_CLOUD_SETUP_OCI_HOST, NM_OCI_BASE);

#define _oci_uri_concat(...) \
    nmcs_utils_uri_build_concat(_oci_base(), NM_OCI_API_VERSION, __VA_ARGS__)
#define _oci_uri_interfaces(...) _oci_uri_concat(NM_OCI_METADATA_URL_NET, ##__VA_ARGS__)

/*****************************************************************************/

struct _NMCSProviderOCI {
    NMCSProvider parent;
};

struct _NMCSProviderOCIClass {
    NMCSProviderClass parent;
};

G_DEFINE_TYPE(NMCSProviderOCI, nmcs_provider_oci, NMCS_TYPE_PROVIDER);

/*****************************************************************************/

static void
_detect_get_meta_data_done_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    gs_unref_object GTask *task      = user_data;
    gs_free_error GError  *get_error = NULL;
    gs_free_error GError  *error     = NULL;

    nm_http_client_poll_req_finish(NM_HTTP_CLIENT(source), result, NULL, NULL, &get_error);

    if (nm_utils_error_is_cancelled(get_error)) {
        g_task_return_error(task, g_steal_pointer(&get_error));
        return;
    }

    if (get_error) {
        nm_utils_error_set(&error,
                           NM_UTILS_ERROR_UNKNOWN,
                           "failure to get OCI metadata: %s",
                           get_error->message);
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_boolean(task, TRUE);
}

static void
detect(NMCSProvider *provider, GTask *task)
{
    NMHttpClient *http_client;
    gs_free char *uri = NULL;

    http_client = nmcs_provider_get_http_client(provider);

    nm_http_client_poll_req(http_client,
                            (uri = _oci_uri_concat("/vnics")),
                            HTTP_TIMEOUT_MS,
                            256 * 1024,
                            7000,
                            1000,
                            NM_MAKE_STRV(NM_OCI_METADATA_HEADER),
                            NULL,
                            g_task_get_cancellable(task),
                            NULL,
                            NULL,
                            _detect_get_meta_data_done_cb,
                            task);
}

/*****************************************************************************/

typedef enum {
    GET_CONFIG_FETCH_TYPE_IPV4_PRIVATEIP,
    GET_CONFIG_FETCH_TYPE_IPV4_SUBNETCIDR,
    GET_CONFIG_FETCH_TYPE_IPV4_VIRTUALROUTERIP,
} GetConfigFetchType;

static void
_get_config_fetch_done_cb(NMHttpClient                   *http_client,
                          GAsyncResult                   *result,
                          NMCSProviderGetConfigIfaceData *config_iface_data,
                          GetConfigFetchType              fetch_type)
{
    gs_unref_bytes GBytes *response = NULL;
    gs_free_error GError  *error    = NULL;
    const char            *resp_str = NULL;
    gsize                  resp_len;
    in_addr_t              gateway  = htonl(INADDR_LOOPBACK);
    in_addr_t              tmp_addr = htonl(INADDR_LOOPBACK);
    int                    prefix   = -1;

    nm_http_client_poll_req_finish(http_client, result, NULL, &response, &error);

    if (nm_utils_error_is_cancelled(error))
        return;

    if (error)
        goto out;

    resp_str = g_bytes_get_data(response, &resp_len);
    nm_assert(resp_str[resp_len] == '\0');

    switch (fetch_type) {
    case GET_CONFIG_FETCH_TYPE_IPV4_PRIVATEIP:

        if (!nmcs_utils_ipaddr_normalize_bin(AF_INET, resp_str, resp_len, NULL, &tmp_addr)) {
            error =
                nm_utils_error_new(NM_UTILS_ERROR_UNKNOWN, "ip is not a valid private ip address");
            goto out;
        }
        _LOGD("get-config: skip fetching meta data for %ld", config_iface_data->iface_idx);
        config_iface_data->ipv4s_arr[config_iface_data->ipv4s_len] = tmp_addr;
        config_iface_data->has_ipv4s                               = TRUE;
        config_iface_data->ipv4s_len++;
        _LOGD("get-config: skip fetching meta data for Private IP %u - %lu",
              tmp_addr,
              config_iface_data->ipv4s_len);
        break;

    case GET_CONFIG_FETCH_TYPE_IPV4_SUBNETCIDR:
        config_iface_data->cidr_addr   = tmp_addr;
        config_iface_data->has_cidr    = TRUE;
        config_iface_data->cidr_prefix = prefix;
        if (nm_inet_parse_with_prefix_bin(AF_INET,
                                          g_bytes_get_data(response, NULL),
                                          NULL,
                                          &tmp_addr,
                                          &prefix)) {
            _LOGD("get-config: skip fetching meta data for Subnet %u - %d", tmp_addr, prefix);
        }
        break;

    case GET_CONFIG_FETCH_TYPE_IPV4_VIRTUALROUTERIP:

        config_iface_data->has_gateway = TRUE;
        config_iface_data->gateway     = gateway;
        if (nm_inet_parse_bin(AF_INET, g_bytes_get_data(response, NULL), NULL, &gateway)) {
            _LOGD("get-config: skip fetching meta data for VirtualRouteIP %u", gateway);
        }
        break;
    }

out:
    config_iface_data->get_config_data->n_pending--;
    _nmcs_provider_get_config_task_maybe_return(config_iface_data->get_config_data,
                                                g_steal_pointer(&error));
}

static void
_get_config_fetch_done_cb_ipv4_privateip(GObject *source, GAsyncResult *result, gpointer user_data)
{
    _get_config_fetch_done_cb(NM_HTTP_CLIENT(source),
                              result,
                              user_data,
                              GET_CONFIG_FETCH_TYPE_IPV4_PRIVATEIP);
}

static void
_get_config_fetch_done_cb_ipv4_subnetcidr(GObject *source, GAsyncResult *result, gpointer user_data)
{
    _get_config_fetch_done_cb(NM_HTTP_CLIENT(source),
                              result,
                              user_data,
                              GET_CONFIG_FETCH_TYPE_IPV4_SUBNETCIDR);
}

static void
_get_config_fetch_done_cb_ipv4_virtualrouterip(GObject      *source,
                                               GAsyncResult *result,
                                               gpointer      user_data)
{
    _get_config_fetch_done_cb(NM_HTTP_CLIENT(source),
                              result,
                              user_data,
                              GET_CONFIG_FETCH_TYPE_IPV4_VIRTUALROUTERIP);
}

typedef struct {
    guint iface_idx;
    char  path[]; /* Flexible array member to store the MAC address */
} GetConfigMetadataMac;

static void
_get_config_metadata_ready_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    NMCSProviderGetConfigTaskData *get_config_data;
    gs_unref_hashtable GHashTable *response_parsed = NULL;
    gs_free_error GError          *error           = NULL;
    GetConfigMetadataMac          *v_mac_data;
    const char                    *v_hwaddr;
    GHashTableIter                 h_iter;
    NMHttpClient                  *http_client;
    char                           iface_idx_char[30];

    nm_http_client_poll_req_finish(NM_HTTP_CLIENT(source), result, NULL, NULL, &error);

    if (nm_utils_error_is_cancelled(error))
        return;

    get_config_data = user_data;

    response_parsed                     = g_steal_pointer(&get_config_data->extra_data);
    get_config_data->extra_data_destroy = NULL;

    /* We ignore errors. Only if we got no response at all, it's a problem.
     * Otherwise, we proceed with whatever we could fetch. */
    if (!response_parsed) {
        _nmcs_provider_get_config_task_maybe_return(
            get_config_data,
            nm_utils_error_new(NM_UTILS_ERROR_UNKNOWN, "meta data for interfaces not found"));
        return;
    }

    http_client = nmcs_provider_get_http_client(g_task_get_source_object(get_config_data->task));

    g_hash_table_iter_init(&h_iter, response_parsed);
    while (g_hash_table_iter_next(&h_iter, (gpointer *) &v_hwaddr, (gpointer *) &v_mac_data)) {
        NMCSProviderGetConfigIfaceData *config_iface_data;
        gs_free char                   *uri1 = NULL;
        gs_free char                   *uri2 = NULL;
        gs_free char                   *uri3 = NULL;

        config_iface_data = g_hash_table_lookup(get_config_data->result_dict, v_hwaddr);

        if (!config_iface_data) {
            if (!get_config_data->any) {
                _LOGD("get-config: skip fetching meta data for %s (%s)",
                      v_hwaddr,
                      v_mac_data->path);
                continue;
            }
            config_iface_data =
                nmcs_provider_get_config_iface_data_create(get_config_data, FALSE, v_hwaddr);
        }

        nm_assert(config_iface_data->iface_idx == -1);

        config_iface_data->iface_idx = v_mac_data->iface_idx;

        _LOGD("get-config: start fetching meta data for #%" G_GSSIZE_FORMAT ", %s (%s)",
              config_iface_data->iface_idx,
              config_iface_data->hwaddr,
              v_mac_data->path);

        get_config_data->n_pending++;
        nm_http_client_poll_req(http_client,
                                (uri1 = _oci_uri_interfaces(iface_idx_char, "/privateIp/")),
                                HTTP_TIMEOUT_MS,
                                512 * 1024,
                                10000,
                                1000,
                                NM_MAKE_STRV(NM_OCI_METADATA_HEADER),
                                NULL,
                                get_config_data->intern_cancellable,
                                NULL,
                                NULL,
                                _get_config_fetch_done_cb_ipv4_privateip,
                                config_iface_data);

        get_config_data->n_pending++;
        nm_http_client_poll_req(NM_HTTP_CLIENT(source),
                                (uri2 = _oci_uri_interfaces(iface_idx_char, "/subnetCidrBlock/")),
                                HTTP_TIMEOUT_MS,
                                512 * 1024,
                                10000,
                                1000,
                                NM_MAKE_STRV(NM_OCI_METADATA_HEADER),
                                NULL,
                                get_config_data->intern_cancellable,
                                NULL,
                                NULL,
                                _get_config_fetch_done_cb_ipv4_subnetcidr,
                                config_iface_data);

        get_config_data->n_pending++;
        nm_http_client_poll_req(NM_HTTP_CLIENT(source),
                                (uri3 = _oci_uri_interfaces(iface_idx_char, "/virtualRouterIp/")),
                                HTTP_TIMEOUT_MS,
                                512 * 1024,
                                10000,
                                1000,
                                NM_MAKE_STRV(NM_OCI_METADATA_HEADER),
                                NULL,
                                get_config_data->intern_cancellable,
                                NULL,
                                NULL,
                                _get_config_fetch_done_cb_ipv4_virtualrouterip,
                                config_iface_data);
    }

    _nmcs_provider_get_config_task_maybe_return(get_config_data, NULL);
}

/* Helper function to trim double quotes and extra spaces from a string */
static char *
trim_quotes_and_spaces(const char *str)
{
    gsize len;
    // Trim leading spaces
    while (isspace((unsigned char) (*str))) {
        str++;
    }

    len = strlen(str);
    if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        // Trim double quotes
        str++;
        len -= 2;
    }

    // Trim trailing spaces
    while (len > 0 && isspace((unsigned char) str[len - 1])) {
        len--;
    }

    return g_strndup(str, len);
}

static gboolean
_get_config_metadata_ready_check(long     response_code,
                                 GBytes  *response,
                                 gpointer check_user_data,
                                 GError **error)
{
    NMCSProviderGetConfigTaskData *get_config_data = check_user_data;
    gs_unref_hashtable GHashTable *response_parsed = NULL;
    const guint8                  *r_data;
    const char                    *cur_line;
    gsize                          r_len;
    gsize                          cur_line_len;
    GHashTableIter                 h_iter;
    gboolean                       has_all;
    const char                    *c_hwaddr;
    gssize                         iface_idx_counter = 0;

    if (response_code != 200 || !response) {
        /* we wait longer. */
        return FALSE;
    }

    r_data = g_bytes_get_data(response, &r_len);
    /* NMHttpClient guarantees that there is a trailing NUL after the data. */
    nm_assert(r_data[r_len] == 0);

    while (nm_utils_parse_next_line((const char **) &r_data, &r_len, &cur_line, &cur_line_len)) {
        GetConfigMetadataMac *mac_data;
        char                 *hwaddr;
        const char           *mac_addr_prefix;
        const char           *mac_addr_start;
        const char           *mac_addr_end;

        if (cur_line_len == 0)
            continue;

        // Find "macAddr": "xx:xx:xx:xx:xx:xx"
        mac_addr_prefix = "macAddr\": \"";
        mac_addr_start  = strstr(cur_line, mac_addr_prefix);
        if (mac_addr_start) {
            mac_addr_start += strlen(mac_addr_prefix);
            mac_addr_end = strchr(mac_addr_start, '\"');
            if (mac_addr_end) {
                // Extract MAC address
                hwaddr = trim_quotes_and_spaces(mac_addr_start);
            } else {
                // Ignoring error: no closing double quote found
                continue;
            }
        } else {
            // No MAC address found in this line
            continue;
        }

        if (!response_parsed)
            response_parsed = g_hash_table_new_full(nm_str_hash, g_str_equal, g_free, g_free);

        mac_data            = g_malloc(sizeof(GetConfigMetadataMac) + 1u + cur_line_len);
        mac_data->iface_idx = iface_idx_counter++;
        memcpy(mac_data->path, cur_line, cur_line_len + 1u);

        /* here we will ignore duplicate responses. */
        g_hash_table_insert(response_parsed, hwaddr, mac_data);
    }

    has_all = TRUE;
    g_hash_table_iter_init(&h_iter, get_config_data->result_dict);
    while (g_hash_table_iter_next(&h_iter, (gpointer *) &c_hwaddr, NULL)) {
        if (!response_parsed || !g_hash_table_contains(response_parsed, c_hwaddr)) {
            has_all = FALSE;
            break;
        }
    }

    nm_clear_pointer(&get_config_data->extra_data, g_hash_table_unref);
    if (response_parsed) {
        get_config_data->extra_data         = g_steal_pointer(&response_parsed);
        get_config_data->extra_data_destroy = (GDestroyNotify) g_hash_table_unref;
    }
    return has_all;
}

static void
get_config(NMCSProvider *provider, NMCSProviderGetConfigTaskData *get_config_data)
{
    gs_free char *uri = NULL;

    /* First we fetch the "macs/". If the caller requested some particular
     * MAC addresses, then we poll until we see them. They might not yet be
     * around from the start...
     */
    nm_http_client_poll_req(nmcs_provider_get_http_client(provider),
                            (uri = _oci_uri_interfaces()),
                            HTTP_TIMEOUT_MS,
                            256 * 1024,
                            15000,
                            1000,
                            NM_MAKE_STRV(NM_OCI_METADATA_HEADER),
                            NULL,
                            get_config_data->intern_cancellable,
                            _get_config_metadata_ready_check,
                            get_config_data,
                            _get_config_metadata_ready_cb,
                            get_config_data);
}

/*****************************************************************************/

static void
nmcs_provider_oci_init(NMCSProviderOCI *self)
{}

static void
dispose(GObject *object)
{
    G_OBJECT_CLASS(nmcs_provider_oci_parent_class)->dispose(object);
}

static void
nmcs_provider_oci_class_init(NMCSProviderOCIClass *klass)
{
    GObjectClass      *object_class   = G_OBJECT_CLASS(klass);
    NMCSProviderClass *provider_class = NMCS_PROVIDER_CLASS(klass);

    object_class->dispose = dispose;

    provider_class->_name                 = "oci";
    provider_class->_env_provider_enabled = NMCS_ENV_NM_CLOUD_SETUP_OCI;
    provider_class->detect                = detect;
    provider_class->get_config            = get_config;
}