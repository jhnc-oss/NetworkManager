/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2025 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_DNS_RESOLVCONF_H__
#define __NETWORKMANAGER_DNS_RESOLVCONF_H__

#include "nm-dns-plugin.h"
#include "nm-dns-manager.h"

#define NM_TYPE_DNS_RESOLVCONF (nm_dns_resolvconf_get_type())
#define NM_DNS_RESOLVCONF(obj) \
    (_NM_G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_DNS_RESOLVCONF, NMDnsResolvconf))
#define NM_DNS_RESOLVCONF_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_DNS_RESOLVCONF, NMDnsResolvconfClass))
#define NM_IS_DNS_RESOLVCONF(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_DNS_RESOLVCONF))
#define NM_IS_DNS_RESOLVCONF_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_DNS_RESOLVCONF))
#define NM_DNS_RESOLVCONF_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_DNS_RESOLVCONF, NMDnsResolvconfClass))

typedef struct _NMDnsResolvconf      NMDnsResolvconf;
typedef struct _NMDnsResolvconfClass NMDnsResolvconfClass;

GType nm_dns_resolvconf_get_type(void);

NMDnsPlugin *nm_dns_resolvconf_new(void);

#endif /* __NETWORKMANAGER_DNS_RESOLVCONF_H__ */
