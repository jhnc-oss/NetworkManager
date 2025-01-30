/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2015 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"
#include "src/core/nm-netns.h"

#include <arpa/inet.h>
#include <linux/if_ether.h>

#include "nm-test-utils-core.h"

static void
test_clat_ip_reserve(void)
{
    NMPlatform            *platform;
    NMNetns               *netns;
    NMNetnsSharedIPHandle *handle[8];

    nm_linux_platform_setup();
    platform = g_object_ref(NM_PLATFORM_GET);
    netns    = nm_netns_new(platform);

    handle[0] = nm_netns_clat_ip_reserve(netns);
    nmtst_assert_ip4_address(handle[0]->addr, "192.0.0.0");
    g_assert(handle[0]->_ref_count == 1);
    nm_netns_clat_ip_release(handle[0]);

    /* Make sure we get the same one back if we release it */
    handle[0] = nm_netns_clat_ip_reserve(netns);
    nmtst_assert_ip4_address(handle[0]->addr, "192.0.0.0");
    g_assert(handle[0]->_ref_count == 1);

    for (int i = 1; i <= 7; ++i) {
        char buf[100];
        handle[i] = nm_netns_clat_ip_reserve(netns);
        g_snprintf(buf, sizeof(buf), "192.0.0.%d", i);
        nmtst_assert_ip4_address(handle[i]->addr, buf);
        g_assert(handle[i]->_ref_count == 1);
    }

    /* Now, we should wrap: */
    handle[7] = nm_netns_clat_ip_reserve(netns);
    nmtst_assert_ip4_address(handle[7]->addr, "192.0.0.7");
    g_assert(handle[7]->_ref_count == 2);
    nm_netns_clat_ip_release(handle[7]);
    g_assert(handle[7]->_ref_count == 1);
    nm_netns_clat_ip_release(handle[7]);

    for (int i = 0; i < 7; ++i) {
        nm_netns_clat_ip_release(handle[i]);
    }

    g_object_unref(netns);
    g_object_unref(platform);
}

/*****************************************************************************/

NMTST_DEFINE();

int
main(int argc, char **argv)
{
    nmtst_init_with_logging(&argc, &argv, NULL, "ALL");

    g_test_add_func("/netns/clat_ip_reserve", test_clat_ip_reserve);

    return g_test_run();
}
