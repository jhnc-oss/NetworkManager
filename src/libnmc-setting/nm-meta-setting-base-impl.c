/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2017 - 2018 Red Hat, Inc.
 */

#define NM_WANT_NM_ARRAY_FIND_BSEARCH_INLINE

#include "libnm-glib-aux/nm-default-glib-i18n-lib.h"

#include "nm-meta-setting-base.h"

#if _NM_META_SETTING_BASE_IMPL_LIBNM
#include "libnm-core-impl/nm-default-libnm-core.h"
#endif

#include "nm-setting-6lowpan.h"
#include "nm-setting-8021x.h"
#include "nm-setting-adsl.h"
#include "nm-setting-bluetooth.h"
#include "nm-setting-bond.h"
#include "nm-setting-bond-port.h"
#include "nm-setting-bridge-port.h"
#include "nm-setting-bridge.h"
#include "nm-setting-cdma.h"
#include "nm-setting-connection.h"
#include "nm-setting-dcb.h"
#include "nm-setting-dummy.h"
#include "nm-setting-ethtool.h"
#include "nm-setting-generic.h"
#include "nm-setting-gsm.h"
#include "nm-setting-hostname.h"
#include "nm-setting-hsr.h"
#include "nm-setting-infiniband.h"
#include "nm-setting-ip-config.h"
#include "nm-setting-ip-tunnel.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-ip6-config.h"
#include "nm-setting-ipvlan.h"
#include "nm-setting-link.h"
#include "nm-setting-loopback.h"
#include "nm-setting-macsec.h"
#include "nm-setting-macvlan.h"
#include "nm-setting-match.h"
#include "nm-setting-olpc-mesh.h"
#include "nm-setting-ovs-bridge.h"
#include "nm-setting-ovs-dpdk.h"
#include "nm-setting-ovs-external-ids.h"
#include "nm-setting-ovs-interface.h"
#include "nm-setting-ovs-other-config.h"
#include "nm-setting-ovs-patch.h"
#include "nm-setting-ovs-port.h"
#include "nm-setting-ppp.h"
#include "nm-setting-pppoe.h"
#include "nm-setting-prefix-delegation.h"
#include "nm-setting-proxy.h"
#include "nm-setting-serial.h"
#include "nm-setting-tc-config.h"
#include "nm-setting-team-port.h"
#include "nm-setting-team.h"
#include "nm-setting-tun.h"
#include "nm-setting-user.h"
#include "nm-setting-veth.h"
#include "nm-setting-vlan.h"
#include "nm-setting-vpn.h"
#include "nm-setting-vrf.h"
#include "nm-setting-vxlan.h"
#include "nm-setting-wifi-p2p.h"
#include "nm-setting-wimax.h"
#include "nm-setting-wired.h"
#include "nm-setting-wireguard.h"
#include "nm-setting-wireless-security.h"
#include "nm-setting-wireless.h"
#include "nm-setting-wpan.h"

#if _NM_META_SETTING_BASE_IMPL_LIBNM
#include "nm-setting-private.h"
#endif

/*****************************************************************************/

const NMSetting8021xSchemeVtable nm_setting_8021x_scheme_vtable[] = {

#define _D(_scheme_type, ...) [(_scheme_type)] = {.scheme_type = (_scheme_type), __VA_ARGS__}

    _D(NM_SETTING_802_1X_SCHEME_TYPE_UNKNOWN),

    _D(NM_SETTING_802_1X_SCHEME_TYPE_CA_CERT,
       .setting_key   = NM_SETTING_802_1X_CA_CERT,
       .scheme_func   = nm_setting_802_1x_get_ca_cert_scheme,
       .format_func   = NULL,
       .path_func     = nm_setting_802_1x_get_ca_cert_path,
       .blob_func     = nm_setting_802_1x_get_ca_cert_blob,
       .uri_func      = nm_setting_802_1x_get_ca_cert_uri,
       .passwd_func   = nm_setting_802_1x_get_ca_cert_password,
       .pwflag_func   = nm_setting_802_1x_get_ca_cert_password_flags,
       .set_cert_func = nm_setting_802_1x_set_ca_cert,
       .file_suffix   = "ca-cert", ),

    _D(NM_SETTING_802_1X_SCHEME_TYPE_PHASE2_CA_CERT,
       .setting_key   = NM_SETTING_802_1X_PHASE2_CA_CERT,
       .scheme_func   = nm_setting_802_1x_get_phase2_ca_cert_scheme,
       .format_func   = NULL,
       .path_func     = nm_setting_802_1x_get_phase2_ca_cert_path,
       .blob_func     = nm_setting_802_1x_get_phase2_ca_cert_blob,
       .uri_func      = nm_setting_802_1x_get_phase2_ca_cert_uri,
       .passwd_func   = nm_setting_802_1x_get_phase2_ca_cert_password,
       .pwflag_func   = nm_setting_802_1x_get_phase2_ca_cert_password_flags,
       .set_cert_func = nm_setting_802_1x_set_phase2_ca_cert,
       .file_suffix   = "inner-ca-cert", ),

    _D(NM_SETTING_802_1X_SCHEME_TYPE_CLIENT_CERT,
       .setting_key   = NM_SETTING_802_1X_CLIENT_CERT,
       .scheme_func   = nm_setting_802_1x_get_client_cert_scheme,
       .format_func   = NULL,
       .path_func     = nm_setting_802_1x_get_client_cert_path,
       .blob_func     = nm_setting_802_1x_get_client_cert_blob,
       .uri_func      = nm_setting_802_1x_get_client_cert_uri,
       .passwd_func   = nm_setting_802_1x_get_client_cert_password,
       .pwflag_func   = nm_setting_802_1x_get_client_cert_password_flags,
       .set_cert_func = nm_setting_802_1x_set_client_cert,
       .file_suffix   = "client-cert", ),

    _D(NM_SETTING_802_1X_SCHEME_TYPE_PHASE2_CLIENT_CERT,
       .setting_key   = NM_SETTING_802_1X_PHASE2_CLIENT_CERT,
       .scheme_func   = nm_setting_802_1x_get_phase2_client_cert_scheme,
       .format_func   = NULL,
       .path_func     = nm_setting_802_1x_get_phase2_client_cert_path,
       .blob_func     = nm_setting_802_1x_get_phase2_client_cert_blob,
       .uri_func      = nm_setting_802_1x_get_phase2_client_cert_uri,
       .passwd_func   = nm_setting_802_1x_get_phase2_client_cert_password,
       .pwflag_func   = nm_setting_802_1x_get_phase2_client_cert_password_flags,
       .set_cert_func = nm_setting_802_1x_set_phase2_client_cert,
       .file_suffix   = "inner-client-cert", ),

    _D(NM_SETTING_802_1X_SCHEME_TYPE_PRIVATE_KEY,
       .setting_key          = NM_SETTING_802_1X_PRIVATE_KEY,
       .scheme_func          = nm_setting_802_1x_get_private_key_scheme,
       .format_func          = nm_setting_802_1x_get_private_key_format,
       .path_func            = nm_setting_802_1x_get_private_key_path,
       .blob_func            = nm_setting_802_1x_get_private_key_blob,
       .uri_func             = nm_setting_802_1x_get_private_key_uri,
       .passwd_func          = nm_setting_802_1x_get_private_key_password,
       .pwflag_func          = nm_setting_802_1x_get_private_key_password_flags,
       .set_private_key_func = nm_setting_802_1x_set_private_key,
       .file_suffix          = "private-key",
       .is_secret            = TRUE, ),

    _D(NM_SETTING_802_1X_SCHEME_TYPE_PHASE2_PRIVATE_KEY,
       .setting_key          = NM_SETTING_802_1X_PHASE2_PRIVATE_KEY,
       .scheme_func          = nm_setting_802_1x_get_phase2_private_key_scheme,
       .format_func          = nm_setting_802_1x_get_phase2_private_key_format,
       .path_func            = nm_setting_802_1x_get_phase2_private_key_path,
       .blob_func            = nm_setting_802_1x_get_phase2_private_key_blob,
       .uri_func             = nm_setting_802_1x_get_phase2_private_key_uri,
       .passwd_func          = nm_setting_802_1x_get_phase2_private_key_password,
       .pwflag_func          = nm_setting_802_1x_get_phase2_private_key_password_flags,
       .set_private_key_func = nm_setting_802_1x_set_phase2_private_key,
       .file_suffix          = "inner-private-key",
       .is_secret            = TRUE, ),

#undef _D
};

const NMSetting8021xSchemeVtable *
nm_setting_8021x_scheme_vtable_by_setting_key(const char *key)
{
    static const NMSetting8021xSchemeType sorted_index[] = {
        NM_SETTING_802_1X_SCHEME_TYPE_CA_CERT,
        NM_SETTING_802_1X_SCHEME_TYPE_CLIENT_CERT,
        NM_SETTING_802_1X_SCHEME_TYPE_PHASE2_CA_CERT,
        NM_SETTING_802_1X_SCHEME_TYPE_PHASE2_CLIENT_CERT,
        NM_SETTING_802_1X_SCHEME_TYPE_PHASE2_PRIVATE_KEY,
        NM_SETTING_802_1X_SCHEME_TYPE_PRIVATE_KEY,
    };
    int imin, imax;

    nm_assert(key);

    if (NM_MORE_ASSERT_ONCE(5)) {
        const NMSetting8021xSchemeVtable *vtable_prev = NULL;
        int                               i, j;

        for (i = 0; i < (int) G_N_ELEMENTS(sorted_index); i++) {
            const NMSetting8021xSchemeType    t = sorted_index[i];
            const NMSetting8021xSchemeVtable *vtable;

            nm_assert(_NM_INT_NOT_NEGATIVE(t));
            nm_assert(t < G_N_ELEMENTS(nm_setting_8021x_scheme_vtable) - 1);

            for (j = 0; j < i; j++)
                nm_assert(t != sorted_index[j]);

            vtable = &nm_setting_8021x_scheme_vtable[t];

            nm_assert(vtable->scheme_type == t);
            nm_assert(vtable->setting_key);

            if (vtable_prev)
                nm_assert(strcmp(vtable_prev->setting_key, vtable->setting_key) < 0);
            vtable_prev = vtable;
        }
    }

    imin = 0;
    imax = G_N_ELEMENTS(sorted_index) - 1;
    while (imin <= imax) {
        const NMSetting8021xSchemeVtable *vtable;
        const int                         imid = imin + (imax - imin) / 2;
        int                               cmp;

        vtable = &nm_setting_8021x_scheme_vtable[sorted_index[imid]];

        cmp = strcmp(vtable->setting_key, key);
        if (cmp == 0)
            return vtable;

        if (cmp < 0)
            imin = imid + 1;
        else
            imax = imid - 1;
    }

    return NULL;
}

/*****************************************************************************/

const NMMetaSettingInfo nm_meta_setting_infos[] = {
    [NM_META_SETTING_TYPE_6LOWPAN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_6LOWPAN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_6LOWPAN_SETTING_NAME,
            .get_setting_gtype = nm_setting_6lowpan_get_type,
        },
    [NM_META_SETTING_TYPE_802_1X] =
        {
            .meta_type         = NM_META_SETTING_TYPE_802_1X,
            .setting_priority  = NM_SETTING_PRIORITY_HW_AUX,
            .setting_name      = NM_SETTING_802_1X_SETTING_NAME,
            .get_setting_gtype = nm_setting_802_1x_get_type,
        },
    [NM_META_SETTING_TYPE_ADSL] =
        {
            .meta_type         = NM_META_SETTING_TYPE_ADSL,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_ADSL_SETTING_NAME,
            .get_setting_gtype = nm_setting_adsl_get_type,
        },
    [NM_META_SETTING_TYPE_BLUETOOTH] =
        {
            .meta_type         = NM_META_SETTING_TYPE_BLUETOOTH,
            .setting_priority  = NM_SETTING_PRIORITY_HW_NON_BASE,
            .setting_name      = NM_SETTING_BLUETOOTH_SETTING_NAME,
            .get_setting_gtype = nm_setting_bluetooth_get_type,
        },
    [NM_META_SETTING_TYPE_BOND] =
        {
            .meta_type         = NM_META_SETTING_TYPE_BOND,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_BOND_SETTING_NAME,
            .get_setting_gtype = nm_setting_bond_get_type,
        },
    [NM_META_SETTING_TYPE_BOND_PORT] =
        {
            .meta_type         = NM_META_SETTING_TYPE_BOND_PORT,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_BOND_PORT_SETTING_NAME,
            .get_setting_gtype = nm_setting_bond_port_get_type,
        },
    [NM_META_SETTING_TYPE_BRIDGE] =
        {
            .meta_type         = NM_META_SETTING_TYPE_BRIDGE,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_BRIDGE_SETTING_NAME,
            .get_setting_gtype = nm_setting_bridge_get_type,
        },
    [NM_META_SETTING_TYPE_BRIDGE_PORT] =
        {
            .meta_type         = NM_META_SETTING_TYPE_BRIDGE_PORT,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_BRIDGE_PORT_SETTING_NAME,
            .get_setting_gtype = nm_setting_bridge_port_get_type,
        },
    [NM_META_SETTING_TYPE_CDMA] =
        {
            .meta_type         = NM_META_SETTING_TYPE_CDMA,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_CDMA_SETTING_NAME,
            .get_setting_gtype = nm_setting_cdma_get_type,
        },
    [NM_META_SETTING_TYPE_CONNECTION] =
        {
            .meta_type         = NM_META_SETTING_TYPE_CONNECTION,
            .setting_priority  = NM_SETTING_PRIORITY_CONNECTION,
            .setting_name      = NM_SETTING_CONNECTION_SETTING_NAME,
            .get_setting_gtype = nm_setting_connection_get_type,
        },
    [NM_META_SETTING_TYPE_DCB] =
        {
            .meta_type         = NM_META_SETTING_TYPE_DCB,
            .setting_priority  = NM_SETTING_PRIORITY_HW_AUX,
            .setting_name      = NM_SETTING_DCB_SETTING_NAME,
            .get_setting_gtype = nm_setting_dcb_get_type,
        },
    [NM_META_SETTING_TYPE_DUMMY] =
        {
            .meta_type         = NM_META_SETTING_TYPE_DUMMY,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_DUMMY_SETTING_NAME,
            .get_setting_gtype = nm_setting_dummy_get_type,
        },
    [NM_META_SETTING_TYPE_ETHTOOL] =
        {
            .meta_type         = NM_META_SETTING_TYPE_ETHTOOL,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_ETHTOOL_SETTING_NAME,
            .get_setting_gtype = nm_setting_ethtool_get_type,
        },
    [NM_META_SETTING_TYPE_GENERIC] =
        {
            .meta_type         = NM_META_SETTING_TYPE_GENERIC,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_GENERIC_SETTING_NAME,
            .get_setting_gtype = nm_setting_generic_get_type,
        },
    [NM_META_SETTING_TYPE_GSM] =
        {
            .meta_type         = NM_META_SETTING_TYPE_GSM,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_GSM_SETTING_NAME,
            .get_setting_gtype = nm_setting_gsm_get_type,
        },
    [NM_META_SETTING_TYPE_HOSTNAME] =
        {
            .meta_type         = NM_META_SETTING_TYPE_HOSTNAME,
            .setting_priority  = NM_SETTING_PRIORITY_IP,
            .setting_name      = NM_SETTING_HOSTNAME_SETTING_NAME,
            .get_setting_gtype = nm_setting_hostname_get_type,
        },
    [NM_META_SETTING_TYPE_HSR] =
        {
            .meta_type         = NM_META_SETTING_TYPE_HSR,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_HSR_SETTING_NAME,
            .get_setting_gtype = nm_setting_hsr_get_type,
        },
    [NM_META_SETTING_TYPE_INFINIBAND] =
        {
            .meta_type         = NM_META_SETTING_TYPE_INFINIBAND,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_INFINIBAND_SETTING_NAME,
            .get_setting_gtype = nm_setting_infiniband_get_type,
        },
    [NM_META_SETTING_TYPE_IP4_CONFIG] =
        {
            .meta_type         = NM_META_SETTING_TYPE_IP4_CONFIG,
            .setting_priority  = NM_SETTING_PRIORITY_IP,
            .setting_name      = NM_SETTING_IP4_CONFIG_SETTING_NAME,
            .get_setting_gtype = nm_setting_ip4_config_get_type,
        },
    [NM_META_SETTING_TYPE_IP6_CONFIG] =
        {
            .meta_type         = NM_META_SETTING_TYPE_IP6_CONFIG,
            .setting_priority  = NM_SETTING_PRIORITY_IP,
            .setting_name      = NM_SETTING_IP6_CONFIG_SETTING_NAME,
            .get_setting_gtype = nm_setting_ip6_config_get_type,
        },
    [NM_META_SETTING_TYPE_IP_TUNNEL] =
        {
            .meta_type         = NM_META_SETTING_TYPE_IP_TUNNEL,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_IP_TUNNEL_SETTING_NAME,
            .get_setting_gtype = nm_setting_ip_tunnel_get_type,
        },
    [NM_META_SETTING_TYPE_IPVLAN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_IPVLAN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_IPVLAN_SETTING_NAME,
            .get_setting_gtype = nm_setting_ipvlan_get_type,
        },
    [NM_META_SETTING_TYPE_LINK] =
        {
            .meta_type         = NM_META_SETTING_TYPE_LINK,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_LINK_SETTING_NAME,
            .get_setting_gtype = nm_setting_link_get_type,
        },
    [NM_META_SETTING_TYPE_LOOPBACK] =
        {
            .meta_type         = NM_META_SETTING_TYPE_LOOPBACK,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_LOOPBACK_SETTING_NAME,
            .get_setting_gtype = nm_setting_loopback_get_type,
        },
    [NM_META_SETTING_TYPE_MACSEC] =
        {
            .meta_type         = NM_META_SETTING_TYPE_MACSEC,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_MACSEC_SETTING_NAME,
            .get_setting_gtype = nm_setting_macsec_get_type,
        },
    [NM_META_SETTING_TYPE_MACVLAN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_MACVLAN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_MACVLAN_SETTING_NAME,
            .get_setting_gtype = nm_setting_macvlan_get_type,
        },
    [NM_META_SETTING_TYPE_MATCH] =
        {
            .meta_type         = NM_META_SETTING_TYPE_MATCH,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_MATCH_SETTING_NAME,
            .get_setting_gtype = nm_setting_match_get_type,
        },
    [NM_META_SETTING_TYPE_OLPC_MESH] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OLPC_MESH,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_OLPC_MESH_SETTING_NAME,
            .get_setting_gtype = nm_setting_olpc_mesh_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_BRIDGE] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_BRIDGE,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_OVS_BRIDGE_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_bridge_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_DPDK] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_DPDK,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_OVS_DPDK_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_dpdk_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_OTHER_CONFIG] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_OTHER_CONFIG,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_OVS_OTHER_CONFIG_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_other_config_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_EXTERNAL_IDS] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_EXTERNAL_IDS,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_OVS_EXTERNAL_IDS_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_external_ids_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_INTERFACE] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_INTERFACE,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_OVS_INTERFACE_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_interface_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_PATCH] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_PATCH,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_OVS_PATCH_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_patch_get_type,
        },
    [NM_META_SETTING_TYPE_OVS_PORT] =
        {
            .meta_type         = NM_META_SETTING_TYPE_OVS_PORT,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_OVS_PORT_SETTING_NAME,
            .get_setting_gtype = nm_setting_ovs_port_get_type,
        },
    [NM_META_SETTING_TYPE_PPPOE] =
        {
            .meta_type         = NM_META_SETTING_TYPE_PPPOE,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_PPPOE_SETTING_NAME,
            .get_setting_gtype = nm_setting_pppoe_get_type,
        },
    [NM_META_SETTING_TYPE_PPP] =
        {
            .meta_type         = NM_META_SETTING_TYPE_PPP,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_PPP_SETTING_NAME,
            .get_setting_gtype = nm_setting_ppp_get_type,
        },
    [NM_META_SETTING_TYPE_PREFIX_DELEGATION] =
        {
            .meta_type         = NM_META_SETTING_TYPE_PREFIX_DELEGATION,
            .setting_priority  = NM_SETTING_PRIORITY_IP,
            .setting_name      = NM_SETTING_PREFIX_DELEGATION_SETTING_NAME,
            .get_setting_gtype = nm_setting_prefix_delegation_get_type,
        },
    [NM_META_SETTING_TYPE_PROXY] =
        {
            .meta_type         = NM_META_SETTING_TYPE_PROXY,
            .setting_priority  = NM_SETTING_PRIORITY_IP,
            .setting_name      = NM_SETTING_PROXY_SETTING_NAME,
            .get_setting_gtype = nm_setting_proxy_get_type,
        },
    [NM_META_SETTING_TYPE_SERIAL] =
        {
            .meta_type         = NM_META_SETTING_TYPE_SERIAL,
            .setting_priority  = NM_SETTING_PRIORITY_HW_AUX,
            .setting_name      = NM_SETTING_SERIAL_SETTING_NAME,
            .get_setting_gtype = nm_setting_serial_get_type,
        },
    [NM_META_SETTING_TYPE_SRIOV] =
        {
            .meta_type         = NM_META_SETTING_TYPE_SRIOV,
            .setting_priority  = NM_SETTING_PRIORITY_HW_AUX,
            .setting_name      = NM_SETTING_SRIOV_SETTING_NAME,
            .get_setting_gtype = nm_setting_sriov_get_type,
        },
    [NM_META_SETTING_TYPE_TC_CONFIG] =
        {
            .meta_type         = NM_META_SETTING_TYPE_TC_CONFIG,
            .setting_priority  = NM_SETTING_PRIORITY_IP,
            .setting_name      = NM_SETTING_TC_CONFIG_SETTING_NAME,
            .get_setting_gtype = nm_setting_tc_config_get_type,
        },
    [NM_META_SETTING_TYPE_TEAM] =
        {
            .meta_type         = NM_META_SETTING_TYPE_TEAM,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_TEAM_SETTING_NAME,
            .get_setting_gtype = nm_setting_team_get_type,
        },
    [NM_META_SETTING_TYPE_TEAM_PORT] =
        {
            .meta_type         = NM_META_SETTING_TYPE_TEAM_PORT,
            .setting_priority  = NM_SETTING_PRIORITY_AUX,
            .setting_name      = NM_SETTING_TEAM_PORT_SETTING_NAME,
            .get_setting_gtype = nm_setting_team_port_get_type,
        },
    [NM_META_SETTING_TYPE_TUN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_TUN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_TUN_SETTING_NAME,
            .get_setting_gtype = nm_setting_tun_get_type,
        },
    [NM_META_SETTING_TYPE_USER] =
        {
            .meta_type         = NM_META_SETTING_TYPE_USER,
            .setting_priority  = NM_SETTING_PRIORITY_USER,
            .setting_name      = NM_SETTING_USER_SETTING_NAME,
            .get_setting_gtype = nm_setting_user_get_type,
        },
    [NM_META_SETTING_TYPE_VETH] =
        {
            .meta_type         = NM_META_SETTING_TYPE_VETH,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_VETH_SETTING_NAME,
            .get_setting_gtype = nm_setting_veth_get_type,
        },
    [NM_META_SETTING_TYPE_VLAN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_VLAN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_VLAN_SETTING_NAME,
            .get_setting_gtype = nm_setting_vlan_get_type,
        },
    [NM_META_SETTING_TYPE_VPN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_VPN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_VPN_SETTING_NAME,
            .get_setting_gtype = nm_setting_vpn_get_type,
        },
    [NM_META_SETTING_TYPE_VRF] =
        {
            .meta_type         = NM_META_SETTING_TYPE_VRF,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_VRF_SETTING_NAME,
            .get_setting_gtype = nm_setting_vrf_get_type,
        },
    [NM_META_SETTING_TYPE_VXLAN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_VXLAN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_VXLAN_SETTING_NAME,
            .get_setting_gtype = nm_setting_vxlan_get_type,
        },
    [NM_META_SETTING_TYPE_WIFI_P2P] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WIFI_P2P,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_WIFI_P2P_SETTING_NAME,
            .get_setting_gtype = nm_setting_wifi_p2p_get_type,
        },
    [NM_META_SETTING_TYPE_WIMAX] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WIMAX,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_WIMAX_SETTING_NAME,
            .get_setting_gtype = nm_setting_wimax_get_type,
        },
    [NM_META_SETTING_TYPE_WIRED] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WIRED,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_WIRED_SETTING_NAME,
            .get_setting_gtype = nm_setting_wired_get_type,
        },
    [NM_META_SETTING_TYPE_WIREGUARD] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WIREGUARD,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_WIREGUARD_SETTING_NAME,
            .get_setting_gtype = nm_setting_wireguard_get_type,
        },
    [NM_META_SETTING_TYPE_WIRELESS] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WIRELESS,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_WIRELESS_SETTING_NAME,
            .get_setting_gtype = nm_setting_wireless_get_type,
        },
    [NM_META_SETTING_TYPE_WIRELESS_SECURITY] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WIRELESS_SECURITY,
            .setting_priority  = NM_SETTING_PRIORITY_HW_AUX,
            .setting_name      = NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
            .get_setting_gtype = nm_setting_wireless_security_get_type,
        },
    [NM_META_SETTING_TYPE_WPAN] =
        {
            .meta_type         = NM_META_SETTING_TYPE_WPAN,
            .setting_priority  = NM_SETTING_PRIORITY_HW_BASE,
            .setting_name      = NM_SETTING_WPAN_SETTING_NAME,
            .get_setting_gtype = nm_setting_wpan_get_type,
        },

    [NM_META_SETTING_TYPE_UNKNOWN] =
        {
            .meta_type = NM_META_SETTING_TYPE_UNKNOWN,
        },
};

const NMMetaSettingType nm_meta_setting_types_by_priority[] = {

    /* NM_SETTING_PRIORITY_CONNECTION */
    NM_META_SETTING_TYPE_CONNECTION,

    /* NM_SETTING_PRIORITY_HW_BASE */
    NM_META_SETTING_TYPE_6LOWPAN,
    NM_META_SETTING_TYPE_OLPC_MESH,
    NM_META_SETTING_TYPE_WIRELESS,
    NM_META_SETTING_TYPE_WIRED,
    NM_META_SETTING_TYPE_ADSL,
    NM_META_SETTING_TYPE_BOND,
    NM_META_SETTING_TYPE_BRIDGE,
    NM_META_SETTING_TYPE_CDMA,
    NM_META_SETTING_TYPE_DUMMY,
    NM_META_SETTING_TYPE_GENERIC,
    NM_META_SETTING_TYPE_GSM,
    NM_META_SETTING_TYPE_HSR,
    NM_META_SETTING_TYPE_INFINIBAND,
    NM_META_SETTING_TYPE_IP_TUNNEL,
    NM_META_SETTING_TYPE_IPVLAN,
    NM_META_SETTING_TYPE_LOOPBACK,
    NM_META_SETTING_TYPE_MACSEC,
    NM_META_SETTING_TYPE_MACVLAN,
    NM_META_SETTING_TYPE_OVS_BRIDGE,
    NM_META_SETTING_TYPE_OVS_INTERFACE,
    NM_META_SETTING_TYPE_OVS_PORT,
    NM_META_SETTING_TYPE_TEAM,
    NM_META_SETTING_TYPE_TUN,
    NM_META_SETTING_TYPE_VETH,
    NM_META_SETTING_TYPE_VLAN,
    NM_META_SETTING_TYPE_VPN,
    NM_META_SETTING_TYPE_VRF,
    NM_META_SETTING_TYPE_VXLAN,
    NM_META_SETTING_TYPE_WIFI_P2P,
    NM_META_SETTING_TYPE_WIMAX,
    NM_META_SETTING_TYPE_WIREGUARD,
    NM_META_SETTING_TYPE_WPAN,

    /* NM_SETTING_PRIORITY_HW_NON_BASE */
    NM_META_SETTING_TYPE_BLUETOOTH,

    /* NM_SETTING_PRIORITY_HW_AUX */
    NM_META_SETTING_TYPE_WIRELESS_SECURITY,
    NM_META_SETTING_TYPE_802_1X,
    NM_META_SETTING_TYPE_DCB,
    NM_META_SETTING_TYPE_SERIAL,
    NM_META_SETTING_TYPE_SRIOV,

    /* NM_SETTING_PRIORITY_AUX  */
    NM_META_SETTING_TYPE_BOND_PORT,
    NM_META_SETTING_TYPE_BRIDGE_PORT,
    NM_META_SETTING_TYPE_ETHTOOL,
    NM_META_SETTING_TYPE_LINK,
    NM_META_SETTING_TYPE_MATCH,
    NM_META_SETTING_TYPE_OVS_DPDK,
    NM_META_SETTING_TYPE_OVS_EXTERNAL_IDS,
    NM_META_SETTING_TYPE_OVS_OTHER_CONFIG,
    NM_META_SETTING_TYPE_OVS_PATCH,
    NM_META_SETTING_TYPE_PPP,
    NM_META_SETTING_TYPE_PPPOE,
    NM_META_SETTING_TYPE_TEAM_PORT,

    /* NM_SETTING_PRIORITY_IP */
    NM_META_SETTING_TYPE_HOSTNAME,
    NM_META_SETTING_TYPE_IP4_CONFIG,
    NM_META_SETTING_TYPE_IP6_CONFIG,
    NM_META_SETTING_TYPE_PREFIX_DELEGATION,
    NM_META_SETTING_TYPE_PROXY,
    NM_META_SETTING_TYPE_TC_CONFIG,

    /* NM_SETTING_PRIORITY_USER */
    NM_META_SETTING_TYPE_USER,
};

const NMMetaSettingInfo *
nm_meta_setting_infos_by_name(const char *name)
{
    gssize idx;

    if (NM_MORE_ASSERTS > 10) {
        guint i, j;

        for (i = 0; i < _NM_META_SETTING_TYPE_NUM; i++) {
            const NMMetaSettingInfo *setting_info = &nm_meta_setting_infos[i];

            nm_assert(setting_info->meta_type == (NMMetaSettingType) i);
            nm_assert(setting_info->setting_name);
            nm_assert(setting_info->setting_name[0]);
            nm_assert(setting_info->get_setting_gtype);
            nm_assert(setting_info->setting_priority != NM_SETTING_PRIORITY_INVALID);
            if (i > 0
                && strcmp(nm_meta_setting_infos[i - 1].setting_name, setting_info->setting_name)
                       >= 0) {
                g_error("nm_meta_setting_infos[%u, \"%s\"] is wrongly sorted before "
                        "nm_meta_setting_infos[%u, \"%s\"]. Rearange NMMetaSettingType enum",
                        i - 1,
                        nm_meta_setting_infos[i - 1].setting_name,
                        i,
                        setting_info->setting_name);
            }
            for (j = 0; j < i; j++) {
                const NMMetaSettingInfo *s = &nm_meta_setting_infos[j];

                nm_assert(setting_info->get_setting_gtype != s->get_setting_gtype);
            }
        }
    }

    G_STATIC_ASSERT_EXPR(G_STRUCT_OFFSET(NMMetaSettingInfo, setting_name) == 0);
    idx = nm_array_find_bsearch(nm_meta_setting_infos,
                                _NM_META_SETTING_TYPE_NUM,
                                sizeof(NMMetaSettingInfo),
                                &name,
                                nm_strcmp_p_with_data,
                                NULL);

    return idx >= 0 ? &nm_meta_setting_infos[idx] : NULL;
}

/*****************************************************************************/

#if _NM_META_SETTING_BASE_IMPL_LIBNM
static const NMMetaSettingInfo *
_infos_by_gtype_from_class(GType gtype)
{
    nm_auto_unref_gtypeclass GTypeClass *gtypeclass_unref = NULL;
    GTypeClass                          *gtypeclass;
    NMSettingClass                      *klass;

    if (!g_type_is_a(gtype, NM_TYPE_SETTING))
        return NULL;

    gtypeclass = g_type_class_peek(gtype);
    if (G_UNLIKELY(!gtypeclass))
        gtypeclass = gtypeclass_unref = g_type_class_ref(gtype);

    nm_assert(NM_IS_SETTING_CLASS(gtypeclass));

    klass = (NMSettingClass *) gtypeclass;

    if (!klass->setting_info)
        return NULL;

    nm_assert(klass->setting_info->get_setting_gtype);
    nm_assert(klass->setting_info->get_setting_gtype() == gtype);
    return klass->setting_info;
}
#endif

static const NMMetaSettingInfo *
_infos_by_gtype_search(GType gtype)
{
    int i;

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        if (nm_meta_setting_infos[i].get_setting_gtype() == gtype)
            return &nm_meta_setting_infos[i];
    }
    return NULL;
}

typedef struct {
    GType                    gtype;
    const NMMetaSettingInfo *setting_info;
} LookupData;

_nm_always_inline static inline int
_lookup_data_cmp(gconstpointer ptr_a, gconstpointer ptr_b, gpointer user_data)
{
    const GType *const a = ptr_a;
    const GType *const b = ptr_b;

    nm_assert(a);
    nm_assert(b);
    nm_assert(a != b);

    NM_CMP_DIRECT(*a, *b);
    return 0;
}

static const NMMetaSettingInfo *
_infos_by_gtype_binary_search(GType gtype)
{
    static LookupData        static_array[_NM_META_SETTING_TYPE_NUM];
    static const LookupData *static_ptr = NULL;
    const LookupData        *ptr;
    gssize                   idx;

again:
    ptr = g_atomic_pointer_get(&static_ptr);
    if (G_UNLIKELY(!ptr)) {
        static gsize g_lock = 0;
        int          i;

        if (!g_once_init_enter(&g_lock))
            goto again;

        for (i = 0; i < _NM_META_SETTING_TYPE_NUM; i++) {
            const NMMetaSettingInfo *m = &nm_meta_setting_infos[i];

            static_array[i] = (LookupData) {
                .gtype        = m->get_setting_gtype(),
                .setting_info = m,
            };
        }

        g_qsort_with_data(static_array,
                          _NM_META_SETTING_TYPE_NUM,
                          sizeof(static_array[0]),
                          _lookup_data_cmp,
                          NULL);

        ptr = static_array;
        g_atomic_pointer_set(&static_ptr, ptr);

        g_once_init_leave(&g_lock, 1);
    }

    idx = nm_array_find_bsearch_inline(ptr,
                                       _NM_META_SETTING_TYPE_NUM,
                                       sizeof(ptr[0]),
                                       &gtype,
                                       _lookup_data_cmp,
                                       NULL);
    if (idx < 0)
        return NULL;

    return ptr[idx].setting_info;
}

const NMMetaSettingInfo *
nm_meta_setting_infos_by_gtype(GType gtype)
{
    const NMMetaSettingInfo *setting_info;

#if _NM_META_SETTING_BASE_IMPL_LIBNM
    setting_info = _infos_by_gtype_from_class(gtype);
#else
    setting_info = _infos_by_gtype_binary_search(gtype);
#endif

    if (NM_MORE_ASSERTS > 20) {
        nm_assert(setting_info == _infos_by_gtype_search(gtype));
        nm_assert(setting_info == _infos_by_gtype_binary_search(gtype));
    }

    return setting_info;
}

/*****************************************************************************/

NMSettingPriority
nm_meta_setting_info_get_base_type_priority(const NMMetaSettingInfo *setting_info, GType gtype)
{
    /* Historical oddity: PPPoE is a base-type even though it's not
     * priority 1.  It needs to be sorted *after* lower-level stuff like
     * Wi-Fi security or 802.1x for secrets, but it's still allowed as a
     * base type.
     */

    if (setting_info) {
        if (NM_IN_SET(setting_info->setting_priority,
                      NM_SETTING_PRIORITY_HW_BASE,
                      NM_SETTING_PRIORITY_HW_NON_BASE)
            || gtype == NM_TYPE_SETTING_PPPOE)
            return setting_info->setting_priority;
    }

    return NM_SETTING_PRIORITY_INVALID;
}

NMSettingPriority
_nm_setting_type_get_base_type_priority(GType type)
{
    return nm_meta_setting_info_get_base_type_priority(nm_meta_setting_infos_by_gtype(type), type);
}

/*****************************************************************************/
