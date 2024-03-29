/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2010 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_DHCP_DHCLIENT_UTILS_H__
#define __NETWORKMANAGER_DHCP_DHCLIENT_UTILS_H__

#include "nm-setting-ip4-config.h"
#include "nm-setting-ip6-config.h"

char *nm_dhcp_dhclient_create_config(const char         *interface,
                                     int                 addr_family,
                                     GBytes             *client_id,
                                     gboolean            send_client_id,
                                     const char         *anycast_addr,
                                     const char         *hostname,
                                     guint32             timeout,
                                     gboolean            use_fqdn,
                                     NMDhcpHostnameFlags hostname_flags,
                                     const char         *mud_url,
                                     const char *const  *reject_servers,
                                     const char         *orig_path,
                                     const char         *orig_contents,
                                     GBytes            **out_new_client_id);

char *nm_dhcp_dhclient_escape_duid(GBytes *duid);

GBytes *nm_dhcp_dhclient_unescape_duid(const char *duid);

GBytes *nm_dhcp_dhclient_read_duid(const char *leasefile, GError **error);

gboolean nm_dhcp_dhclient_save_duid(const char *leasefile,
                                    GBytes     *duid,
                                    gboolean    enforce_duid,
                                    GError    **error);

#endif /* __NETWORKMANAGER_DHCP_DHCLIENT_UTILS_H__ */
