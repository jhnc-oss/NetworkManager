/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "libnm-core-impl/nm-default-libnm-core.h"

#include "nm-setting-prefix-delegation.h"

#include "nm-connection-private.h"
#include "nm-setting-connection.h"
#include "nm-setting-private.h"

/**
 * SECTION:nm-setting-prefix-delegation
 * @short_description: Describes connection properties related to IPv6 prefix delegation
 *
 * The #NMSettingPrefixDelegation object is a #NMSetting subclass that describes the
 * configuration of downstream interfaces using IPv6 prefix delegation.
 **/

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE_BASE(PROP_SUBNET_ID, PROP_EVICT_OLDEST, );

/**
 * NMSettingPrefixDelegation:
 *
 * IPv6 prefix delegation settings
 *
 * Since: 1.54
 */
struct _NMSettingPrefixDelegation {
    NMSetting parent;
    gint64    subnet_id;
    bool      evict_oldest;
};

struct _NMSettingPrefixDelegationClass {
    NMSettingClass parent;
};

G_DEFINE_TYPE(NMSettingPrefixDelegation, nm_setting_prefix_delegation, NM_TYPE_SETTING)

/*****************************************************************************/

/**
 * nm_setting_prefix_delegation_get_subnet_id:
 * @setting: the #NMSettingPrefixDelegation
 *
 * Returns: the subnet ID for prefix delegation
 *
 * Since: 1.54
 **/
gint64
nm_setting_prefix_delegation_get_subnet_id(NMSettingPrefixDelegation *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_PREFIX_DELEGATION(setting), 0);

    return setting->subnet_id;
}

/**
 * nm_setting_prefix_delegation_get_evict_oldest:
 * @setting: the #NMSettingPrefixDelegation
 *
 * Returns: whether to evict oldest addresses when adding new ones
 *
 * Since: 1.58
 **/
gboolean
nm_setting_prefix_delegation_get_evict_oldest(NMSettingPrefixDelegation *setting)
{
    g_return_val_if_fail(NM_IS_SETTING_PREFIX_DELEGATION(setting), FALSE);

    return setting->evict_oldest;
}

/*****************************************************************************/

static void
nm_setting_prefix_delegation_init(NMSettingPrefixDelegation *setting)
{}

/**
 * nm_setting_prefix_delegation_new:
 *
 * Creates a new #NMSettingPrefixDelegation object with default values.
 *
 * Returns: (transfer full): the new empty #NMSettingPrefixDelegation object
 *
 * Since: 1.54
 **/
NMSetting *
nm_setting_prefix_delegation_new(void)
{
    return g_object_new(NM_TYPE_SETTING_PREFIX_DELEGATION, NULL);
}

static void
nm_setting_prefix_delegation_class_init(NMSettingPrefixDelegationClass *klass)
{
    GObjectClass   *object_class        = G_OBJECT_CLASS(klass);
    NMSettingClass *setting_class       = NM_SETTING_CLASS(klass);
    GArray         *properties_override = _nm_sett_info_property_override_create_array();

    object_class->get_property = _nm_setting_property_get_property_direct;
    object_class->set_property = _nm_setting_property_set_property_direct;

    /**
     * NMSettingPrefixDelegation:subnet-id:
     *
     * The subnet ID to use on the interface from the prefix delegation received via
     * an upstream interface. Set to a value between 0 and 0xffffffff (2^32 - 1)
     * to indicate a specific subnet ID; or set to -1 to automatically choose
     * an available subnet ID.
     *
     * Since: 1.54
     **/
    _nm_setting_property_define_direct_int64(properties_override,
                                             obj_properties,
                                             NM_SETTING_PREFIX_DELEGATION_SUBNET_ID,
                                             PROP_SUBNET_ID,
                                             -1,
                                             G_MAXUINT32,
                                             -1,
                                             NM_SETTING_PARAM_NONE,
                                             NMSettingPrefixDelegation,
                                             subnet_id);

    /**
     * NMSettingPrefixDelegation:evict-oldest:
     *
     * If %TRUE, when the maximum number of addresses is reached and a new
     * prefix is delegated, the address with the shortest remaining lifetime
     * will be evicted to make room for the new prefix. This is useful for
     * scenarios where upstream prefixes change frequently (e.g., unstable
     * connections), ensuring the newest working prefix is always advertised.
     *
     * Since: 1.58
     **/
    _nm_setting_property_define_direct_boolean(properties_override,
                                               obj_properties,
                                               NM_SETTING_PREFIX_DELEGATION_EVICT_OLDEST,
                                               PROP_EVICT_OLDEST,
                                               FALSE,
                                               NM_SETTING_PARAM_NONE,
                                               NMSettingPrefixDelegation,
                                               evict_oldest);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);

    _nm_setting_class_commit(setting_class,
                             NM_META_SETTING_TYPE_PREFIX_DELEGATION,
                             NULL,
                             properties_override,
                             0);
}
