/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2005 - 2018 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "libnm-glib-aux/nm-default-glib-i18n-lib.h"

#include "nm-wifi-utils.h"

#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>

#include "nm-wifi-utils-private.h"
#include "nm-wifi-utils-nl80211.h"
#if HAVE_WEXT
#include "nm-wifi-utils-wext.h"
#endif
#include "libnm-platform/nm-platform-utils.h"

G_DEFINE_ABSTRACT_TYPE(NMWifiUtils, nm_wifi_utils, G_TYPE_OBJECT)

/*****************************************************************************/

static void
nm_wifi_utils_init(NMWifiUtils *self)
{}

static void
nm_wifi_utils_class_init(NMWifiUtilsClass *klass)
{}

NMWifiUtils *
nm_wifi_utils_new(struct nl_sock *genl, guint16 genl_family_id, int ifindex, gboolean check_scan)
{
    NMWifiUtils *ret;

    g_return_val_if_fail(ifindex > 0, NULL);

    ret = nm_wifi_utils_nl80211_new(genl, genl_family_id, ifindex);

#if HAVE_WEXT
    if (!ret)
        ret = nm_wifi_utils_wext_new(ifindex, check_scan);
#endif

    return ret;
}

_NMDeviceWifiCapabilities
nm_wifi_utils_get_caps(NMWifiUtils *data)
{
    g_return_val_if_fail(data != NULL, _NM_WIFI_DEVICE_CAP_NONE);

    return data->caps;
}

_NM80211Mode
nm_wifi_utils_get_mode(NMWifiUtils *data)
{
    g_return_val_if_fail(data != NULL, _NM_802_11_MODE_UNKNOWN);

    return NM_WIFI_UTILS_GET_CLASS(data)->get_mode(data);
}

gboolean
nm_wifi_utils_set_mode(NMWifiUtils *data, const _NM80211Mode mode)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);
    g_return_val_if_fail((mode == _NM_802_11_MODE_INFRA) || (mode == _NM_802_11_MODE_AP)
                             || (mode == _NM_802_11_MODE_ADHOC) || (mode == _NM_802_11_MODE_MESH),
                         FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);

    /* nl80211 probably doesn't need this */
    return klass->set_mode ? klass->set_mode(data, mode) : TRUE;
}

gboolean
nm_wifi_utils_set_powersave(NMWifiUtils *data, guint32 powersave)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);
    return klass->set_powersave ? klass->set_powersave(data, powersave) : TRUE;
}

_NMSettingWirelessWakeOnWLan
nm_wifi_utils_get_wake_on_wlan(NMWifiUtils *data)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, _NM_SETTING_WIRELESS_WAKE_ON_WLAN_IGNORE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);

    return klass->get_wake_on_wlan ? klass->get_wake_on_wlan(data)
                                   : _NM_SETTING_WIRELESS_WAKE_ON_WLAN_IGNORE;
}

gboolean
nm_wifi_utils_set_wake_on_wlan(NMWifiUtils *data, _NMSettingWirelessWakeOnWLan wowl)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);
    return klass->set_wake_on_wlan ? klass->set_wake_on_wlan(data, wowl) : FALSE;
}

guint32
nm_wifi_utils_get_freq(NMWifiUtils *data)
{
    g_return_val_if_fail(data != NULL, 0);

    return NM_WIFI_UTILS_GET_CLASS(data)->get_freq(data);
}

guint32
nm_wifi_utils_find_freq(NMWifiUtils *data, const guint32 *freqs, gboolean ap)
{
    g_return_val_if_fail(data != NULL, 0);
    g_return_val_if_fail(freqs != NULL, 0);

    return NM_WIFI_UTILS_GET_CLASS(data)->find_freq(data, freqs, ap);
}

gboolean
nm_wifi_utils_get_station(NMWifiUtils *data,
                          NMEtherAddr *out_bssid,
                          int         *out_quality,
                          guint32     *out_rate)
{
    g_return_val_if_fail(data != NULL, FALSE);

    return NM_WIFI_UTILS_GET_CLASS(data)->get_station(data, out_bssid, out_quality, out_rate);
}

gboolean
nm_wifi_utils_is_wifi(int dirfd, const char *ifname)
{
    g_return_val_if_fail(dirfd >= 0, FALSE);

    if (faccessat(dirfd, "phy80211", F_OK, 0) == 0)
        return TRUE;
#if HAVE_WEXT
    if (nm_wifi_utils_wext_is_wifi(ifname))
        return TRUE;
#endif
    return FALSE;
}

/* OLPC Mesh-only functions */

guint32
nm_wifi_utils_get_mesh_channel(NMWifiUtils *data)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);
    g_return_val_if_fail(klass->get_mesh_channel != NULL, FALSE);

    return klass->get_mesh_channel(data);
}

gboolean
nm_wifi_utils_set_mesh_channel(NMWifiUtils *data, guint32 channel)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);
    g_return_val_if_fail(channel <= 13, FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);
    g_return_val_if_fail(klass->set_mesh_channel != NULL, FALSE);

    return klass->set_mesh_channel(data, channel);
}

gboolean
nm_wifi_utils_set_mesh_ssid(NMWifiUtils *data, const guint8 *ssid, gsize len)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);
    g_return_val_if_fail(klass->set_mesh_ssid != NULL, FALSE);

    return klass->set_mesh_ssid(data, ssid, len);
}

gboolean
nm_wifi_utils_indicate_addressing_running(NMWifiUtils *data, gboolean running)
{
    NMWifiUtilsClass *klass;

    g_return_val_if_fail(data != NULL, FALSE);

    klass = NM_WIFI_UTILS_GET_CLASS(data);
    return klass->indicate_addressing_running ? klass->indicate_addressing_running(data, running)
                                              : FALSE;
}

/**
 * nm_wifi_utils_can_concurrent:
 * @data: The NMWifiUtils instance
 * @iftype1: First interface type (NL80211_IFTYPE_*)
 * @iftype2: Second interface type (NL80211_IFTYPE_*)
 * @out_num_channels: (out) (optional): Number of different channels allowed
 *
 * Check if two interface types can operate concurrently based on
 * the hardware's interface combination capabilities parsed from
 * NL80211_ATTR_INTERFACE_COMBINATIONS.
 *
 * The algorithm tries to find a valid allocation: place iftype1 in one Limit
 * and iftype2 in another Limit (or the same Limit if its max >= 2).
 *
 * Returns: %TRUE if the combination is allowed, %FALSE otherwise.
 */
gboolean
nm_wifi_utils_can_concurrent(NMWifiUtils    *data,
                             NMWifiIfaceType iftype1,
                             NMWifiIfaceType iftype2,
                             guint8         *out_num_channels)
{
    GArray *combs;
    guint   i, j, k;

    g_return_val_if_fail(data != NULL, FALSE);

    combs = data->iface_combinations;
    if (!combs || combs->len == 0)
        return FALSE;

    /* Check each combination to find a valid allocation for both interface types */
    for (i = 0; i < combs->len; i++) {
        NMWifiIfaceCombination *comb = &g_array_index(combs, NMWifiIfaceCombination, i);

        /* Quick check: we need at least 2 interfaces total for concurrent operation */
        if (comb->max_num < 2)
            continue;

        if (!comb->limits || comb->limits->len == 0)
            continue;

        /* Try to allocate iftype1 to limit[j] and iftype2 to limit[k].
         * They can be the same limit (j == k) or different limits (j != k).
         */
        for (j = 0; j < comb->limits->len; j++) {
            NMWifiIfaceCombLimit *limit_a = &g_array_index(comb->limits, NMWifiIfaceCombLimit, j);

            /* Check if limit_a supports iftype1 */
            if (!(limit_a->types & (1 << iftype1)))
                continue;

            for (k = 0; k < comb->limits->len; k++) {
                NMWifiIfaceCombLimit *limit_b =
                    &g_array_index(comb->limits, NMWifiIfaceCombLimit, k);

                /* Check if limit_b supports iftype2 */
                if (!(limit_b->types & (1 << iftype2)))
                    continue;

                /* Found two limits that support the requested types.
                 * Now verify the constraints.
                 */

                if (j == k) {
                    /* Case 1: Both types in the same limit.
                     * The limit must allow at least 2 interfaces.
                     */
                    if (limit_a->max >= 2) {
                        if (out_num_channels)
                            *out_num_channels = comb->num_channels;
                        return TRUE;
                    }
                } else {
                    /* Case 2: Types in different limits.
                     * Each limit only needs to support 1 interface (which is
                     * guaranteed since we're here), and we already verified
                     * comb->max_num >= 2.
                     */
                    if (out_num_channels)
                        *out_num_channels = comb->num_channels;
                    return TRUE;
                }
            }
        }
    }

    return FALSE;
}
