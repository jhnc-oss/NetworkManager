/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2010 Red Hat, Inc.
 */

#ifndef __NM_DNS_PLUGIN_H__
#define __NM_DNS_PLUGIN_H__

#include "c-list/src/c-list.h"

#include "nm-config-data.h"

#define NM_TYPE_DNS_PLUGIN (nm_dns_plugin_get_type())
#define NM_DNS_PLUGIN(obj) (_NM_G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_DNS_PLUGIN, NMDnsPlugin))
#define NM_DNS_PLUGIN_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_DNS_PLUGIN, NMDnsPluginClass))
#define NM_IS_DNS_PLUGIN(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_DNS_PLUGIN))
#define NM_IS_DNS_PLUGIN_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_DNS_PLUGIN))
#define NM_DNS_PLUGIN_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_DNS_PLUGIN, NMDnsPluginClass))

#define NM_DNS_PLUGIN_UPDATE_PENDING_CHANGED "update-pending-changed"

/**
 * NMDnsManagerResolvConfManager
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN: unspecified rc-manager.
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED: do not touch /etc/resolv.conf
 *   (but still write the internal copy -- unless it is symlinked by
 *   /etc/resolv.conf)
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_AUTO: if /etc/resolv.conf is marked
 *   as an immutable file, use "unmanaged" and don't touch /etc/resolv.conf.
 *   Otherwise, if "systemd-resolved" is enabled (or detected), configure systemd-resolved via D-Bus
 *   and don't touch /etc/resolv.conf.
 *   Otherwise, if "resolvconf" application is found, use it.
 *   As last resort, fallback to "symlink" which writes to /etc/resolv.conf
 *   if (and only if) the file is missing or not a symlink.
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE: similar to "unmanaged",
 *   but indicates that resolv.conf cannot be modified.
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK: NM writes /etc/resolv.conf
 *   if the file is missing or not a symlink. An existing symlink is
 *   left untouched.
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE: Write to /etc/resolv.conf directly.
 *   If it is a file, write it as file, otherwise follow symlinks.
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF: NM is managing resolv.conf
     through resolvconf
 * @NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG: NM is managing resolv.conf
     through netconfig
 *
 * NMDnsManager's management of resolv.conf
 */
typedef enum {
    NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_AUTO,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF,
    NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG,
} NMDnsManagerResolvConfManager;

const char *_rc_manager_to_string(NMDnsManagerResolvConfManager val);

typedef struct {
    const CList                  *ip_data_lst_head;
    gboolean                      caching_successful;
    gboolean                      resolved_used;
    gboolean                      resolver_depends_on_nm;
    NMDnsManagerResolvConfManager rc_manager;
    const char                   *hostdomain;
    NMGlobalDnsConfig            *global_config;
} NMDnsUpdateData;

struct _NMDnsPluginPrivate;

typedef struct {
    GObject                     parent;
    struct _NMDnsPluginPrivate *_priv;
} NMDnsPlugin;

typedef struct {
    GObjectClass parent;

    /* Called when DNS information is changed.  'configs' is an array
     * of pointers to NMDnsConfigIPData sorted by priority.
     * 'global_config' is the optional global DNS
     * configuration.
     */
    gboolean (*update)(NMDnsPlugin *plugin, NMDnsUpdateData *update_data, GError **error);

    void (*stop)(NMDnsPlugin *self);

    gboolean (*get_update_pending)(NMDnsPlugin *self);

    const char *plugin_name;

    /* Types should set to TRUE if they start a local caching nameserver
     * that listens on localhost and would block any other local caching
     * nameserver from operating.
     */
    bool is_caching : 1;

    guint8 hash[NM_UTILS_CHECKSUM_LENGTH_SHA1]; /* SHA1 hash of current plugin config */

    /* Each way to set up resolution can be sensitive to different
     * options or data, this function ensures that update is done
     * only when relevant data change */
    void (*checksum)(const NML3ConfigData *l3cd,
                     GChecksum            *sum,
                     int                   addr_family,
                     NMDnsIPConfigType     dns_ip_config_type);
} NMDnsPluginClass;

GType nm_dns_plugin_get_type(void);

gboolean nm_dns_plugin_is_caching(NMDnsPlugin *self);

const char *nm_dns_plugin_get_name(NMDnsPlugin *self);

const guint8 *nm_dns_plugin_get_hash(NMDnsPlugin *self);

void nm_dns_plugin_set_hash(NMDnsPlugin *self, guint8 *hash);

gboolean nm_dns_plugin_update(NMDnsPlugin *self, NMDnsUpdateData *update_data, GError **error);

void nm_dns_plugin_checksum(NMDnsPlugin          *self,
                            const NML3ConfigData *l3cd,
                            GChecksum            *sum,
                            int                   addr_family,
                            NMDnsIPConfigType     dns_ip_config_type);

void nm_dns_plugin_stop(NMDnsPlugin *self);

gboolean nm_dns_plugin_get_update_pending(NMDnsPlugin *self);

void _nm_dns_plugin_update_pending_maybe_changed(NMDnsPlugin *self);

#endif /* __NM_DNS_PLUGIN_H__ */
