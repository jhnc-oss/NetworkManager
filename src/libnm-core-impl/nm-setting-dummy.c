/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2017 Red Hat, Inc.
 */

#include "libnm-core-impl/nm-default-libnm-core.h"

#include "nm-setting-dummy.h"

#include "nm-connection-private.h"
#include "nm-setting-connection.h"
#include "nm-setting-private.h"

/**
 * SECTION:nm-setting-dummy
 * @short_description: Describes connection properties for dummy interfaces
 *
 * The #NMSettingDummy object is a #NMSetting subclass that describes properties
 * necessary for connection to dummy devices
 **/

/*****************************************************************************/

/**
 * NMSettingDummy:
 *
 * Dummy Link Settings
 */
struct _NMSettingDummy {
    NMSetting parent;
};

struct _NMSettingDummyClass {
    NMSettingClass parent;
};

G_DEFINE_TYPE(NMSettingDummy, nm_setting_dummy, NM_TYPE_SETTING)

/*****************************************************************************/

static gboolean
verify(NMSetting *setting, NMConnection *connection, GError **error)
{
    if (!_nm_connection_verify_required_interface_name(connection, error))
        return FALSE;

    return TRUE;
}

/*****************************************************************************/

static void
nm_setting_dummy_init(NMSettingDummy *setting)
{}

/**
 * nm_setting_dummy_new:
 *
 * Creates a new #NMSettingDummy object with default values.
 *
 * Returns: (transfer full): the new empty #NMSettingDummy object
 *
 * Since: 1.8
 **/
NMSetting *
nm_setting_dummy_new(void)
{
    return g_object_new(NM_TYPE_SETTING_DUMMY, NULL);
}

static void
nm_setting_dummy_class_init(NMSettingDummyClass *klass)
{
    NMSettingClass *setting_class = NM_SETTING_CLASS(klass);

    setting_class->verify = verify;

    _nm_setting_class_commit(setting_class, NM_META_SETTING_TYPE_DUMMY, NULL, NULL, 0);
}
