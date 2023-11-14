/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2023 Red Hat, Inc.
 */

#include "libnm-core-impl/nm-default-libnm-core.h"

#include "nm-setting-hsr.h"

#include <linux/if_ether.h>

#include "nm-connection-private.h"
#include "nm-utils.h"
#include "nm-utils-private.h"

/**
 * SECTION:nm-setting-hsr
 * @short_description: Describes connection properties for HSR/PRP interfaces
 *
 * The #NMSettingHsr object is a #NMSetting subclass that describes properties
 * necessary for HSR/PRP connections.
 **/

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE(NMSettingHsr,
                             PROP_PORT1,
                             PROP_PORT2,
                             PROP_SUPERVISION_ADDRESS,
                             PROP_MULTICAST_SPEC,
                             PROP_PRP, );

typedef struct {
    char   *port1;
    char   *port2;
    char   *supervision_address;
    guint32 multicast_spec;
    bool    prp;
} NMSettingHsrPrivate;

/**
 * NMSettingHsr:
 *
 * HSR/PRP Settings
 */
struct _NMSettingHsr {
    NMSetting           parent;
    NMSettingHsrPrivate _priv;
};

struct _NMSettingHsrClass {
    NMSettingClass parent;
};

G_DEFINE_TYPE(NMSettingHsr, nm_setting_hsr, NM_TYPE_SETTING)

#define NM_SETTING_HSR_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMSettingHsr, NM_IS_SETTING_HSR, NMSetting)

/*****************************************************************************/

/**
 * nm_setting_hsr_get_port1:
 * @setting: the #NMSettingHsr
 *
 * Returns: the #NMSettingHsr:port1 property of the setting
 *
 * Since: 1.46
 **/
const char *
nm_setting_hsr_get_port1(NMSettingHsr *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_HSR(setting), NULL);

    return NM_SETTING_HSR_GET_PRIVATE(setting)->port1;
}

/**
 * nm_setting_hsr_get_port2:
 * @setting: the #NMSettingHsr
 *
 * Returns: the #NMSettingHsr:port2 property of the setting
 *
 * Since: 1.46
 **/
const char *
nm_setting_hsr_get_port2(NMSettingHsr *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_HSR(setting), NULL);

    return NM_SETTING_HSR_GET_PRIVATE(setting)->port2;
}

/**
 * nm_setting_hsr_get_supervision_address:
 * @setting: the #NMSettingHsr
 *
 * Returns: the #NMSettingHsr:supervision_address property of the setting
 *
 * Since: 1.46
 **/
const char *
nm_setting_hsr_get_supervision_address(NMSettingHsr *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_HSR(setting), NULL);

    return NM_SETTING_HSR_GET_PRIVATE(setting)->supervision_address;
}

/**
 * nm_setting_hsr_get_multicast_spec:
 * @setting: the #NMSettingHsr
 *
 * Returns: the #NMSettingHsr:multicast_spec property of the setting
 *
 * Since: 1.46
 **/
guint32
nm_setting_hsr_get_multicast_spec(NMSettingHsr *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_HSR(setting), 0);

    return NM_SETTING_HSR_GET_PRIVATE(setting)->multicast_spec;
}

/**
 * nm_setting_hsr_get_prp:
 * @setting: the #NMSettingHsr
 *
 * Returns: the #NMSettingHsr:prp property of the setting
 *
 * Since: 1.46
 **/
gboolean
nm_setting_hsr_get_prp(NMSettingHsr *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_HSR(setting), FALSE);

    return NM_SETTING_HSR_GET_PRIVATE(setting)->prp;
}

/*****************************************************************************/

static gboolean
verify(NMSetting *setting, NMConnection *connection, GError **error)
{
    NMSettingHsrPrivate *priv = NM_SETTING_HSR_GET_PRIVATE(setting);

    if (!priv->port1) {
        g_set_error(error,
                    NM_CONNECTION_ERROR,
                    NM_CONNECTION_ERROR_MISSING_PROPERTY,
                    _("property is not specified"));
        g_prefix_error(error, "%s.%s: ", NM_SETTING_HSR_SETTING_NAME, NM_SETTING_HSR_PORT1);
        return FALSE;
    }

    if (!priv->port2) {
        g_set_error(error,
                    NM_CONNECTION_ERROR,
                    NM_CONNECTION_ERROR_MISSING_PROPERTY,
                    _("property is not specified"));
        g_prefix_error(error, "%s.%s: ", NM_SETTING_HSR_SETTING_NAME, NM_SETTING_HSR_PORT2);
        return FALSE;
    }

    return TRUE;
}

/*****************************************************************************/

static void
nm_setting_hsr_init(NMSettingHsr *setting)
{}

/**
 * nm_setting_hsr_new:
 *
 * Creates a new #NMSettingHsr object with default values.
 *
 * Returns: (transfer full): the new empty #NMSettingHsr object
 *
 * Since: 1.46
 **/
NMSetting *
nm_setting_hsr_new(void)
{
    return g_object_new(NM_TYPE_SETTING_HSR, NULL);
}

static void
nm_setting_hsr_class_init(NMSettingHsrClass *klass)
{
    GObjectClass   *object_class        = G_OBJECT_CLASS(klass);
    NMSettingClass *setting_class       = NM_SETTING_CLASS(klass);
    GArray         *properties_override = _nm_sett_info_property_override_create_array();

    object_class->get_property = _nm_setting_property_get_property_direct;
    object_class->set_property = _nm_setting_property_set_property_direct;

    setting_class->verify = verify;

    /**
	 * NMSettingHsr:port1:
	 *
	 * This property specifies the port1 interface name of the HSR. This property is mandatory.
	 *
	 * Since: 1.46
	 **/
    _nm_setting_property_define_direct_string(properties_override,
                                              obj_properties,
                                              NM_SETTING_HSR_PORT1,
                                              PROP_PORT1,
                                              NM_SETTING_PARAM_INFERRABLE,
                                              NMSettingHsr,
                                              _priv.port1);

    /**
	 * NMSettingHsr:port2:
	 *
	 * This property specifies the port2 interface name of the HSR. This property is mandatory.
	 *
	 * Since: 1.46
	 **/
    _nm_setting_property_define_direct_string(properties_override,
                                              obj_properties,
                                              NM_SETTING_HSR_PORT2,
                                              PROP_PORT2,
                                              NM_SETTING_PARAM_INFERRABLE,
                                              NMSettingHsr,
                                              _priv.port2);

    /**
	 * NMSettingHsr:supervision-address:
	 *
	 * This property specifies the supverision MAC address.
	 *
	 * Since: 1.46
	 **/
    _nm_setting_property_define_direct_string(properties_override,
                                              obj_properties,
                                              NM_SETTING_HSR_SUPERVISION_ADDRESS,
                                              PROP_SUPERVISION_ADDRESS,
                                              NM_SETTING_PARAM_INFERRABLE,
                                              NMSettingHsr,
                                              _priv.supervision_address);

    /**
	 * NMSettingHsr:multicast-spec:
	 *
	 * This property specifies the last byte of supervision address.
	 *
	 * Since: 1.46
	 **/
    _nm_setting_property_define_direct_uint32(properties_override,
                                              obj_properties,
                                              NM_SETTING_HSR_MULTICAST_SPEC,
                                              PROP_MULTICAST_SPEC,
                                              0,
                                              G_MAXUINT8,
                                              0,
                                              NM_SETTING_PARAM_INFERRABLE,
                                              NMSettingHsr,
                                              _priv.multicast_spec);

    /**
	 * NMSettingHsr:prp:
	 *
	 * This property specifies the protocol used by the interface whether it is PRP or HSR.
	 *
	 * Since: 1.46
	 **/
    _nm_setting_property_define_direct_boolean(properties_override,
                                               obj_properties,
                                               NM_SETTING_HSR_PRP,
                                               PROP_PRP,
                                               FALSE,
                                               NM_SETTING_PARAM_INFERRABLE,
                                               NMSettingHsr,
                                               _priv.prp);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);

    _nm_setting_class_commit(setting_class, NM_META_SETTING_TYPE_HSR, NULL, properties_override, 0);
}
