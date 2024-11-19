/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __NM_LLDP_H__
#define __NM_LLDP_H__

/* IEEE 802.1AB-2009 Clause 8: TLV Types */
enum {
    NM_LLDP_TYPE_END                 = 0,
    NM_LLDP_TYPE_CHASSIS_ID          = 1,
    NM_LLDP_TYPE_PORT_ID             = 2,
    NM_LLDP_TYPE_TTL                 = 3,
    NM_LLDP_TYPE_PORT_DESCRIPTION    = 4,
    NM_LLDP_TYPE_SYSTEM_NAME         = 5,
    NM_LLDP_TYPE_SYSTEM_DESCRIPTION  = 6,
    NM_LLDP_TYPE_SYSTEM_CAPABILITIES = 7,
    NM_LLDP_TYPE_MGMT_ADDRESS        = 8,
    NM_LLDP_TYPE_PRIVATE             = 127
};

/* IEEE 802.1AB-2009 Clause 8.5.2: Chassis subtypes */
enum {
    NM_LLDP_CHASSIS_SUBTYPE_RESERVED          = 0,
    NM_LLDP_CHASSIS_SUBTYPE_CHASSIS_COMPONENT = 1,
    NM_LLDP_CHASSIS_SUBTYPE_INTERFACE_ALIAS   = 2,
    NM_LLDP_CHASSIS_SUBTYPE_PORT_COMPONENT    = 3,
    NM_LLDP_CHASSIS_SUBTYPE_MAC_ADDRESS       = 4,
    NM_LLDP_CHASSIS_SUBTYPE_NETWORK_ADDRESS   = 5,
    NM_LLDP_CHASSIS_SUBTYPE_INTERFACE_NAME    = 6,
    NM_LLDP_CHASSIS_SUBTYPE_LOCALLY_ASSIGNED  = 7
};

/* IEEE 802.1AB-2009 Clause 8.5.3: Port subtype */
enum {
    NM_LLDP_PORT_SUBTYPE_RESERVED         = 0,
    NM_LLDP_PORT_SUBTYPE_INTERFACE_ALIAS  = 1,
    NM_LLDP_PORT_SUBTYPE_PORT_COMPONENT   = 2,
    NM_LLDP_PORT_SUBTYPE_MAC_ADDRESS      = 3,
    NM_LLDP_PORT_SUBTYPE_NETWORK_ADDRESS  = 4,
    NM_LLDP_PORT_SUBTYPE_INTERFACE_NAME   = 5,
    NM_LLDP_PORT_SUBTYPE_AGENT_CIRCUIT_ID = 6,
    NM_LLDP_PORT_SUBTYPE_LOCALLY_ASSIGNED = 7
};

/* IEEE 802.1AB-2009 Clause 8.5.8: System capabilities */
enum {
    NM_LLDP_SYSTEM_CAPABILITIES_OTHER    = 1 << 0,
    NM_LLDP_SYSTEM_CAPABILITIES_REPEATER = 1 << 1,
    NM_LLDP_SYSTEM_CAPABILITIES_BRIDGE   = 1 << 2,
    NM_LLDP_SYSTEM_CAPABILITIES_WLAN_AP  = 1 << 3,
    NM_LLDP_SYSTEM_CAPABILITIES_ROUTER   = 1 << 4,
    NM_LLDP_SYSTEM_CAPABILITIES_PHONE    = 1 << 5,
    NM_LLDP_SYSTEM_CAPABILITIES_DOCSIS   = 1 << 6,
    NM_LLDP_SYSTEM_CAPABILITIES_STATION  = 1 << 7,
    NM_LLDP_SYSTEM_CAPABILITIES_CVLAN    = 1 << 8,
    NM_LLDP_SYSTEM_CAPABILITIES_SVLAN    = 1 << 9,
    NM_LLDP_SYSTEM_CAPABILITIES_TPMR     = 1 << 10
};

#define NM_LLDP_SYSTEM_CAPABILITIES_ALL UINT16_MAX

#define NM_LLDP_SYSTEM_CAPABILITIES_ALL_ROUTERS                                             \
    ((uint16_t) (NM_LLDP_SYSTEM_CAPABILITIES_REPEATER | NM_LLDP_SYSTEM_CAPABILITIES_BRIDGE  \
                 | NM_LLDP_SYSTEM_CAPABILITIES_WLAN_AP | NM_LLDP_SYSTEM_CAPABILITIES_ROUTER \
                 | NM_LLDP_SYSTEM_CAPABILITIES_DOCSIS | NM_LLDP_SYSTEM_CAPABILITIES_CVLAN   \
                 | NM_LLDP_SYSTEM_CAPABILITIES_SVLAN | NM_LLDP_SYSTEM_CAPABILITIES_TPMR))

#define NM_LLDP_OUI_802_1 (const uint8_t[]){0x00, 0x80, 0xc2}
#define NM_LLDP_OUI_802_3 (const uint8_t[]){0x00, 0x12, 0x0f}

#define _SD_LLDP_OUI_IANA 0x00, 0x00, 0x5E
#define NM_LLDP_OUI_IANA  \
    (const uint8_t[])     \
    {                     \
        _SD_LLDP_OUI_IANA \
    }

#define NM_LLDP_OUI_IANA_SUBTYPE_MUD 0x01
#define NM_LLDP_OUI_IANA_MUD                            \
    (const uint8_t[])                                   \
    {                                                   \
        _SD_LLDP_OUI_IANA, NM_LLDP_OUI_IANA_SUBTYPE_MUD \
    }

/* IEEE 802.1AB-2009 Annex E */
enum {
    NM_LLDP_OUI_802_1_SUBTYPE_PORT_VLAN_ID          = 1,
    NM_LLDP_OUI_802_1_SUBTYPE_PORT_PROTOCOL_VLAN_ID = 2,
    NM_LLDP_OUI_802_1_SUBTYPE_VLAN_NAME             = 3,
    NM_LLDP_OUI_802_1_SUBTYPE_PROTOCOL_IDENTITY     = 4,
    NM_LLDP_OUI_802_1_SUBTYPE_VID_USAGE_DIGEST      = 5,
    NM_LLDP_OUI_802_1_SUBTYPE_MANAGEMENT_VID        = 6,
    NM_LLDP_OUI_802_1_SUBTYPE_LINK_AGGREGATION      = 7
};

/* IEEE 802.1AB-2009 Annex F */
enum {
    NM_LLDP_OUI_802_3_SUBTYPE_MAC_PHY_CONFIG_STATUS = 1,
    NM_LLDP_OUI_802_3_SUBTYPE_POWER_VIA_MDI         = 2,
    NM_LLDP_OUI_802_3_SUBTYPE_LINK_AGGREGATION      = 3,
    NM_LLDP_OUI_802_3_SUBTYPE_MAXIMUM_FRAME_SIZE    = 4
};

#endif /* __NM_LLDP_H__ */