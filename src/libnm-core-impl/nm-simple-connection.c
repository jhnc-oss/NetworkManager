/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2007 - 2008 Novell, Inc.
 * Copyright (C) 2007 - 2014 Red Hat, Inc.
 */

#include "libnm-core-impl/nm-default-libnm-core.h"

#include "nm-simple-connection.h"

#include "nm-setting-private.h"

/**
 * SECTION:nm-simple-connection
 * @short_description: An unmanaged connection
 *
 * An #NMSimpleConnection does not directly represent a D-Bus-exported connection,
 * but might be used in the process of creating a new one.
 **/

/*****************************************************************************/

GTypeClass *_nm_simple_connection_class_instance = NULL;
int         _nm_simple_connection_private_offset;

/*****************************************************************************/

/**
 * NMSimpleConnection:
 */
struct _NMSimpleConnection {
    GObject parent;
};

struct _NMSimpleConnectionClass {
    GObjectClass parent;
};

static void nm_simple_connection_interface_init(NMConnectionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(NMSimpleConnection,
                        nm_simple_connection,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(NM_TYPE_CONNECTION,
                                              nm_simple_connection_interface_init);)

/*****************************************************************************/

static void
nm_simple_connection_init(NMSimpleConnection *self)
{
    NMConnectionPrivate *priv;

    priv = _NM_SIMPLE_CONNECTION_GET_CONNECTION_PRIVATE(self);

    priv->self = (NMConnection *) self;
}

/**
 * nm_simple_connection_new:
 *
 * Creates a new #NMSimpleConnection object with no #NMSetting objects.
 *
 * Returns: (transfer full): the new empty #NMConnection object
 **/
NMConnection *
nm_simple_connection_new(void)
{
    return g_object_new(NM_TYPE_SIMPLE_CONNECTION, NULL);
}

/**
 * _nm_simple_connection_new_from_dbus:
 * @dict: a #GVariant of type %NM_VARIANT_TYPE_CONNECTION describing the connection
 * @error: on unsuccessful return, an error
 *
 * Creates a new #NMSimpleConnection from a hash table describing the
 * connection.  See nm_connection_to_dbus() for a description of the expected
 * hash table.
 *
 * Returns: (transfer full): the new #NMSimpleConnection object, populated with
 * settings created from the values in the hash table, or %NULL if there was
 * an error.
 **/
NMConnection *
_nm_simple_connection_new_from_dbus(GVariant *dict, NMSettingParseFlags parse_flags, GError **error)
{
    NMConnection *connection;

    g_return_val_if_fail(dict != NULL, NULL);
    g_return_val_if_fail(g_variant_is_of_type(dict, NM_VARIANT_TYPE_CONNECTION), NULL);
    g_return_val_if_fail(!NM_FLAGS_ANY(parse_flags, ~NM_SETTING_PARSE_FLAGS_ALL), NULL);
    g_return_val_if_fail(
        !NM_FLAGS_ALL(parse_flags,
                      NM_SETTING_PARSE_FLAGS_STRICT | NM_SETTING_PARSE_FLAGS_BEST_EFFORT),
        NULL);

    connection = nm_simple_connection_new();
    if (!_nm_connection_replace_settings(connection, dict, parse_flags, error))
        g_clear_object(&connection);
    return connection;
}

/**
 * nm_simple_connection_new_from_dbus:
 * @dict: a #GVariant of type %NM_VARIANT_TYPE_CONNECTION describing the connection
 * @error: on unsuccessful return, an error
 *
 * Creates a new #NMSimpleConnection from a hash table describing the
 * connection and normalize the connection.  See nm_connection_to_dbus() for a
 * description of the expected hash table.
 *
 * Returns: (transfer full): the new #NMSimpleConnection object, populated with
 * settings created from the values in the hash table, or %NULL if the
 * connection failed to normalize.
 **/
NMConnection *
nm_simple_connection_new_from_dbus(GVariant *dict, GError **error)
{
    return _nm_simple_connection_new_from_dbus(dict, NM_SETTING_PARSE_FLAGS_NORMALIZE, error);
}

/**
 * nm_simple_connection_new_clone:
 * @connection: the #NMConnection to clone
 *
 * Clones an #NMConnection as an #NMSimpleConnection.
 *
 * Returns: (transfer full): a new #NMConnection containing the same settings
 * and properties as the source #NMConnection
 **/
NMConnection *
nm_simple_connection_new_clone(NMConnection *connection)
{
    NMConnection *clone;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    clone = nm_simple_connection_new();

    _nm_connection_set_path_rstr(clone, _nm_connection_get_path_rstr(connection));

    nm_connection_replace_settings_from_connection(clone, connection);

    return clone;
}

static void
dispose(GObject *object)
{
#if NM_MORE_ASSERTS
    g_signal_handlers_disconnect_by_data(object,
                                         (gpointer) &_nm_assert_connection_unchanging_user_data);
#endif

    _nm_connection_private_clear(_NM_SIMPLE_CONNECTION_GET_CONNECTION_PRIVATE(object));

    G_OBJECT_CLASS(nm_simple_connection_parent_class)->dispose(object);
}

static void
nm_simple_connection_class_init(NMSimpleConnectionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(NMConnectionPrivate));

    object_class->dispose = dispose;

    _nm_simple_connection_private_offset = g_type_class_get_instance_private_offset(klass);
    _nm_simple_connection_class_instance = (GTypeClass *) klass;
}

static void
nm_simple_connection_interface_init(NMConnectionInterface *iface)
{}
