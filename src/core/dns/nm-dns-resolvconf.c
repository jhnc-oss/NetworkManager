/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2024 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include <resolv.h>

#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-glib-aux/nm-str-buf.h"
#include "nm-dns-resolvconf.h"
#include "NetworkManagerUtils.h"
#include "nm-l3-config-data.h"

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

/*****************************************************************************/

typedef enum { SR_SUCCESS, SR_NOTFOUND, SR_ERROR } SpawnResult;

typedef struct {
    GPtrArray *nameservers;
    GPtrArray *searches;
    GPtrArray *options;
    char      *nis_domain;
    GPtrArray *nis_servers;
    NMTernary  has_trust_ad;
} NMResolvConfDataInProgress;

typedef struct {
    char    **nameservers;
    char    **searches;
    char    **options;
    char     *nis_domain;
    char    **nis_servers;
    NMTernary has_trust_ad;
} NMResolvConfData;

static NM_UTILS_LOOKUP_STR_DEFINE(
    _config_type_to_string,
    NMDnsIPConfigType,
    NM_UTILS_LOOKUP_DEFAULT_WARN("<unknown>"),
    NM_UTILS_LOOKUP_STR_ITEM(NM_DNS_IP_CONFIG_TYPE_REMOVED, "removed"),
    NM_UTILS_LOOKUP_STR_ITEM(NM_DNS_IP_CONFIG_TYPE_DEFAULT, "default"),
    NM_UTILS_LOOKUP_STR_ITEM(NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE, "best"),
    NM_UTILS_LOOKUP_STR_ITEM(NM_DNS_IP_CONFIG_TYPE_VPN, "vpn"), );

/*****************************************************************************/

typedef struct {
    NMResolvConfData last_update;
    gboolean         dns_touched;
    gboolean         resolver_depends_on_nm;
    gboolean         caching_successful;
} NMDnsResolvconfPrivate;

struct _NMDnsResolvconf {
    NMDnsPlugin            parent;
    NMDnsResolvconfPrivate _priv;
};

struct _NMDnsResolvconfClass {
    NMDnsPluginClass parent;
};

G_DEFINE_TYPE(NMDnsResolvconf, nm_dns_resolvconf, NM_TYPE_DNS_PLUGIN)

#define NM_DNS_RESOLVCONF_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMDnsResolvconf, NM_IS_DNS_RESOLVCONF, NMDnsPlugin)

#define _NMLOG_DOMAIN      LOGD_DNS
#define _NMLOG(level, ...) __NMLOG_DEFAULT(level, _NMLOG_DOMAIN, "resolvconf", __VA_ARGS__)

/*****************************************************************************/

static GPid
run_netconfig(GError **error, int *stdin_fd)
{
    char         *argv[5];
    gs_free char *tmp = NULL;
    GPid          pid = -1;

    argv[0] = NETCONFIG_PATH;
    argv[1] = "modify";
    argv[2] = "--service";
    argv[3] = "NetworkManager";
    argv[4] = NULL;

    _LOGD("spawning '%s'", (tmp = g_strjoinv(" ", argv)));

    if (!g_spawn_async_with_pipes(NULL,
                                  argv,
                                  NULL,
                                  G_SPAWN_CLOEXEC_PIPES | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL,
                                  NULL,
                                  &pid,
                                  stdin_fd,
                                  NULL,
                                  NULL,
                                  error))
        return -1;

    return pid;
}

static void
netconfig_construct_str(GString *str, const char *key, const char *value)
{
    if (value) {
        _LOGD("writing to netconfig: %s='%s'", key, value);
        g_string_append_printf(str, "%s='%s'\n", key, value);
    }
}

static void
netconfig_construct_strv(GString *str, const char *key, const char *const *values)
{
    if (values) {
        gs_free char *value = NULL;

        value = g_strjoinv(" ", (char **) values);
        netconfig_construct_str(str, key, value);
    }
}

static SpawnResult
dispatch_netconfig(const char *const *searches,
                   const char *const *nameservers,
                   const char        *nis_domain,
                   const char *const *nis_servers,
                   GError           **error)
{
    GPid                          pid;
    int                           fd;
    int                           errsv;
    int                           status;
    gssize                        l;
    nm_auto_free_gstring GString *str = NULL;

    pid = run_netconfig(error, &fd);
    if (pid <= 0)
        return SR_NOTFOUND;

    str = g_string_new("");

    /* NM is writing already-merged DNS information to netconfig, so it
     * does not apply to a specific network interface.
     */
    netconfig_construct_str(str, "INTERFACE", "NetworkManager");
    netconfig_construct_strv(str, "DNSSEARCH", searches);
    netconfig_construct_strv(str, "DNSSERVERS", nameservers);
    netconfig_construct_str(str, "NISDOMAIN", nis_domain);
    netconfig_construct_strv(str, "NISSERVERS", nis_servers);

again:
    l = write(fd, str->str, str->len);
    if (l == -1) {
        if (errno == EINTR)
            goto again;
    }

    nm_close(fd);

    /* FIXME: don't write to netconfig synchronously. */

    /* Wait until the process exits */
    if (!nm_utils_kill_child_sync(pid, 0, LOGD_DNS, "netconfig", &status, 1000, 0)) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Error waiting for netconfig to exit: %s",
                    nm_strerror_native(errsv));
        return SR_ERROR;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Error calling netconfig: %s %d",
                    WIFEXITED(status) ? "exited with status"
                                      : (WIFSIGNALED(status) ? "exited with signal"
                                                             : "exited with unknown reason"),
                    WIFEXITED(status) ? WEXITSTATUS(status)
                                      : (WIFSIGNALED(status) ? WTERMSIG(status) : status));
        return SR_ERROR;
    }
    return SR_SUCCESS;
}

static char *
create_resolv_conf(const char *const *searches,
                   const char *const *nameservers,
                   const char *const *options)
{
    GString *str;
    gsize    i;

    str = g_string_new_len(NULL, 245);

    g_string_append(str, "# Generated by NetworkManager\n");

    if (searches && searches[0]) {
        gsize search_base_idx;

        g_string_append(str, "search");
        search_base_idx = str->len;

        for (i = 0; searches[i]; i++) {
            const char *s = searches[i];
            gsize       l = strlen(s);

            if (l == 0 || NM_STRCHAR_ANY(s, ch, NM_IN_SET(ch, ' ', '\t', '\n'))) {
                /* there should be no such characters in the search entry. Also,
                 * because glibc parser would treat them as line/word separator.
                 *
                 * Skip the value silently. */
                continue;
            }

            if (search_base_idx > 0) {
                if (str->len - search_base_idx + 1 + l > 254) {
                    /* this entry crosses the 256 character boundary. Older glibc versions
                     * would truncate the entry at this point.
                     *
                     * Fill the line with spaces to cross the 256 char boundary and continue
                     * afterwards. This way, the truncation happens between two search entries. */
                    while (str->len - search_base_idx < 257)
                        g_string_append_c(str, ' ');
                    search_base_idx = 0;
                }
            }

            g_string_append_c(str, ' ');
            g_string_append_len(str, s, l);
        }
        g_string_append_c(str, '\n');
    }

    if (nameservers && nameservers[0]) {
        for (i = 0; nameservers[i]; i++) {
            if (i == 3) {
                g_string_append(
                    str,
                    "# NOTE: the libc resolver may not support more than 3 nameservers.\n");
                g_string_append(str, "# The nameservers listed below may not be recognized.\n");
            }
            g_string_append(str, "nameserver ");
            g_string_append(str, nameservers[i]);
            g_string_append_c(str, '\n');
        }
    }

    if (options && options[0]) {
        g_string_append(str, "options");
        for (i = 0; options[i]; i++) {
            /* omit internal options, starting with '_' */
            if (options[i][0] != '_') {
                g_string_append_c(str, ' ');
                g_string_append(str, options[i]);
            }
        }
        g_string_append_c(str, '\n');
    }

    return g_string_free(str, FALSE);
}

static void
add_dns_option_item(GPtrArray *array, const char *str)
{
    if (_nm_utils_dns_option_find_idx((const char *const *) array->pdata, array->len, str) < 0)
        g_ptr_array_add(array, g_strdup(str));
}

char *
nmtst_dns_create_resolv_conf(const char *const *searches,
                             const char *const *nameservers,
                             const char *const *options)
{
    return create_resolv_conf(searches, nameservers, options);
}

static gboolean
write_resolv_conf_contents(FILE *f, const char *content, GError **error)
{
    int errsv;

    if (fprintf(f, "%s", content) < 0) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not write " _PATH_RESCONF ": %s",
                    nm_strerror_native(errsv));
        errno = errsv;
        return FALSE;
    }

    return TRUE;
}

static gboolean
write_resolv_conf(FILE              *f,
                  const char *const *searches,
                  const char *const *nameservers,
                  const char *const *options,
                  GError           **error)
{
    gs_free char *content = NULL;

    content = create_resolv_conf(searches, nameservers, options);
    return write_resolv_conf_contents(f, content, error);
}

static SpawnResult
dispatch_resolvconf(char **searches, char **nameservers, char **options, GError **error)
{
    gs_free char *cmd = NULL;
    FILE         *f;
    gboolean      success = FALSE;
    int           errsv;
    int           err;
    char         *argv[] = {RESOLVCONF_PATH, "-d", "NetworkManager", NULL};
    int           status;

    if (!g_file_test(RESOLVCONF_PATH, G_FILE_TEST_IS_EXECUTABLE)) {
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_FAILED,
                            RESOLVCONF_PATH " is not executable");
        return SR_NOTFOUND;
    }

    if (!searches && !nameservers) {
        _LOGI("Removing DNS information from %s", RESOLVCONF_PATH);

        if (!g_spawn_sync("/", argv, NULL, 0, NULL, NULL, NULL, NULL, &status, error))
            return SR_ERROR;

        if (status != 0) {
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_FAILED,
                        "%s returned error code",
                        RESOLVCONF_PATH);
            return SR_ERROR;
        }

        return SR_SUCCESS;
    }

    _LOGI("Writing DNS information to %s", RESOLVCONF_PATH);

    /* FIXME: don't write to resolvconf synchronously. */

    cmd = g_strconcat(RESOLVCONF_PATH, " -a ", "NetworkManager", NULL);
    if ((f = popen(cmd, "w")) == NULL) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not write to %s: %s",
                    RESOLVCONF_PATH,
                    nm_strerror_native(errsv));
        return SR_ERROR;
    }

    success = write_resolv_conf(f,
                                NM_CAST_STRV_CC(searches),
                                NM_CAST_STRV_CC(nameservers),
                                NM_CAST_STRV_CC(options),
                                error);
    err     = pclose(f);
    if (err < 0) {
        errsv = errno;
        g_clear_error(error);
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errsv),
                    "Failed to close pipe to resolvconf: %d",
                    errsv);
        return SR_ERROR;
    } else if (err > 0) {
        _LOGW("resolvconf failed with status %d", err);
        g_clear_error(error);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "resolvconf failed with status %d", err);
        return SR_ERROR;
    }

    return success ? SR_SUCCESS : SR_ERROR;
}

static const char *
_read_link_cached(const char *path, gboolean *is_cached, char **cached)
{
    nm_assert(is_cached);
    nm_assert(cached);

    if (*is_cached)
        return *cached;

    nm_assert(!*cached);
    *is_cached = TRUE;
    return (*cached = g_file_read_link(path, NULL));
}

#define MY_RESOLV_CONF     NMRUNDIR "/resolv.conf"
#define MY_RESOLV_CONF_TMP MY_RESOLV_CONF ".tmp"
#define RESOLV_CONF_TMP    "/etc/.resolv.conf.NetworkManager"

#define NO_STUB_RESOLV_CONF NMRUNDIR "/no-stub-resolv.conf"

static void
_update_resolv_conf_no_stub(const char *const *searches,
                            const char *const *nameservers,
                            const char *const *options)
{
    gs_free char *content = NULL;
    GError       *local   = NULL;

    content = create_resolv_conf(searches, nameservers, options);

    if (!g_file_set_contents(NO_STUB_RESOLV_CONF, content, -1, &local)) {
        _LOGD("update-resolv-no-stub: failure to write file: %s", local->message);
        g_error_free(local);
        return;
    }

    _LOGT("update-resolv-no-stub: '%s' successfully written", NO_STUB_RESOLV_CONF);
}

static SpawnResult
update_resolv_conf(const char *const            *searches,
                   const char *const            *nameservers,
                   const char *const            *options,
                   GError                      **error,
                   NMDnsManagerResolvConfManager rc_manager)
{
    FILE         *f;
    gboolean      success;
    gs_free char *content           = NULL;
    SpawnResult   write_file_result = SR_SUCCESS;
    int           errsv;
    gboolean      resconf_link_cached = FALSE;
    gs_free char *resconf_link        = NULL;

    content = create_resolv_conf(searches, nameservers, options);

    if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE
        || (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK
            && !_read_link_cached(_PATH_RESCONF, &resconf_link_cached, &resconf_link))) {
        gs_free char      *rc_path_syml = NULL;
        nm_auto_free char *rc_path_real = NULL;
        const char        *rc_path      = _PATH_RESCONF;
        GError            *local        = NULL;

        if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE) {
            rc_path_real = realpath(_PATH_RESCONF, NULL);
            if (rc_path_real)
                rc_path = rc_path_real;
            else {
                /* realpath did not resolve a path-name. That either means,
                 * _PATH_RESCONF:
                 *   - does not exist
                 *   - is a plain file
                 *   - is a dangling symlink
                 *
                 * Handle the case, where it is a dangling symlink... */
                rc_path_syml = nm_utils_read_link_absolute(_PATH_RESCONF, NULL);
                if (rc_path_syml)
                    rc_path = rc_path_syml;
            }
        }

        /* we first write to /etc/resolv.conf directly. If that fails,
         * we still continue to write to runstatedir but remember the
         * error. */
        if (!g_file_set_contents(rc_path, content, -1, &local)) {
            _LOGT("update-resolv-conf: write to %s failed (rc-manager=%s, %s)",
                  rc_path,
                  _rc_manager_to_string(rc_manager),
                  local->message);
            g_propagate_error(error, local);
            /* clear @error, so that we don't try reset it. This is the error
             * we want to propagate to the caller. */
            error             = NULL;
            write_file_result = SR_ERROR;
        } else {
            _LOGT("update-resolv-conf: write to %s succeeded (rc-manager=%s)",
                  rc_path,
                  _rc_manager_to_string(rc_manager));
        }
    }

    if ((f = fopen(MY_RESOLV_CONF_TMP, "we")) == NULL) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not open %s: %s",
                    MY_RESOLV_CONF_TMP,
                    nm_strerror_native(errsv));
        _LOGT("update-resolv-conf: open temporary file %s failed (%s)",
              MY_RESOLV_CONF_TMP,
              nm_strerror_native(errsv));
        return SR_ERROR;
    }

    success = write_resolv_conf_contents(f, content, error);
    if (!success) {
        errsv = errno;
        _LOGT("update-resolv-conf: write temporary file %s failed (%s)",
              MY_RESOLV_CONF_TMP,
              nm_strerror_native(errsv));
    }

    if (fclose(f) < 0) {
        if (success) {
            errsv = errno;
            /* only set an error here if write_resolv_conf() was successful,
             * since its error is more important.
             */
            g_set_error(error,
                        NM_MANAGER_ERROR,
                        NM_MANAGER_ERROR_FAILED,
                        "Could not close %s: %s",
                        MY_RESOLV_CONF_TMP,
                        nm_strerror_native(errsv));
            _LOGT("update-resolv-conf: close temporary file %s failed (%s)",
                  MY_RESOLV_CONF_TMP,
                  nm_strerror_native(errsv));
        }
        return SR_ERROR;
    } else if (!success)
        return SR_ERROR;

    if (rename(MY_RESOLV_CONF_TMP, MY_RESOLV_CONF) < 0) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not replace %s: %s",
                    MY_RESOLV_CONF,
                    nm_strerror_native(errsv));
        _LOGT("update-resolv-conf: failed to rename temporary file %s to %s (%s)",
              MY_RESOLV_CONF_TMP,
              MY_RESOLV_CONF,
              nm_strerror_native(errsv));
        return SR_ERROR;
    }

    if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE) {
        _LOGT("update-resolv-conf: write internal file %s succeeded (rc-manager=%s)",
              MY_RESOLV_CONF,
              _rc_manager_to_string(rc_manager));
        return write_file_result;
    }

    if (rc_manager != NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK
        || !_read_link_cached(_PATH_RESCONF, &resconf_link_cached, &resconf_link)) {
        _LOGT("update-resolv-conf: write internal file %s succeeded", MY_RESOLV_CONF);
        return write_file_result;
    }

    if (!nm_streq0(_read_link_cached(_PATH_RESCONF, &resconf_link_cached, &resconf_link),
                   MY_RESOLV_CONF)) {
        _LOGT("update-resolv-conf: write internal file %s succeeded (don't touch symlink %s "
              "linking to %s)",
              MY_RESOLV_CONF,
              _PATH_RESCONF,
              _read_link_cached(_PATH_RESCONF, &resconf_link_cached, &resconf_link));
        return write_file_result;
    }

    /* By this point, /etc/resolv.conf exists and is a symlink to our internal
     * resolv.conf. We update the symlink so that applications get an inotify
     * notification.
     */
    if (unlink(RESOLV_CONF_TMP) != 0 && ((errsv = errno) != ENOENT)) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not unlink %s: %s",
                    RESOLV_CONF_TMP,
                    nm_strerror_native(errsv));
        _LOGT("update-resolv-conf: write internal file %s succeeded "
              "but cannot delete temporary file %s: %s",
              MY_RESOLV_CONF,
              RESOLV_CONF_TMP,
              nm_strerror_native(errsv));
        return SR_ERROR;
    }

    if (symlink(MY_RESOLV_CONF, RESOLV_CONF_TMP) == -1) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not create symlink %s pointing to %s: %s",
                    RESOLV_CONF_TMP,
                    MY_RESOLV_CONF,
                    nm_strerror_native(errsv));
        _LOGT("update-resolv-conf: write internal file %s succeeded "
              "but failed to symlink %s: %s",
              MY_RESOLV_CONF,
              RESOLV_CONF_TMP,
              nm_strerror_native(errsv));
        return SR_ERROR;
    }

    if (rename(RESOLV_CONF_TMP, _PATH_RESCONF) == -1) {
        errsv = errno;
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_FAILED,
                    "Could not rename %s to %s: %s",
                    RESOLV_CONF_TMP,
                    _PATH_RESCONF,
                    nm_strerror_native(errsv));
        _LOGT("update-resolv-conf: write internal file %s succeeded "
              "but failed to rename temporary symlink %s to %s: %s",
              MY_RESOLV_CONF,
              RESOLV_CONF_TMP,
              _PATH_RESCONF,
              nm_strerror_native(errsv));
        return SR_ERROR;
    }

    _LOGT("update-resolv-conf: write internal file %s succeeded and update symlink %s",
          MY_RESOLV_CONF,
          _PATH_RESCONF);
    return write_file_result;
}

static gboolean
merge_global_dns_config(NMResolvConfDataInProgress *rc, NMGlobalDnsConfig *global_conf)
{
    NMGlobalDnsDomain *default_domain;
    const char *const *searches;
    const char *const *options;
    const char *const *servers;
    guint              i;

    if (!global_conf)
        return FALSE;

    searches = nm_global_dns_config_get_searches(global_conf);
    if (searches) {
        for (i = 0; searches[i]; i++) {
            if (domain_is_routing(searches[i]))
                continue;
            if (!domain_is_valid(searches[i], FALSE, TRUE))
                continue;
            nm_utils_g_ptr_array_add_string_item(rc->searches, searches[i], TRUE);
        }
    }

    options = nm_global_dns_config_get_options(global_conf);
    if (options) {
        for (i = 0; options[i]; i++)
            nm_utils_g_ptr_array_add_string_item(rc->options, options[i], TRUE);
    }

    default_domain = nm_global_dns_config_lookup_domain(global_conf, "*");
    if (!default_domain)
        return TRUE;

    servers = nm_global_dns_domain_get_servers(default_domain);
    if (!servers)
        return TRUE;

    for (i = 0; servers[i]; i++) {
        char addrstr[NM_INET_ADDRSTRLEN];

        /* TODO: support IPv6 link-local addresses with scope id */
        if (!nm_dns_uri_parse_plain(AF_UNSPEC, servers[i], addrstr, NULL))
            continue;

        nm_utils_g_ptr_array_add_string_item(rc->nameservers, addrstr, TRUE);
    }

    return TRUE;
}

static const char *
get_nameserver_list(int addr_family, const NML3ConfigData *l3cd, NMStrBuf *tmp_strbuf)
{
    guint              num;
    guint              i;
    const char *const *strarr;

    nm_str_buf_reset(tmp_strbuf);

    strarr = nm_l3_config_data_get_nameservers(l3cd, addr_family, &num);
    for (i = 0; i < num; i++) {
        if (i > 0)
            nm_str_buf_append_c(tmp_strbuf, ' ');
        nm_str_buf_append(tmp_strbuf, strarr[i]);
    }

    nm_str_buf_maybe_expand(tmp_strbuf, 1, FALSE);
    return nm_str_buf_get_str(tmp_strbuf);
}

static char **
_ptrarray_to_strv(GPtrArray *parray)
{
    if (parray->len > 0)
        g_ptr_array_add(parray, NULL);
    return (char **) g_ptr_array_free(parray, parray->len == 0);
}

static void
merge_one_l3cd(NMResolvConfDataInProgress *rc,
               int                         addr_family,
               int                         ifindex,
               const NML3ConfigData       *l3cd)
{
    char               buf[NM_INET_ADDRSTRLEN + 50];
    gboolean           has_trust_ad;
    guint              num_nameservers;
    guint              num;
    guint              i;
    const char *const *strarr;

    nm_assert(ifindex == nm_l3_config_data_get_ifindex(l3cd));

    strarr = nm_l3_config_data_get_nameservers(l3cd, addr_family, &num_nameservers);
    for (i = 0; i < num_nameservers; i++) {
        NMIPAddr a;

        if (!nm_dns_uri_parse_plain(addr_family, strarr[i], NULL, &a))
            continue;

        if (addr_family == AF_INET)
            nm_inet_ntop(addr_family, &a, buf);
        else if (IN6_IS_ADDR_V4MAPPED(&a))
            nm_inet4_ntop(a.addr6.s6_addr32[3], buf);
        else {
            nm_inet6_ntop(&a.addr6, buf);
            if (IN6_IS_ADDR_LINKLOCAL(&a)) {
                const char *ifname;

                ifname = nm_platform_link_get_name(NM_PLATFORM_GET, ifindex);
                if (ifname) {
                    g_strlcat(buf, "%", sizeof(buf));
                    g_strlcat(buf, ifname, sizeof(buf));
                }
            }
        }

        nm_utils_g_ptr_array_add_string_item(rc->nameservers, buf, TRUE);
    }

    nm_l3_config_data_get_dns_domains(rc->searches, addr_family, l3cd, FALSE, TRUE);

    has_trust_ad = FALSE;
    strarr       = nm_l3_config_data_get_dns_options(l3cd, addr_family, &num);
    for (i = 0; i < num; i++) {
        const char *option = strarr[i];

        if (nm_streq(option, NM_SETTING_DNS_OPTION_TRUST_AD)) {
            has_trust_ad = TRUE;
            continue;
        }
        add_dns_option_item(rc->options, option);
    }

    if (num_nameservers == 0) {
        /* If the @l3cd contributes no DNS servers, ignore whether trust-ad is set or unset
         * for this @l3cd. */
    } else if (has_trust_ad) {
        /* We only set has_trust_ad to TRUE, if all IP configs agree (or don't contribute).
         * Once set to FALSE, it doesn't get reset. */
        if (rc->has_trust_ad == NM_TERNARY_DEFAULT)
            rc->has_trust_ad = NM_TERNARY_TRUE;
    } else
        rc->has_trust_ad = NM_TERNARY_FALSE;

    if (addr_family == AF_INET) {
        const in_addr_t *nis_servers;
        const char      *nis_domain;

        nis_servers = nm_l3_config_data_get_nis_servers(l3cd, &num);
        for (i = 0; i < num; i++)
            nm_utils_g_ptr_array_add_string_item(rc->nis_servers,
                                                 nm_inet4_ntop(nis_servers[i], buf),
                                                 TRUE);

        if ((nis_domain = nm_l3_config_data_get_nis_domain(l3cd))) {
            /* FIXME: handle multiple domains */
            if (!rc->nis_domain)
                rc->nis_domain = g_strdup(nis_domain);
        }
    }
}

static void
_clear_resolv_conf_data(NMDnsResolvconfPrivate *priv)
{
    g_strfreev(priv->last_update.nameservers);
    g_strfreev(priv->last_update.nis_servers);
    g_strfreev(priv->last_update.options);
    g_strfreev(priv->last_update.searches);

    g_free(priv->last_update.nis_domain);
    priv->last_update = (NMResolvConfData) {
        .nameservers  = NULL,
        .searches     = NULL,
        .options      = NULL,
        .nis_domain   = NULL,
        .nis_servers  = NULL,
        .has_trust_ad = NM_TERNARY_DEFAULT,
    };
    priv->resolver_depends_on_nm = FALSE;
    priv->caching_successful     = FALSE;
}

static void
_finalize_resolv_conf_data(NMDnsResolvconfPrivate *priv, NMResolvConfDataInProgress in_progress)
{
    priv->last_update.nameservers  = _ptrarray_to_strv(in_progress.nameservers);
    priv->last_update.nis_servers  = _ptrarray_to_strv(in_progress.nis_servers);
    priv->last_update.options      = _ptrarray_to_strv(in_progress.options);
    priv->last_update.searches     = _ptrarray_to_strv(in_progress.searches);
    priv->last_update.has_trust_ad = in_progress.has_trust_ad;
    priv->last_update.nis_domain   = in_progress.nis_domain;
}

static NMResolvConfDataInProgress
_collect_resolv_conf_data(const CList       *ip_data_lst_head,
                          NMGlobalDnsConfig *global_config,
                          const char        *hostdomain)
{
    NMResolvConfDataInProgress data_in_progress = {.nameservers  = g_ptr_array_sized_new(5),
                                                   .nis_servers  = g_ptr_array_sized_new(5),
                                                   .options      = g_ptr_array_sized_new(5),
                                                   .searches     = g_ptr_array_sized_new(5),
                                                   .nis_domain   = NULL,
                                                   .has_trust_ad = NM_TERNARY_DEFAULT};

    if (global_config)
        merge_global_dns_config(&data_in_progress, global_config);

    if (!global_config || !nm_global_dns_config_lookup_domain(global_config, "*")) {
        nm_auto_str_buf NMStrBuf tmp_strbuf = NM_STR_BUF_INIT(0, FALSE);
        int                      first_prio = 0;
        const NMDnsConfigIPData *ip_data;
        gboolean                 is_first = TRUE;

        c_list_for_each_entry (ip_data, ip_data_lst_head, ip_data_lst) {
            gboolean skip = FALSE;
            int      dns_priority;

            if (!nm_l3_config_data_get_dns_priority(ip_data->l3cd,
                                                    ip_data->addr_family,
                                                    &dns_priority))
                nm_assert_not_reached();

            if (is_first) {
                is_first   = FALSE;
                first_prio = dns_priority;
            } else if (first_prio < 0 && first_prio != dns_priority)
                skip = TRUE;

            _LOGT("config: %8d %-7s v%c %-5d %s: %s",
                  dns_priority,
                  _config_type_to_string(ip_data->ip_config_type),
                  nm_utils_addr_family_to_char(ip_data->addr_family),
                  ip_data->data->ifindex,
                  skip ? "<SKIP>" : "",
                  get_nameserver_list(ip_data->addr_family, ip_data->l3cd, &tmp_strbuf));

            if (!skip)
                merge_one_l3cd(&data_in_progress,
                               ip_data->addr_family,
                               ip_data->data->ifindex,
                               ip_data->l3cd);
        }
    }

    if (hostdomain)
        nm_utils_g_ptr_array_add_string_item(data_in_progress.searches, hostdomain, TRUE);

    if (data_in_progress.has_trust_ad == NM_TERNARY_TRUE)
        g_ptr_array_add(data_in_progress.options, g_strdup(NM_SETTING_DNS_OPTION_TRUST_AD));

    return data_in_progress;
}

static gboolean
update(NMDnsPlugin *plugin, NMDnsUpdateData *update_data, GError **error)
{
    NMResolvConfDataInProgress data_in_progress;
    NMDnsResolvconf           *self                      = NM_DNS_RESOLVCONF(plugin);
    NMDnsResolvconfPrivate    *priv                      = NM_DNS_RESOLVCONF_GET_PRIVATE(self);
    char                      *substitute_nameservers[2] = {0};
    gs_strfreev char         **substitute_options        = NULL;
    char                     **final_nameservers         = NULL;
    char                     **final_options             = NULL;
    gboolean                   resolv_conf_updated       = FALSE;
    SpawnResult                result                    = SR_SUCCESS;

    _clear_resolv_conf_data(priv);

    data_in_progress = _collect_resolv_conf_data(update_data->ip_data_lst_head,
                                                 update_data->global_config,
                                                 update_data->hostdomain);

    _finalize_resolv_conf_data(priv, data_in_progress);

    _update_resolv_conf_no_stub(NM_CAST_STRV_CC(priv->last_update.searches),
                                NM_CAST_STRV_CC(priv->last_update.nameservers),
                                NM_CAST_STRV_CC(priv->last_update.options));

    if (NM_IN_SET(update_data->rc_manager,
                  NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED,
                  NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE)) {
        _LOGD("update-dns: not updating resolv.conf");
        return TRUE;
    } else {
        _LOGD("update-dns: updating resolv.conf");
    }

    priv->caching_successful     = update_data->caching_successful;
    priv->resolver_depends_on_nm = update_data->resolver_depends_on_nm;
    /* If caching was successful, we only send 127.0.0.1 to /etc/resolv.conf
     * to ensure that the glibc resolver doesn't try to round-robin nameservers,
     * but only uses the local caching nameserver.
     */
    if (update_data->caching_successful) {
        gboolean need_edns0;
        gboolean need_trust;

        if (update_data->resolved_used) {
            /* systemd-resolved uses 127.0.0.53 link-local address */
            substitute_nameservers[0] = "127.0.0.53";
        } else {
            substitute_nameservers[0] = "127.0.0.1";
        }

        need_edns0 = !nm_strv_contains(priv->last_update.options, -1, NM_SETTING_DNS_OPTION_EDNS0)
                     && !nm_strv_contains(priv->last_update.options,
                                          -1,
                                          NM_SETTING_DNS_OPTION_INTERNAL_NO_ADD_EDNS0);
        need_trust =
            !nm_strv_contains(priv->last_update.options, -1, NM_SETTING_DNS_OPTION_TRUST_AD)
            && !nm_strv_contains(priv->last_update.options,
                                 -1,
                                 NM_SETTING_DNS_OPTION_INTERNAL_NO_ADD_TRUST_AD);

        if (need_edns0 || need_trust) {
            gsize len;

            len                = NM_PTRARRAY_LEN(priv->last_update.options);
            substitute_options = g_malloc(sizeof(char *) * (len + 3u));
            if (need_edns0)
                substitute_options[len++] = g_strdup(NM_SETTING_DNS_OPTION_EDNS0);
            if (need_trust)
                substitute_options[len++] = g_strdup(NM_SETTING_DNS_OPTION_TRUST_AD);
            substitute_options[len] = NULL;
        }
    }

    final_nameservers =
        (substitute_nameservers[0] ? substitute_nameservers : priv->last_update.nameservers);
    final_options = (substitute_options ? substitute_options : priv->last_update.options);

    switch (update_data->rc_manager) {
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK:
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE:
        result              = update_resolv_conf(NM_CAST_STRV_CC(priv->last_update.searches),
                                    NM_CAST_STRV_CC(final_nameservers),
                                    NM_CAST_STRV_CC(final_options),
                                    error,
                                    update_data->rc_manager);
        resolv_conf_updated = TRUE;
        break;
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF:
        result = dispatch_resolvconf(priv->last_update.searches,
                                     final_nameservers,
                                     final_options,
                                     error);
        break;
    case NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG:
        result = dispatch_netconfig((const char *const *) priv->last_update.searches,
                                    (const char *const *) final_nameservers,
                                    priv->last_update.nis_domain,
                                    (const char *const *) final_options,
                                    error);
        break;
    default:
        nm_assert_not_reached();
    }

    if (result == SR_NOTFOUND) {
        _LOGD("update-dns: program not available, writing to resolv.conf");
        g_clear_error(error);
        result              = update_resolv_conf(NM_CAST_STRV_CC(priv->last_update.searches),
                                    NM_CAST_STRV_CC(final_nameservers),
                                    NM_CAST_STRV_CC(final_options),
                                    error,
                                    NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK);
        resolv_conf_updated = TRUE;
    }

    /* Unless we've already done it, update private resolv.conf in NMRUNDIR
     * ignoring any errors */
    if (!resolv_conf_updated) {
        update_resolv_conf(NM_CAST_STRV_CC(priv->last_update.searches),
                           NM_CAST_STRV_CC(final_nameservers),
                           NM_CAST_STRV_CC(final_options),
                           NULL,
                           NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED);
    } else {
        priv->dns_touched = TRUE;
    }

    return result == SR_SUCCESS;
}

static void
resolvconf_checksum(const NML3ConfigData *l3cd,
                    GChecksum            *sum,
                    int                   addr_family,
                    NMDnsIPConfigType     dns_ip_config_type)
{
    guint              i;
    const char *const *strarr;
    const in_addr_t   *wins;
    guint              num_elements;
    gboolean           empty = TRUE;
    int                prio  = 0;

    g_return_if_fail(l3cd);
    g_return_if_fail(sum);

    strarr = nm_l3_config_data_get_nameservers(l3cd, addr_family, &num_elements);
    for (i = 0; i < num_elements; i++) {
        g_checksum_update(sum, (gpointer) strarr[i], strlen(strarr[i]));
        empty = FALSE;
    }

    if (addr_family == AF_INET) {
        wins = nm_l3_config_data_get_wins(l3cd, &num_elements);
        for (i = 0; i < num_elements; i++) {
            g_checksum_update(sum, (guint8 *) &wins[i], 4);
            empty = FALSE;
        }
    }

    /* Resolvconf without servers serves to no purpose */
    if (empty) {
        return;
    }

    strarr = nm_l3_config_data_get_domains(l3cd, addr_family, &num_elements);
    for (i = 0; i < num_elements; i++) {
        g_checksum_update(sum, (const guint8 *) strarr[i], strlen(strarr[i]));
    }

    strarr = nm_l3_config_data_get_searches(l3cd, addr_family, &num_elements);
    for (i = 0; i < num_elements; i++) {
        g_checksum_update(sum, (const guint8 *) strarr[i], strlen(strarr[i]));
    }

    strarr = nm_l3_config_data_get_dns_options(l3cd, addr_family, &num_elements);
    for (i = 0; i < num_elements; i++) {
        g_checksum_update(sum, (const guint8 *) strarr[i], strlen(strarr[i]));
    }

    g_checksum_update(sum, (const guint8 *) &dns_ip_config_type, sizeof(dns_ip_config_type));
    nm_l3_config_data_get_dns_priority(l3cd, addr_family, &prio);
    g_checksum_update(sum, (const guint8 *) &prio, sizeof(prio));
}

/*****************************************************************************/

static void
nm_dns_resolvconf_init(NMDnsResolvconf *self)
{
    NMDnsResolvconfPrivate *priv   = NM_DNS_RESOLVCONF_GET_PRIVATE(self);
    priv->last_update.has_trust_ad = NM_TERNARY_DEFAULT;
}

NMDnsPlugin *
nm_dns_resolvconf_new(void)
{
    return g_object_new(NM_TYPE_DNS_RESOLVCONF, NULL);
}

static void
stop(NMDnsPlugin *plugin)
{
    NMDnsResolvconf        *self = NM_DNS_RESOLVCONF(plugin);
    NMDnsResolvconfPrivate *priv = NM_DNS_RESOLVCONF_GET_PRIVATE(self);

    /* If we're quitting, leave a valid resolv.conf in place, not one
     * pointing to 127.0.0.1 if dnsmasq was active.  But if we haven't
     * done any DNS updates yet, there's no reason to touch resolv.conf
     * on shutdown.
     */
    if (priv->dns_touched && priv->caching_successful && priv->resolver_depends_on_nm) {
        gs_free_error GError *error = NULL;

        update_resolv_conf(NM_CAST_STRV_CC(priv->last_update.searches),
                           NM_CAST_STRV_CC(priv->last_update.nameservers),
                           NM_CAST_STRV_CC(priv->last_update.options),
                           &error,
                           NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE);

        if (error)
            _LOGW("could not commit DNS changes on shutdown: %s", error->message);

        priv->dns_touched = FALSE;
    }
}

static void
dispose(GObject *object)
{
    NMDnsResolvconfPrivate *priv = NM_DNS_RESOLVCONF_GET_PRIVATE(NM_DNS_RESOLVCONF(object));

    _LOGT("disposing of Resolvconf plugin");

    stop(NM_DNS_PLUGIN(object));

    g_strfreev(priv->last_update.nameservers);
    g_strfreev(priv->last_update.nis_servers);
    g_strfreev(priv->last_update.options);
    g_strfreev(priv->last_update.searches);
    g_free(priv->last_update.nis_domain);

    G_OBJECT_CLASS(nm_dns_resolvconf_parent_class)->dispose(object);
}

static inline gboolean
get_update_pending(NMDnsPlugin *plugin)
{
    return FALSE;
}

static void
nm_dns_resolvconf_class_init(NMDnsResolvconfClass *dns_class)
{
    NMDnsPluginClass *plugin_class = NM_DNS_PLUGIN_CLASS(dns_class);
    GObjectClass     *object_class = G_OBJECT_CLASS(dns_class);

    object_class->dispose = dispose;

    plugin_class->plugin_name        = "resolvconf";
    plugin_class->is_caching         = FALSE;
    plugin_class->stop               = stop;
    plugin_class->update             = update;
    plugin_class->get_update_pending = get_update_pending;
    plugin_class->checksum           = resolvconf_checksum;
}
