/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2004 - 2005 Colin Walters <walters@redhat.com>
 * Copyright (C) 2004 - 2013 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#ifndef __NETWORKMANAGER_DNS_MANAGER_H__
#define __NETWORKMANAGER_DNS_MANAGER_H__

#include "c-list/src/c-list.h"
#include "nm-setting-connection.h"
#include "nm-dns-plugin.h"

struct _NMDnsConfigData;
struct _NMDnsManager;

typedef struct {
    struct _NMDnsConfigData *data;
    gconstpointer            source_tag;
    const NML3ConfigData    *l3cd;
    CList                    data_lst;
    CList                    ip_data_lst;
    NMDnsIPConfigType        ip_config_type;
    int                      addr_family;
    struct {
        const char **search;
        char       **reverse;

        /* Whether "search" explicitly contains a default route "~"
         * or "". It is redundant information, but for faster lookup. */
        bool has_default_route_explicit : 1;

        /* Whether an explicit "~" search domain should be added.
         * For systemd-resolved, this configured an explicit wildcard
         * search domain, and should be used for profiles with negative
         * DNS priority.
         *
         * If "has_default_route_explicit", this is always TRUE and implied.
         *
         * With systemd-resolved, if TRUE we will set a "." search domain.
         */
        bool has_default_route_exclusive : 1;

        /* Whether the device should be used for any domains "~".
         *
         * If "has_default_route_exclusive", this is always TRUE and implied.
         *
         * With systemd-resolved, this is the value for SetLinkDefaultRoute(). */
        bool has_default_route : 1;
    } domains;
} NMDnsConfigIPData;

typedef struct _NMDnsConfigData {
    int                   ifindex;
    struct _NMDnsManager *self;
    CList                 data_lst_head;
    CList                 configs_lst;
} NMDnsConfigData;

/*****************************************************************************/

#define NM_TYPE_DNS_MANAGER (nm_dns_manager_get_type())
#define NM_DNS_MANAGER(o)   (_NM_G_TYPE_CHECK_INSTANCE_CAST((o), NM_TYPE_DNS_MANAGER, NMDnsManager))
#define NM_DNS_MANAGER_CLASS(k) \
    (G_TYPE_CHECK_CLASS_CAST((k), NM_TYPE_DNS_MANAGER, NMDnsManagerClass))
#define NM_IS_DNS_MANAGER(o)       (G_TYPE_CHECK_INSTANCE_TYPE((o), NM_TYPE_DNS_MANAGER))
#define NM_IS_DNS_MANAGER_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE((k), NM_TYPE_DNS_MANAGER))
#define NM_DNS_MANAGER_GET_CLASS(o) \
    (G_TYPE_INSTANCE_GET_CLASS((o), NM_TYPE_DNS_MANAGER, NMDnsManagerClass))

/* properties */
#define NM_DNS_MANAGER_MODE           "mode"
#define NM_DNS_MANAGER_RC_MANAGER     "rc-manager"
#define NM_DNS_MANAGER_CONFIGURATION  "configuration"
#define NM_DNS_MANAGER_UPDATE_PENDING "update-pending"

/* internal signals */
#define NM_DNS_MANAGER_CONFIG_CHANGED "config-changed"

typedef struct _NMDnsManager      NMDnsManager;
typedef struct _NMDnsManagerClass NMDnsManagerClass;

GType nm_dns_manager_get_type(void);

NMDnsManager *nm_dns_manager_get(void);

/* Allow changes to be batched together */
void nm_dns_manager_begin_updates(NMDnsManager *self, const char *func);
void nm_dns_manager_end_updates(NMDnsManager *self, const char *func);

gboolean nm_dns_manager_set_ip_config(NMDnsManager         *self,
                                      int                   addr_family,
                                      gconstpointer         source_tag,
                                      const NML3ConfigData *l3cd,
                                      NMDnsIPConfigType     ip_config_type,
                                      gboolean              replace_all);

void nm_dns_manager_set_hostname(NMDnsManager *self, const char *hostname, gboolean skip_update);

void nm_dns_manager_stop(NMDnsManager *self);

NMDnsPlugin *nm_dns_manager_get_systemd_resolved(NMDnsManager *self);

gboolean nm_dns_manager_get_update_pending(NMDnsManager *self);

/*****************************************************************************/

char *nmtst_dns_create_resolv_conf(const char *const *searches,
                                   const char *const *nameservers,
                                   const char *const *options);

gboolean nm_dns_manager_is_unmanaged(NMDnsManager *self);

#endif /* __NETWORKMANAGER_DNS_MANAGER_H__ */
