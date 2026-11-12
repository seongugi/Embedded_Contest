#define _POSIX_C_SOURCE 200809L

#include "../aruco_ble.h"

#include <stdio.h>
#include <string.h>

/* Hardware-free integration: both BlueZ and the system bus are private mocks.
 * BlueZ runs in its own context because a transport may synchronously inspect
 * Device1 properties while dispatching a characteristic request. */
#define ADAPTER "/org/bluez/hci0"
#define PEER ADAPTER "/dev_12_34_56_78_9A_BC"
#define FOREIGN_PEER ADAPTER "/dev_00_11_22_33_44_55"
#define ALLOWED_ADDRESS "12:34:56:78:9A:BC"
#define FOREIGN_ADDRESS "00:11:22:33:44:55"
#define CALL_TIMEOUT_MS 3000

typedef struct {
    GMutex lock;
    GCond condition;
    gboolean initialized;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *connection;
    char *address;
    char *application_owner;
    char *application_path;
    char *advertisement_path;
    gint application_count;
    gint advertisement_count;
    gint application_inspection_count;
    gint advertisement_inspection_count;
    gint unregister_application_count;
    gint unregister_advertisement_count;
    gint paired;
    gint trusted;
    gint connected;
    gint powered;
} mock_bluez_t;

typedef struct {
    guint write_count;
    guint read_count;
    guint invalidation_count;
    guint8 last_write[ARUCO_BLE_POSE_SIZE];
    gint64 last_timestamp;
    gboolean reject_write;
    gboolean short_read;
} callbacks_t;

typedef struct {
    gboolean done;
    GVariant *reply;
    GError *error;
} pending_call_t;

static const char mock_xml[] =
    "<node>"
    "<interface name='org.freedesktop.DBus.ObjectManager'>"
    "<method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
    "<signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>"
    "</interface>"
    "<interface name='org.bluez.GattManager1'>"
    "<method name='RegisterApplication'><arg type='o' direction='in'/>"
    "<arg type='a{sv}' direction='in'/></method>"
    "<method name='UnregisterApplication'><arg type='o' direction='in'/></method>"
    "</interface>"
    "<interface name='org.bluez.LEAdvertisingManager1'>"
    "<method name='RegisterAdvertisement'><arg type='o' direction='in'/>"
    "<arg type='a{sv}' direction='in'/></method>"
    "<method name='UnregisterAdvertisement'><arg type='o' direction='in'/></method>"
    "<property name='SupportedInstances' type='y' access='read'/>"
    "<property name='ActiveInstances' type='y' access='read'/>"
    "</interface>"
    "<interface name='org.bluez.Adapter1'>"
    "<property name='Powered' type='b' access='read'/>"
    "<property name='Address' type='s' access='read'/>"
    "</interface>"
    "<interface name='org.bluez.Device1'>"
    "<property name='Address' type='s' access='read'/>"
    "<property name='AddressType' type='s' access='read'/>"
    "<property name='Adapter' type='o' access='read'/>"
    "<property name='Paired' type='b' access='read'/>"
    "<property name='Trusted' type='b' access='read'/>"
    "<property name='Connected' type='b' access='read'/>"
    "</interface>"
    "</node>";

static GVariant *device_properties(mock_bluez_t *mock, gboolean foreign)
{
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&properties, "{sv}", "Address",
                          g_variant_new_string(foreign ? FOREIGN_ADDRESS : ALLOWED_ADDRESS));
    g_variant_builder_add(&properties, "{sv}", "AddressType", g_variant_new_string("public"));
    g_variant_builder_add(&properties, "{sv}", "Adapter", g_variant_new_object_path(ADAPTER));
    g_variant_builder_add(&properties, "{sv}", "Paired",
                          g_variant_new_boolean(g_atomic_int_get(&mock->paired) != 0));
    g_variant_builder_add(&properties, "{sv}", "Trusted",
                          g_variant_new_boolean(g_atomic_int_get(&mock->trusted) != 0));
    g_variant_builder_add(&properties, "{sv}", "Connected",
                          g_variant_new_boolean(g_atomic_int_get(&mock->connected) != 0));
    return g_variant_builder_end(&properties);
}

static GVariant *managed_objects(mock_bluez_t *mock)
{
    GVariantBuilder objects;
    GVariantBuilder interfaces;
    GVariantBuilder properties;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&properties, "{sv}", "Powered",
                          g_variant_new_boolean(g_atomic_int_get(&mock->powered) != 0));
    g_variant_builder_add(&properties, "{sv}", "Address", g_variant_new_string("02:00:00:00:00:01"));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.Adapter1",
                          g_variant_builder_end(&properties));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.GattManager1",
                          g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0u));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.LEAdvertisingManager1",
                          g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0u));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}", ADAPTER, g_variant_builder_end(&interfaces));
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.Device1", device_properties(mock, FALSE));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}", PEER, g_variant_builder_end(&interfaces));
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.Device1", device_properties(mock, TRUE));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}", FOREIGN_PEER, g_variant_builder_end(&interfaces));
    return g_variant_builder_end(&objects);
}

static void mock_method(GDBusConnection *connection, const char *sender,
                        const char *object_path, const char *interface_name,
                        const char *method_name, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer user_data)
{
    mock_bluez_t *mock = user_data;
    const char *path;
    (void)connection;
    (void)object_path;
    (void)interface_name;
    if (g_str_equal(method_name, "GetManagedObjects")) {
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(@a{oa{sa{sv}}})", managed_objects(mock)));
        return;
    }
    g_variant_get_child(parameters, 0u, "&o", &path);
    /* Actual BlueZ calls back into the server before accepting registration.
     * Exercise that handshake so a synchronous registration deadlock fails. */
    if (g_str_equal(method_name, "RegisterApplication") ||
        g_str_equal(method_name, "RegisterAdvertisement")) {
        gboolean application = g_str_equal(method_name, "RegisterApplication");
        GError *error = NULL;
        GVariant *reply = g_dbus_connection_call_sync(
            mock->connection, sender, path,
            application ? "org.freedesktop.DBus.ObjectManager" : "org.freedesktop.DBus.Properties",
            application ? "GetManagedObjects" : "GetAll",
            application ? NULL : g_variant_new("(s)", "org.bluez.LEAdvertisement1"),
            application ? G_VARIANT_TYPE("(a{oa{sa{sv}}})") : G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, &error);
        g_assert_no_error(error);
        g_assert_nonnull(reply);
        if (application)
            g_atomic_int_inc(&mock->application_inspection_count);
        else
            g_atomic_int_inc(&mock->advertisement_inspection_count);
        g_variant_unref(reply);
    }
    g_mutex_lock(&mock->lock);
    if (g_str_equal(method_name, "RegisterApplication")) {
        g_free(mock->application_owner);
        g_free(mock->application_path);
        mock->application_owner = g_strdup(sender);
        mock->application_path = g_strdup(path);
        g_atomic_int_inc(&mock->application_count);
    } else if (g_str_equal(method_name, "RegisterAdvertisement")) {
        g_free(mock->advertisement_path);
        mock->advertisement_path = g_strdup(path);
        g_atomic_int_inc(&mock->advertisement_count);
    } else if (g_str_equal(method_name, "UnregisterApplication")) {
        g_atomic_int_inc(&mock->unregister_application_count);
    } else if (g_str_equal(method_name, "UnregisterAdvertisement")) {
        g_atomic_int_inc(&mock->unregister_advertisement_count);
    } else {
        g_assert_not_reached();
    }
    g_mutex_unlock(&mock->lock);
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *mock_property(GDBusConnection *connection, const char *sender,
                              const char *object_path, const char *interface_name,
                              const char *property_name, GError **error, gpointer user_data)
{
    mock_bluez_t *mock = user_data;
    (void)connection;
    (void)sender;
    (void)error;
    if (g_str_equal(interface_name, "org.bluez.Adapter1")) {
        if (g_str_equal(property_name, "Powered"))
            return g_variant_new_boolean(g_atomic_int_get(&mock->powered) != 0);
        return g_variant_new_string("02:00:00:00:00:01");
    }
    if (g_str_equal(interface_name, "org.bluez.LEAdvertisingManager1"))
        return g_variant_new_byte(1u);
    if (g_str_equal(property_name, "Address"))
        return g_variant_new_string(g_str_equal(object_path, PEER) ? ALLOWED_ADDRESS : FOREIGN_ADDRESS);
    if (g_str_equal(property_name, "AddressType"))
        return g_variant_new_string("public");
    if (g_str_equal(property_name, "Adapter"))
        return g_variant_new_object_path(ADAPTER);
    if (g_str_equal(property_name, "Paired"))
        return g_variant_new_boolean(g_atomic_int_get(&mock->paired) != 0);
    if (g_str_equal(property_name, "Trusted"))
        return g_variant_new_boolean(g_atomic_int_get(&mock->trusted) != 0);
    if (g_str_equal(property_name, "Connected"))
        return g_variant_new_boolean(g_atomic_int_get(&mock->connected) != 0);
    g_assert_not_reached();
}

static const GDBusInterfaceVTable mock_vtable = {
    .method_call = mock_method,
    .get_property = mock_property,
    .set_property = NULL
};

static GDBusConnection *private_connection(const char *address)
{
    GError *error = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
        address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);
    g_dbus_connection_set_exit_on_close(connection, FALSE);
    return connection;
}

static void change_name(GDBusConnection *connection, const char *method)
{
    GError *error = NULL;
    gboolean acquire = g_str_equal(method, "RequestName");
    GVariant *reply = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        method, acquire ? g_variant_new("(su)", "org.bluez", 0u) : g_variant_new("(s)", "org.bluez"),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, &error);
    guint32 result;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(u)", &result);
    g_assert_cmpuint(result, ==, 1u);
    g_variant_unref(reply);
}

static gpointer mock_thread(gpointer user_data)
{
    mock_bluez_t *mock = user_data;
    GError *error = NULL;
    GDBusNodeInfo *info;
    guint registrations[7];
    guint count = 0u;
    mock->context = g_main_context_new();
    g_main_context_push_thread_default(mock->context);
    mock->loop = g_main_loop_new(mock->context, FALSE);
    mock->connection = private_connection(mock->address);
    info = g_dbus_node_info_new_for_xml(mock_xml, &error);
    g_assert_no_error(error);
    for (guint index = 0u; info->interfaces[index] != NULL; ++index) {
        const char *name = info->interfaces[index]->name;
        const char *path = g_str_equal(name, "org.freedesktop.DBus.ObjectManager") ? "/" : ADAPTER;
        if (g_str_equal(name, "org.bluez.Device1"))
            path = PEER;
        registrations[count++] = g_dbus_connection_register_object(
            mock->connection, path, info->interfaces[index], &mock_vtable, mock, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpuint(registrations[count - 1u], >, 0u);
        if (g_str_equal(name, "org.freedesktop.DBus.ObjectManager") || g_str_equal(name, "org.bluez.Device1")) {
            registrations[count++] = g_dbus_connection_register_object(
                mock->connection, g_str_equal(name, "org.bluez.Device1") ? FOREIGN_PEER : "/org/bluez",
                info->interfaces[index], &mock_vtable, mock, NULL, &error);
            g_assert_no_error(error);
        }
    }
    change_name(mock->connection, "RequestName");
    g_mutex_lock(&mock->lock);
    mock->initialized = TRUE;
    g_cond_signal(&mock->condition);
    g_mutex_unlock(&mock->lock);
    g_main_loop_run(mock->loop);
    for (guint index = 0u; index < count; ++index)
        g_assert_true(g_dbus_connection_unregister_object(mock->connection, registrations[index]));
    g_dbus_node_info_unref(info);
    g_assert_true(g_dbus_connection_close_sync(mock->connection, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(mock->connection);
    g_main_loop_unref(mock->loop);
    g_main_context_pop_thread_default(mock->context);
    g_main_context_unref(mock->context);
    return NULL;
}

static gboolean quit_mock(gpointer user_data)
{
    mock_bluez_t *mock = user_data;
    g_main_loop_quit(mock->loop);
    return G_SOURCE_REMOVE;
}

static void pump_context(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    g_usleep(1000u);
}

static void call_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    pending_call_t *pending = user_data;
    pending->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &pending->error);
    pending->done = TRUE;
}

static GVariant *invoke(GDBusConnection *connection, const char *owner, const char *path,
                        const char *interface_name, const char *method, GVariant *parameters,
                        gboolean expected_success)
{
    pending_call_t pending = {0};
    g_dbus_connection_call(connection, owner, path, interface_name, method, parameters,
                           NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, call_finished, &pending);
    while (!pending.done)
        pump_context();
    if (expected_success) {
        g_assert_no_error(pending.error);
        g_assert_nonnull(pending.reply);
    } else {
        g_assert_null(pending.reply);
        g_assert_nonnull(pending.error);
        g_assert_true(g_dbus_error_is_remote_error(pending.error));
        g_assert_false(g_error_matches(pending.error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD));
        g_clear_error(&pending.error);
    }
    return pending.reply;
}

static gboolean pose_written(const char *device, const guint8 *data, gsize length,
                              gint64 timestamp, gpointer user_data, GError **error)
{
    callbacks_t *callbacks = user_data;
    g_assert_cmpstr(device, ==, PEER);
    g_assert_cmpuint(length, ==, ARUCO_BLE_POSE_SIZE);
    g_assert_cmpint(timestamp, >, 0);
    callbacks->write_count++;
    memcpy(callbacks->last_write, data, length);
    callbacks->last_timestamp = timestamp;
    if (callbacks->reject_write) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "test rejected pose");
        return FALSE;
    }
    return TRUE;
}

static gboolean clock_read(const char *device, GByteArray *value,
                            gpointer user_data, GError **error)
{
    callbacks_t *callbacks = user_data;
    guint8 bytes[ARUCO_BLE_CLOCK_SIZE];
    (void)error;
    g_assert_cmpstr(device, ==, PEER);
    callbacks->read_count++;
    for (guint index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = (guint8)(index + 0x30u);
    g_byte_array_append(value, bytes, callbacks->short_read ? 1u : (guint)sizeof(bytes));
    return TRUE;
}

static void invalidate(gpointer user_data)
{
    callbacks_t *callbacks = user_data;
    callbacks->invalidation_count++;
}

static GVariant *request_options(const char *device, guint16 offset, guint16 mtu,
                                  const char *write_type, gboolean prepare)
{
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    if (device != NULL)
        g_variant_builder_add(&options, "{sv}", "device", g_variant_new_object_path(device));
    g_variant_builder_add(&options, "{sv}", "offset", g_variant_new_uint16(offset));
    g_variant_builder_add(&options, "{sv}", "mtu", g_variant_new_uint16(mtu));
    g_variant_builder_add(&options, "{sv}", "link", g_variant_new_string("LE"));
    if (write_type != NULL)
        g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string(write_type));
    if (prepare)
        g_variant_builder_add(&options, "{sv}", "prepare-authorize", g_variant_new_boolean(TRUE));
    return g_variant_builder_end(&options);
}

static void write_pose(GDBusConnection *connection, const char *owner, const char *path,
                       const char *device, guint16 offset, guint16 mtu, const char *type,
                       gboolean prepare, gsize length, gboolean expected_success)
{
    guint8 payload[ARUCO_BLE_POSE_SIZE + 1u];
    GVariant *reply;
    for (guint index = 0u; index < sizeof(payload); ++index)
        payload[index] = (guint8)(index + 1u);
    reply = invoke(connection, owner, path, "org.bluez.GattCharacteristic1", "WriteValue",
                   g_variant_new("(@ay@a{sv})", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, payload, length, 1u),
                                 request_options(device, offset, mtu, type, prepare)), expected_success);
    if (reply != NULL)
        g_variant_unref(reply);
}

static void read_clock(GDBusConnection *connection, const char *owner, const char *path,
                        const char *device, guint16 offset, gboolean expected_success)
{
    GVariant *reply = invoke(connection, owner, path, "org.bluez.GattCharacteristic1", "ReadValue",
                             g_variant_new("(@a{sv})", request_options(device, offset, 64u, NULL, FALSE)),
                             expected_success);
    if (reply != NULL) {
        GVariant *bytes;
        gsize length;
        const guint8 *data;
        g_variant_get(reply, "(@ay)", &bytes);
        data = g_variant_get_fixed_array(bytes, &length, 1u);
        g_assert_cmpuint(length, ==, ARUCO_BLE_CLOCK_SIZE);
        for (guint index = 0u; index < length; ++index)
            g_assert_cmpuint(data[index], ==, index + 0x30u);
        g_variant_unref(bytes);
        g_variant_unref(reply);
    }
}

static void emit_boolean_from(GDBusConnection *connection, const char *path, const char *interface_name,
                               const char *property, gboolean value)
{
    GVariantBuilder changed;
    GVariantBuilder invalidated;
    GError *error = NULL;
    g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&changed, "{sv}", property, g_variant_new_boolean(value));
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
    g_assert_true(g_dbus_connection_emit_signal(
        connection, NULL, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)", interface_name,
                      g_variant_builder_end(&changed), g_variant_builder_end(&invalidated)), &error));
    g_assert_no_error(error);
    g_assert_true(g_dbus_connection_flush_sync(connection, NULL, &error));
    g_assert_no_error(error);
}

static void emit_boolean(mock_bluez_t *mock, const char *path, const char *interface_name,
                          const char *property, gboolean value)
{
    emit_boolean_from(mock->connection, path, interface_name, property, value);
}

static void wait_ready(aruco_ble_t *ble)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!aruco_ble_is_ready(ble) && g_get_monotonic_time() < deadline)
        pump_context();
    g_assert_true(aruco_ble_is_ready(ble));
}

static void wait_invalidation(callbacks_t *callbacks, guint previous)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (callbacks->invalidation_count <= previous && g_get_monotonic_time() < deadline)
        pump_context();
    g_assert_cmpuint(callbacks->invalidation_count, >, previous);
}

static gboolean string_array_contains(GVariant *array, const char *expected)
{
    GVariantIter iter;
    const char *value;
    g_variant_iter_init(&iter, array);
    while (g_variant_iter_next(&iter, "&s", &value)) {
        if (g_str_equal(value, expected))
            return TRUE;
    }
    return FALSE;
}

static void inspect_application(mock_bluez_t *mock, const char *owner, const char *root,
                                 char **pose_path, char **clock_path)
{
    GVariant *reply = invoke(mock->connection, owner, root, "org.freedesktop.DBus.ObjectManager",
                             "GetManagedObjects", NULL, TRUE);
    GVariant *objects;
    GVariantIter iter;
    const char *path;
    GVariant *interfaces;
    guint service_count = 0u;
    guint characteristic_count = 0u;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        GVariant *properties = g_variant_lookup_value(interfaces, "org.bluez.GattService1", G_VARIANT_TYPE_VARDICT);
        if (properties != NULL) {
            const char *uuid;
            gboolean primary = FALSE;
            g_assert_true(g_variant_lookup(properties, "UUID", "&s", &uuid));
            g_assert_cmpstr(uuid, ==, ARUCO_BLE_SERVICE_UUID);
            g_assert_true(g_variant_lookup(properties, "Primary", "b", &primary));
            g_assert_true(primary);
            service_count++;
            g_variant_unref(properties);
        }
        properties = g_variant_lookup_value(interfaces, "org.bluez.GattCharacteristic1", G_VARIANT_TYPE_VARDICT);
        if (properties != NULL) {
            const char *uuid;
            const char *service;
            GVariant *flags;
            g_assert_true(g_variant_lookup(properties, "UUID", "&s", &uuid));
            g_assert_true(g_variant_lookup(properties, "Service", "&o", &service));
            g_assert_true(g_str_has_prefix(service, root));
            flags = g_variant_lookup_value(properties, "Flags", G_VARIANT_TYPE("as"));
            g_assert_nonnull(flags);
            if (g_str_equal(uuid, ARUCO_BLE_POSE_UUID)) {
                g_assert_null(*pose_path);
                *pose_path = g_strdup(path);
                g_assert_true(string_array_contains(flags, "write"));
                g_assert_true(string_array_contains(flags, "encrypt-write"));
                g_assert_false(string_array_contains(flags, "write-without-response"));
            } else {
                g_assert_cmpstr(uuid, ==, ARUCO_BLE_CLOCK_UUID);
                g_assert_null(*clock_path);
                *clock_path = g_strdup(path);
                g_assert_true(string_array_contains(flags, "read"));
                g_assert_true(string_array_contains(flags, "encrypt-read"));
            }
            characteristic_count++;
            g_variant_unref(flags);
            g_variant_unref(properties);
        }
        g_variant_unref(interfaces);
    }
    g_assert_cmpuint(service_count, ==, 1u);
    g_assert_cmpuint(characteristic_count, ==, 2u);
    g_assert_nonnull(*pose_path);
    g_assert_nonnull(*clock_path);
    g_variant_unref(objects);
    g_variant_unref(reply);
}

static void inspect_advertisement(mock_bluez_t *mock, const char *owner, const char *path)
{
    GVariant *reply = invoke(mock->connection, owner, path, "org.freedesktop.DBus.Properties", "GetAll",
                             g_variant_new("(s)", "org.bluez.LEAdvertisement1"), TRUE);
    GVariant *properties;
    GVariant *services;
    const char *type;
    const char *name;
    g_variant_get(reply, "(@a{sv})", &properties);
    g_assert_true(g_variant_lookup(properties, "Type", "&s", &type));
    g_assert_cmpstr(type, ==, "peripheral");
    g_assert_true(g_variant_lookup(properties, "LocalName", "&s", &name));
    g_assert_cmpstr(name, ==, "ACE-Test");
    services = g_variant_lookup_value(properties, "ServiceUUIDs", G_VARIANT_TYPE("as"));
    g_assert_nonnull(services);
    g_assert_true(string_array_contains(services, ARUCO_BLE_SERVICE_UUID));
    g_variant_unref(services);
    g_variant_unref(properties);
    g_variant_unref(reply);
}

int main(int argc, char **argv)
{
    GTestDBus *bus;
    mock_bluez_t mock = {0};
    callbacks_t callbacks = {0};
    GThread *thread;
    GDBusConnection *stranger;
    aruco_ble_config_t config = {
        .adapter_path = ADAPTER,
        .local_name = "ACE-Test",
        .allowed_peer_address = ALLOWED_ADDRESS
    };
    aruco_ble_t *ble;
    GError *error = NULL;
    char *saved_system_address = g_strdup(g_getenv("DBUS_SYSTEM_BUS_ADDRESS"));
    char *owner;
    char *root;
    char *advertisement;
    char *pose_path = NULL;
    char *clock_path = NULL;
    guint previous;
    g_test_init(&argc, &argv, NULL);
    bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    mock.address = g_strdup(g_test_dbus_get_bus_address(bus));
    g_assert_true(g_setenv("DBUS_SYSTEM_BUS_ADDRESS", mock.address, TRUE));
    g_mutex_init(&mock.lock);
    g_cond_init(&mock.condition);
    g_atomic_int_set(&mock.paired, TRUE);
    g_atomic_int_set(&mock.trusted, TRUE);
    g_atomic_int_set(&mock.connected, TRUE);
    g_atomic_int_set(&mock.powered, TRUE);
    thread = g_thread_new("mock-bluez", mock_thread, &mock);
    g_mutex_lock(&mock.lock);
    while (!mock.initialized)
        g_cond_wait(&mock.condition, &mock.lock);
    g_mutex_unlock(&mock.lock);
    ble = aruco_ble_new(&config, pose_written, clock_read, invalidate, &callbacks, &error);
    g_assert_no_error(error);
    g_assert_nonnull(ble);
    g_assert_true(aruco_ble_start(ble, &error));
    g_assert_no_error(error);
    wait_ready(ble);
    g_assert_cmpint(g_atomic_int_get(&mock.application_count), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&mock.advertisement_count), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&mock.application_inspection_count), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&mock.advertisement_inspection_count), ==, 1);
    g_mutex_lock(&mock.lock);
    owner = g_strdup(mock.application_owner);
    root = g_strdup(mock.application_path);
    advertisement = g_strdup(mock.advertisement_path);
    g_mutex_unlock(&mock.lock);
    inspect_application(&mock, owner, root, &pose_path, &clock_path);
    inspect_advertisement(&mock, owner, advertisement);
    g_print("[PASS] GATT application exports service/characteristics and encrypted flags; advertisement registered\n");

    read_clock(mock.connection, owner, clock_path, PEER, 0u, TRUE);
    g_assert_cmpuint(callbacks.read_count, ==, 1u);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, TRUE);
    g_assert_cmpuint(callbacks.write_count, ==, 1u);
    for (guint index = 0u; index < ARUCO_BLE_POSE_SIZE; ++index)
        g_assert_cmpuint(callbacks.last_write[index], ==, index + 1u);
    g_assert_cmpint(callbacks.last_timestamp, <=, g_get_monotonic_time());
    g_print("[PASS] actual D-Bus ReadValue and WriteValue dispatch exact bytes and monotonic receive timestamp\n");

    stranger = private_connection(mock.address);
    write_pose(stranger, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    read_clock(stranger, owner, clock_path, PEER, 0u, FALSE);
    write_pose(mock.connection, owner, pose_path, FOREIGN_PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    read_clock(mock.connection, owner, clock_path, FOREIGN_PEER, 0u, FALSE);
    write_pose(mock.connection, owner, pose_path, NULL, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    read_clock(mock.connection, owner, clock_path, NULL, 0u, FALSE);
    write_pose(mock.connection, owner, pose_path, PEER, 1u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    read_clock(mock.connection, owner, clock_path, PEER, 1u, FALSE);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "command", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", TRUE, ARUCO_BLE_POSE_SIZE, FALSE);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 23u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE - 1u, FALSE);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE + 1u, FALSE);
    g_assert_cmpuint(callbacks.write_count, ==, 1u);
    g_assert_cmpuint(callbacks.read_count, ==, 1u);
    g_print("[PASS] rejects non-BlueZ callers, wrong/missing peer, offsets, commands, prepared writes, small MTU and bad lengths\n");

    previous = callbacks.invalidation_count;
    emit_boolean_from(stranger, PEER, "org.bluez.Device1", "Connected", FALSE);
    read_clock(mock.connection, owner, clock_path, PEER, 0u, TRUE);
    (void)invoke(stranger, owner, advertisement, "org.bluez.LEAdvertisement1", "Release", NULL, FALSE);
    g_assert_cmpuint(callbacks.invalidation_count, ==, previous);
    g_assert_true(aruco_ble_is_ready(ble));
    g_print("[PASS] spoofed BlueZ property signals and advertisement release cannot invalidate state\n");

    previous = callbacks.invalidation_count;
    g_atomic_int_set(&mock.paired, FALSE);
    emit_boolean(&mock, PEER, "org.bluez.Device1", "Paired", FALSE);
    wait_invalidation(&callbacks, previous);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    read_clock(mock.connection, owner, clock_path, PEER, 0u, FALSE);
    g_atomic_int_set(&mock.paired, TRUE);
    emit_boolean(&mock, PEER, "org.bluez.Device1", "Paired", TRUE);
    previous = callbacks.invalidation_count;
    g_atomic_int_set(&mock.trusted, FALSE);
    emit_boolean(&mock, PEER, "org.bluez.Device1", "Trusted", FALSE);
    wait_invalidation(&callbacks, previous);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    g_atomic_int_set(&mock.trusted, TRUE);
    emit_boolean(&mock, PEER, "org.bluez.Device1", "Trusted", TRUE);
    previous = callbacks.invalidation_count;
    g_atomic_int_set(&mock.connected, FALSE);
    emit_boolean(&mock, PEER, "org.bluez.Device1", "Connected", FALSE);
    wait_invalidation(&callbacks, previous);
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    g_assert_cmpuint(callbacks.write_count, ==, 1u);
    g_atomic_int_set(&mock.connected, TRUE);
    emit_boolean(&mock, PEER, "org.bluez.Device1", "Connected", TRUE);
    read_clock(mock.connection, owner, clock_path, PEER, 0u, TRUE);
    g_print("[PASS] pairing/trust loss and disconnect invalidate state and block pose dispatch\n");

    callbacks.reject_write = TRUE;
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    callbacks.reject_write = FALSE;
    g_assert_cmpuint(callbacks.write_count, ==, 2u);
    callbacks.short_read = TRUE;
    read_clock(mock.connection, owner, clock_path, PEER, 0u, FALSE);
    callbacks.short_read = FALSE;
    g_print("[PASS] callback rejection and malformed clock results propagate as D-Bus errors\n");

    previous = callbacks.invalidation_count;
    g_atomic_int_set(&mock.powered, FALSE);
    emit_boolean(&mock, ADAPTER, "org.bluez.Adapter1", "Powered", FALSE);
    wait_invalidation(&callbacks, previous);
    g_assert_false(aruco_ble_is_ready(ble));
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    g_assert_cmpuint(callbacks.write_count, ==, 2u);
    g_atomic_int_set(&mock.powered, TRUE);
    emit_boolean(&mock, ADAPTER, "org.bluez.Adapter1", "Powered", TRUE);
    wait_ready(ble);
    read_clock(mock.connection, owner, clock_path, PEER, 0u, TRUE);
    g_print("[PASS] adapter power loss invalidates and blocks writes; power restoration registers again\n");

    previous = callbacks.invalidation_count;
    change_name(mock.connection, "ReleaseName");
    wait_invalidation(&callbacks, previous);
    g_assert_false(aruco_ble_is_ready(ble));
    g_assert_false(aruco_ble_needs_restart(ble));
    write_pose(mock.connection, owner, pose_path, PEER, 0u, 64u, "request", FALSE, ARUCO_BLE_POSE_SIZE, FALSE);
    change_name(mock.connection, "RequestName");
    wait_ready(ble);
    g_assert_cmpint(g_atomic_int_get(&mock.application_count), >=, 2);
    g_assert_cmpint(g_atomic_int_get(&mock.advertisement_count), >=, 2);
    read_clock(mock.connection, owner, clock_path, PEER, 0u, TRUE);
    g_print("[PASS] BlueZ ownership loss invalidates immediately and reappearance registers again\n");

    aruco_ble_stop(ble);
    g_assert_false(aruco_ble_is_ready(ble));
    g_assert_false(aruco_ble_needs_restart(ble));
    aruco_ble_free(ble);
    g_assert_true(g_dbus_connection_close_sync(stranger, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(stranger);
    g_main_context_invoke(mock.context, quit_mock, &mock);
    g_thread_join(thread);
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    g_free(owner);
    g_free(root);
    g_free(advertisement);
    g_free(pose_path);
    g_free(clock_path);
    g_free(mock.application_owner);
    g_free(mock.application_path);
    g_free(mock.advertisement_path);
    g_free(mock.address);
    g_cond_clear(&mock.condition);
    g_mutex_clear(&mock.lock);
    g_test_dbus_down(bus);
    g_object_unref(bus);
    if (saved_system_address != NULL)
        g_assert_true(g_setenv("DBUS_SYSTEM_BUS_ADDRESS", saved_system_address, TRUE));
    else
        g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
    g_free(saved_system_address);
    g_print("[PASS] private bus and mock cleaned up; no Bluetooth hardware or live IPC used\n");
    return 0;
}
