/* SPDX-License-Identifier: LGPL-2.1+ */
/*
 * Copyright (C) 2015 Red Hat, Inc.
 */

#ifndef NM_CORE_TYPES_INTERNAL_H
#define NM_CORE_TYPES_INTERNAL_H

#if !((NETWORKMANAGER_COMPILATION) &NM_NETWORKMANAGER_COMPILATION_WITH_LIBNM_CORE_INTERNAL)
    #error Cannot use this header.
#endif

typedef struct {
    guint32 from;
    guint32 to;
} NMVlanQosMapping;

#define _NM_IP_TUNNEL_FLAG_ALL_IP6TNL                                              \
    (NM_IP_TUNNEL_FLAG_IP6_IGN_ENCAP_LIMIT | NM_IP_TUNNEL_FLAG_IP6_USE_ORIG_TCLASS \
     | NM_IP_TUNNEL_FLAG_IP6_USE_ORIG_FLOWLABEL | NM_IP_TUNNEL_FLAG_IP6_MIP6_DEV   \
     | NM_IP_TUNNEL_FLAG_IP6_RCV_DSCP_COPY | NM_IP_TUNNEL_FLAG_IP6_USE_ORIG_FWMARK)

#endif /* NM_CORE_TYPES_INTERNAL_H */