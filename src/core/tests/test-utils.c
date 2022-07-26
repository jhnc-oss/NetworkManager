/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include <arpa/inet.h>
#include <linux/if_ether.h>

#include "nm-test-utils-core.h"

static void
test_stable_privacy(void)
{
    struct in6_addr addr1;

    inet_pton(AF_INET6, "1234::", &addr1);
    nm_utils_ipv6_addr_set_stable_privacy_with_host_id(NM_UTILS_STABLE_TYPE_UUID,
                                                       &addr1,
                                                       "eth666",
                                                       "6b138152-9f3e-4b97-aaf7-e6e553f2a24e",
                                                       0,
                                                       (guint8 *) "key",
                                                       3);
    nmtst_assert_ip6_address(&addr1, "1234::4ceb:14cd:3d54:793f");

    /* We get an address without the UUID. */
    inet_pton(AF_INET6, "1::", &addr1);
    nm_utils_ipv6_addr_set_stable_privacy_with_host_id(NM_UTILS_STABLE_TYPE_UUID,
                                                       &addr1,
                                                       "eth666",
                                                       "",
                                                       384,
                                                       (guint8 *) "key",
                                                       3);
    nmtst_assert_ip6_address(&addr1, "1::11aa:2530:9144:dafa");

    /* We get a different address in a different network. */
    inet_pton(AF_INET6, "2::", &addr1);
    nm_utils_ipv6_addr_set_stable_privacy_with_host_id(NM_UTILS_STABLE_TYPE_UUID,
                                                       &addr1,
                                                       "eth666",
                                                       "",
                                                       384,
                                                       (guint8 *) "key",
                                                       3);
    nmtst_assert_ip6_address(&addr1, "2::338e:8d:c11:8726");

    inet_pton(AF_INET6, "1234::", &addr1);
    nm_utils_ipv6_addr_set_stable_privacy_with_host_id(NM_UTILS_STABLE_TYPE_STABLE_ID,
                                                       &addr1,
                                                       "eth666",
                                                       "6b138152-9f3e-4b97-aaf7-e6e553f2a24e",
                                                       0,
                                                       (guint8 *) "key",
                                                       3);
    nmtst_assert_ip6_address(&addr1, "1234::ad4c:ae44:3d30:af1e");

    inet_pton(AF_INET6, "1234::", &addr1);
    nm_utils_ipv6_addr_set_stable_privacy_with_host_id(NM_UTILS_STABLE_TYPE_STABLE_ID,
                                                       &addr1,
                                                       "eth666",
                                                       "stable-id-1",
                                                       0,
                                                       (guint8 *) "key",
                                                       3);
    nmtst_assert_ip6_address(&addr1, "1234::4944:67b0:7a6c:1cf");
}

/*****************************************************************************/

static void
_do_test_hw_addr(NMUtilsStableType  stable_type,
                 const char        *stable_id,
                 const guint8      *secret_key,
                 gsize              key_len,
                 const char        *ifname,
                 const char        *current_mac_address,
                 const char        *generate_mac_address_mask,
                 const char *const *expected)
{
    gs_free char      *generated = NULL;
    const char *const *e;
    gboolean           found = FALSE;

    for (e = expected; *e; e++) {
        g_assert(*e);
        g_assert(nm_utils_hwaddr_valid(*e, ETH_ALEN));
    }

    generated = nm_utils_hw_addr_gen_stable_eth_impl(stable_type,
                                                     stable_id,
                                                     secret_key,
                                                     key_len,
                                                     ifname,
                                                     current_mac_address,
                                                     generate_mac_address_mask);

    g_assert(generated);
    g_assert(nm_utils_hwaddr_valid(generated, ETH_ALEN));
    for (e = expected; *e; e++) {
        if (!nm_utils_hwaddr_matches(generated, -1, *e, -1))
            continue;
        g_assert(!found);
        found = TRUE;
        g_assert_cmpstr(generated, ==, *e);
    }
    g_assert(found);
}
#define do_test_hw_addr(stable_type,                    \
                        stable_id,                      \
                        secret_key,                     \
                        ifname,                         \
                        current_mac_address,            \
                        generate_mac_address_mask,      \
                        ...)                            \
    _do_test_hw_addr((stable_type),                     \
                     (stable_id),                       \
                     (const guint8 *) "" secret_key "", \
                     NM_STRLEN(secret_key),             \
                     (ifname),                          \
                     "" current_mac_address "",         \
                     generate_mac_address_mask,         \
                     NM_MAKE_STRV(__VA_ARGS__))

static void
test_hw_addr_gen_stable_eth(void)
{
    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    NULL,
                    "06:0D:CD:0C:9E:2C");
    do_test_hw_addr(NM_UTILS_STABLE_TYPE_STABLE_ID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    NULL,
                    "C6:AE:A9:9A:76:09");

    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "FF:FF:FF:00:00:00",
                    "00:23:45:0C:9E:2C");
    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "03:23:45:67:89:ab",
                    "FF:FF:FF:00:00:00",
                    "02:23:45:0C:9E:2C");

    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "00:00:00:00:00:00",
                    "06:0D:CD:0C:9E:2C");
    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "02:00:00:00:00:00",
                    "04:0D:CD:0C:9E:2C");
    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "02:00:00:00:00:00",
                    "04:0D:CD:0C:9E:2C");

    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "02:00:00:00:00:00 00:00:00:00:00:00",
                    "04:0D:CD:0C:9E:2C");
    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "02:00:00:00:00:00 02:00:00:00:00:00",
                    "06:0D:CD:0C:9E:2C");

    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "00:00:00:00:00:00 E9:60:CE:F5:ED:2F",
                    "06:0D:CD:0C:9E:2C");

    do_test_hw_addr(NM_UTILS_STABLE_TYPE_UUID,
                    "stable-1",
                    "key1",
                    "eth0",
                    "01:23:45:67:89:ab",
                    "02:00:00:00:00:00 00:00:00:00:00:00 02:00:00:00:00:00",
                    "06:0D:CD:0C:9E:2C",
                    "04:0D:CD:0C:9E:2C");
}

static void
test_shorten_hostname(void)
{
    gs_free char *maxhost = NULL;
    char         *hostname;

#define do_test_shorten_hostname(_host, _exp_res, _exp_short) \
    G_STMT_START                                              \
    {                                                         \
        gboolean      _res;                                   \
        gs_free char *_short = NULL;                          \
                                                              \
        _res = nm_utils_shorten_hostname((_host), &_short);   \
        g_assert_cmpint((_res), ==, (_exp_res));              \
        g_assert_cmpstr(_short, ==, (_exp_short));            \
    }                                                         \
    G_STMT_END

    /* 'maxhost' is the longest allowed hostname according to
     * system configuration (`getconf HOST_NAME_MAX`). On Linux
     * it's typically 64 characters, but POSIX allows up to
     * 255 characters.
     *
     * We use our own define NM_HOST_NAME_MAX, which is always 64.
     */
    maxhost = g_strnfill(NM_HOST_NAME_MAX, 'a');

    do_test_shorten_hostname("name1", TRUE, NULL);

    do_test_shorten_hostname("name1.example.com", TRUE, NULL);

    do_test_shorten_hostname(maxhost, TRUE, NULL);

    hostname = g_strdup_printf("%sbbb", maxhost);
    do_test_shorten_hostname(hostname, TRUE, maxhost);
    nm_clear_g_free(&hostname);

    hostname = g_strdup_printf("%s.com", maxhost);
    do_test_shorten_hostname(hostname, TRUE, maxhost);
    nm_clear_g_free(&hostname);

    hostname = g_strdup_printf("name1.%s.com", maxhost);
    do_test_shorten_hostname(hostname, TRUE, "name1");
    nm_clear_g_free(&hostname);

    do_test_shorten_hostname(".name1", FALSE, NULL);
}

/*****************************************************************************/

NMTST_DEFINE();

int
main(int argc, char **argv)
{
    nmtst_init_with_logging(&argc, &argv, NULL, "ALL");

    g_test_add_func("/utils/stable_privacy", test_stable_privacy);
    g_test_add_func("/utils/hw_addr_gen_stable_eth", test_hw_addr_gen_stable_eth);
    g_test_add_func("/utils/shorten-hostname", test_shorten_hostname);

    return g_test_run();
}
