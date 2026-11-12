#include "aruco_ble.h"

#include <string.h>

#define APP_PATH "/org/ace/aruco"
#define SERVICE_PATH APP_PATH "/service0"
#define POSE_PATH SERVICE_PATH "/pose"
#define CLOCK_PATH SERVICE_PATH "/clock"
#define AD_PATH "/org/ace/aruco_advertisement"
#define BLUEZ "org.bluez"
#define DEVICE_IFACE "org.bluez.Device1"
#define GATT_IFACE "org.bluez.GattCharacteristic1"
#define GATT_MANAGER "org.bluez.GattManager1"
#define AD_MANAGER "org.bluez.LEAdvertisingManager1"
#define AD_IFACE "org.bluez.LEAdvertisement1"
#define SERVICE_IFACE "org.bluez.GattService1"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"

/* See BlueZ doc/org.bluez.{GattManager,GattCharacteristic,LEAdvertisement}.rst.
 * GDBus supplies Properties.Get/GetAll for registered readable properties. */
static const gchar introspection_xml[] =
    "<node>"
    " <interface name='org.freedesktop.DBus.ObjectManager'>"
    "  <method name='GetManagedObjects'>"
    "   <arg type='a{oa{sa{sv}}}' direction='out'/>"
    "  </method>"
    "  <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>"
    "  <signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>"
    " </interface>"
    " <interface name='org.bluez.GattService1'>"
    "  <property name='UUID' type='s' access='read'/>"
    "  <property name='Primary' type='b' access='read'/>"
    "  <property name='Includes' type='ao' access='read'/>"
    " </interface>"
    " <interface name='org.bluez.GattCharacteristic1'>"
    "  <method name='ReadValue'>"
    "   <arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/>"
    "  </method>"
    "  <method name='WriteValue'>"
    "   <arg type='ay' direction='in'/><arg type='a{sv}' direction='in'/>"
    "  </method>"
    "  <property name='UUID' type='s' access='read'/>"
    "  <property name='Service' type='o' access='read'/>"
    "  <property name='Flags' type='as' access='read'/>"
    " </interface>"
    " <interface name='org.bluez.LEAdvertisement1'>"
    "  <method name='Release'/>"
    "  <property name='Type' type='s' access='read'/>"
    "  <property name='ServiceUUIDs' type='as' access='read'/>"
    "  <property name='LocalName' type='s' access='read'/>"
    " </interface>"
    "</node>";

struct aruco_ble {
    guint refs;
    gchar *adapter_path, *local_name, *allowed_peer_address;
    gchar *service_uuid, *pose_uuid, *clock_uuid, *owner;
    aruco_ble_write_fn write_fn;
    aruco_ble_read_fn read_fn;
    aruco_ble_state_fn state_fn;
    gpointer user_data;
    GDBusConnection *connection;
    GDBusObjectManager *manager;
    GDBusNodeInfo *introspection;
    GCancellable *cancellable;
    guint exports[5], retry_source, generation;
    gboolean started, app_registered, ad_registered, registering;
    gboolean adapter_available, peer_connected;
};

typedef struct {
    aruco_ble_t *ble;
    guint generation;
    gboolean advertisement;
} Registration;

static void try_register(aruco_ble_t *ble);

static void release_ref(aruco_ble_t *ble)
{
    if (--ble->refs != 0)
        return;
    g_free(ble->adapter_path);
    g_free(ble->local_name);
    g_free(ble->allowed_peer_address);
    g_free(ble->service_uuid);
    g_free(ble->pose_uuid);
    g_free(ble->clock_uuid);
    g_free(ble->owner);
    g_clear_pointer(&ble->introspection, g_dbus_node_info_unref);
    g_free(ble);
}

static void invalidate(aruco_ble_t *ble)
{
    if (ble->started && ble->state_fn != NULL)
        ble->state_fn(ble->user_data);
}

static void reset_registration(aruco_ble_t *ble)
{
    ++ble->generation;
    if (ble->cancellable != NULL)
        g_cancellable_cancel(ble->cancellable);
    g_clear_object(&ble->cancellable);
    ble->cancellable = g_cancellable_new();
    ble->app_registered = FALSE;
    ble->ad_registered = FALSE;
    ble->registering = FALSE;
}

static gboolean proxy_boolean(GDBusProxy *proxy, const gchar *name)
{
    GVariant *value = g_dbus_proxy_get_cached_property(proxy, name);
    gboolean result = value != NULL &&
        g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
        g_variant_get_boolean(value);
    g_clear_pointer(&value, g_variant_unref);
    return result;
}

static gboolean proxy_string_equals(GDBusProxy *proxy, const gchar *name,
                                    const gchar *expected, gboolean address)
{
    GVariant *value = g_dbus_proxy_get_cached_property(proxy, name);
    gboolean result = FALSE;
    if (value != NULL &&
        (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
         g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH))) {
        const gchar *actual = g_variant_get_string(value, NULL);
        result = address ? g_ascii_strcasecmp(actual, expected) == 0 :
                           strcmp(actual, expected) == 0;
    }
    g_clear_pointer(&value, g_variant_unref);
    return result;
}

static gboolean allowed_device(aruco_ble_t *ble, GDBusProxy *proxy)
{
    return proxy_string_equals(proxy, "Address", ble->allowed_peer_address, TRUE) &&
           proxy_string_equals(proxy, "Adapter", ble->adapter_path, FALSE) &&
           proxy_boolean(proxy, "Paired") && proxy_boolean(proxy, "Trusted") &&
           proxy_boolean(proxy, "Connected") && !proxy_boolean(proxy, "Blocked");
}

static gboolean adapter_ready(aruco_ble_t *ble)
{
    GDBusInterface *adapter, *gatt, *advertising;
    gboolean result;
    if (ble->manager == NULL || ble->owner == NULL)
        return FALSE;
    adapter = g_dbus_object_manager_get_interface(ble->manager,
                    ble->adapter_path, "org.bluez.Adapter1");
    gatt = g_dbus_object_manager_get_interface(ble->manager,
                    ble->adapter_path, GATT_MANAGER);
    advertising = g_dbus_object_manager_get_interface(ble->manager,
                    ble->adapter_path, AD_MANAGER);
    result = adapter != NULL && gatt != NULL && advertising != NULL &&
             proxy_boolean(G_DBUS_PROXY(adapter), "Powered");
    g_clear_object(&adapter);
    g_clear_object(&gatt);
    g_clear_object(&advertising);
    return result;
}

static void refresh_state(aruco_ble_t *ble)
{
    gboolean connected = FALSE;
    gboolean available = adapter_ready(ble);
    GList *objects = ble->manager == NULL ? NULL :
                    g_dbus_object_manager_get_objects(ble->manager);
    for (GList *item = objects; item != NULL; item = item->next) {
        GDBusInterface *device = g_dbus_object_get_interface(item->data, DEVICE_IFACE);
        if (device != NULL && allowed_device(ble, G_DBUS_PROXY(device)))
            connected = TRUE;
        g_clear_object(&device);
    }
    g_list_free_full(objects, g_object_unref);
    if ((ble->peer_connected && !connected) ||
        (ble->adapter_available && !available))
        invalidate(ble);
    if (ble->adapter_available && !available)
        reset_registration(ble);
    ble->peer_connected = connected;
    ble->adapter_available = available;
}

static void objects_changed(GDBusObjectManager *manager, GDBusObject *object,
                            gpointer user_data)
{
    (void)manager;
    (void)object;
    refresh_state(user_data);
}

static void interfaces_changed(GDBusObjectManager *manager, GDBusObject *object,
                               GDBusInterface *interface, gpointer user_data)
{
    (void)manager;
    (void)object;
    (void)interface;
    refresh_state(user_data);
}

static void properties_changed(GDBusObjectManagerClient *manager,
                               GDBusObjectProxy *object, GDBusProxy *interface,
                               GVariant *changed, const gchar *const *invalidated,
                               gpointer user_data)
{
    (void)manager;
    (void)object;
    (void)interface;
    (void)changed;
    (void)invalidated;
    refresh_state(user_data);
}

static void owner_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    aruco_ble_t *ble = user_data;
    gchar *owner = g_dbus_object_manager_client_get_name_owner(
                            G_DBUS_OBJECT_MANAGER_CLIENT(object));
    (void)property;
    if (g_strcmp0(owner, ble->owner) == 0) {
        g_free(owner);
        return;
    }
    if (ble->owner != NULL)
        invalidate(ble);
    g_free(ble->owner);
    ble->owner = owner;
    ble->peer_connected = FALSE;
    ble->adapter_available = FALSE;
    reset_registration(ble);
    refresh_state(ble);
    try_register(ble);
}

static void bus_closed(GDBusConnection *connection, gboolean remote,
                       GError *error, gpointer user_data)
{
    aruco_ble_t *ble = user_data;
    (void)connection;
    (void)remote;
    reset_registration(ble);
    invalidate(ble);
    g_warning("ArUco BLE system bus closed; restart receiver: %s",
              error == NULL ? "connection closed" : error->message);
}

static GVariant *property_value(aruco_ble_t *ble, const gchar *path,
                                const gchar *interface, const gchar *property)
{
    if (strcmp(interface, SERVICE_IFACE) == 0) {
        if (strcmp(property, "UUID") == 0)
            return g_variant_new_string(ble->service_uuid);
        if (strcmp(property, "Primary") == 0)
            return g_variant_new_boolean(TRUE);
        if (strcmp(property, "Includes") == 0)
            return g_variant_new_objv(NULL, 0);
    } else if (strcmp(interface, GATT_IFACE) == 0) {
        gboolean pose = strcmp(path, POSE_PATH) == 0;
        if (strcmp(property, "UUID") == 0)
            return g_variant_new_string(pose ? ble->pose_uuid : ble->clock_uuid);
        if (strcmp(property, "Service") == 0)
            return g_variant_new_object_path(SERVICE_PATH);
        if (strcmp(property, "Flags") == 0) {
            const gchar *pose_flags[] = { "write", "encrypt-write", NULL };
            const gchar *clock_flags[] = { "read", "encrypt-read", NULL };
            return g_variant_new_strv(pose ? pose_flags : clock_flags, -1);
        }
    } else if (strcmp(interface, AD_IFACE) == 0) {
        if (strcmp(property, "Type") == 0)
            return g_variant_new_string("peripheral");
        if (strcmp(property, "LocalName") == 0)
            return g_variant_new_string(ble->local_name);
        if (strcmp(property, "ServiceUUIDs") == 0) {
            const gchar *uuids[] = { ble->service_uuid, NULL };
            return g_variant_new_strv(uuids, -1);
        }
    }
    return NULL;
}

static GVariant *get_property(GDBusConnection *connection, const gchar *sender,
                             const gchar *path, const gchar *interface,
                             const gchar *property, GError **error,
                             gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)error;
    return property_value(user_data, path, interface, property);
}

static void add_managed_object(aruco_ble_t *ble, GVariantBuilder *objects,
                               const gchar *path, const gchar *interface)
{
    GVariantBuilder interfaces, properties;
    GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface(
                                            ble->introspection, interface);
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    for (guint i = 0; info->properties[i] != NULL; ++i) {
        const gchar *name = info->properties[i]->name;
        g_variant_builder_add(&properties, "{sv}", name,
                              property_value(ble, path, interface, name));
    }
    g_variant_builder_add(&interfaces, "{s@a{sv}}", interface,
                          g_variant_builder_end(&properties));
    g_variant_builder_add(objects, "{o@a{sa{sv}}}", path,
                          g_variant_builder_end(&interfaces));
}

static void reject(GDBusMethodInvocation *invocation, const gchar *name,
                   const gchar *message)
{
    g_dbus_method_invocation_return_dbus_error(invocation, name, message);
}

static gboolean authorize(aruco_ble_t *ble, const gchar *sender,
                          GVariant *options, const gchar **device_path,
                          GDBusMethodInvocation *invocation)
{
    GDBusInterface *device;
    const gchar *link = NULL;
    gboolean allowed;
    if (!aruco_ble_is_ready(ble) ||
        g_strcmp0(sender, ble->owner) != 0 ||
        !g_variant_lookup(options, "device", "&o", device_path)) {
        reject(invocation, "org.bluez.Error.NotAuthorized",
               "Only BlueZ requests from the configured paired PC are accepted");
        return FALSE;
    }
    if (g_variant_lookup(options, "link", "&s", &link) && strcmp(link, "LE") != 0) {
        reject(invocation, "org.bluez.Error.NotSupported", "LE transport required");
        return FALSE;
    }
    device = g_dbus_object_manager_get_interface(ble->manager,
                                                *device_path, DEVICE_IFACE);
    allowed = device != NULL && allowed_device(ble, G_DBUS_PROXY(device));
    g_clear_object(&device);
    if (!allowed)
        reject(invocation, "org.bluez.Error.NotAuthorized",
               "PC must match allowed address and be paired, trusted and connected");
    return allowed;
}

static void write_pose(aruco_ble_t *ble, const gchar *sender, GVariant *parameters,
                       GDBusMethodInvocation *invocation)
{
    const gint64 received_us = g_get_monotonic_time();
    GVariant *bytes, *options;
    const gchar *device_path = NULL, *type = NULL;
    const guint8 *data;
    guint16 offset = 0, mtu = 0;
    gboolean prepare = FALSE;
    gsize length;
    GError *error = NULL;
    g_variant_get(parameters, "(@ay@a{sv})", &bytes, &options);
    if (!authorize(ble, sender, options, &device_path, invocation))
        goto out;
    g_variant_lookup(options, "offset", "q", &offset);
    g_variant_lookup(options, "prepare-authorize", "b", &prepare);
    g_variant_lookup(options, "type", "&s", &type);
    if (offset != 0 || prepare || (type != NULL && strcmp(type, "request") != 0)) {
        reject(invocation, "org.bluez.Error.NotSupported",
               "Use one write-with-response request; no offsets or reliable writes");
        goto out;
    }
    data = g_variant_get_fixed_array(bytes, &length, sizeof(guint8));
    if (length != ARUCO_BLE_POSE_SIZE ||
        (g_variant_lookup(options, "mtu", "q", &mtu) &&
         mtu < ARUCO_BLE_POSE_SIZE + 3)) {
        reject(invocation, "org.bluez.Error.InvalidValueLength",
               "Pose must be 40 bytes in one ATT request (MTU at least 43)");
        goto out;
    }
    if (!ble->write_fn(device_path, data, length, received_us,
                       ble->user_data, &error)) {
        reject(invocation, "org.bluez.Error.Failed",
               error == NULL ? "Pose rejected" : error->message);
    } else {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    g_clear_error(&error);
out:
    g_variant_unref(bytes);
    g_variant_unref(options);
}

static void read_clock(aruco_ble_t *ble, const gchar *sender, GVariant *parameters,
                       GDBusMethodInvocation *invocation)
{
    GVariant *options;
    const gchar *device_path = NULL;
    guint16 offset = 0;
    GByteArray *result;
    GError *error = NULL;
    g_variant_get(parameters, "(@a{sv})", &options);
    if (!authorize(ble, sender, options, &device_path, invocation))
        goto out;
    g_variant_lookup(options, "offset", "q", &offset);
    if (offset != 0) {
        reject(invocation, "org.bluez.Error.InvalidOffset", "Clock offset must be zero");
        goto out;
    }
    result = g_byte_array_sized_new(ARUCO_BLE_CLOCK_SIZE);
    if (!ble->read_fn(device_path, result, ble->user_data, &error) ||
        result->len != ARUCO_BLE_CLOCK_SIZE) {
        reject(invocation, "org.bluez.Error.Failed",
               error == NULL ? "Clock unavailable" : error->message);
    } else {
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                            result->data, result->len, sizeof(guint8));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)", bytes));
    }
    g_clear_error(&error);
    g_byte_array_unref(result);
out:
    g_variant_unref(options);
}

static void method_call(GDBusConnection *connection, const gchar *sender,
                         const gchar *path, const gchar *interface,
                         const gchar *method, GVariant *parameters,
                         GDBusMethodInvocation *invocation, gpointer user_data)
{
    aruco_ble_t *ble = user_data;
    (void)connection;
    if (strcmp(interface, OBJECT_MANAGER) == 0 &&
        strcmp(method, "GetManagedObjects") == 0) {
        GVariantBuilder objects;
        g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        add_managed_object(ble, &objects, SERVICE_PATH, SERVICE_IFACE);
        add_managed_object(ble, &objects, POSE_PATH, GATT_IFACE);
        add_managed_object(ble, &objects, CLOCK_PATH, GATT_IFACE);
        g_dbus_method_invocation_return_value(invocation,
                    g_variant_new("(a{oa{sa{sv}}})", &objects));
    } else if (strcmp(interface, AD_IFACE) == 0 && strcmp(method, "Release") == 0) {
        if (ble->owner == NULL || g_strcmp0(sender, ble->owner) != 0) {
            reject(invocation, "org.bluez.Error.NotAuthorized", "BlueZ caller required");
            return;
        }
        /* Release can race the asynchronous registration reply. Retire that
         * generation so a late success cannot claim advertising is ready. */
        reset_registration(ble);
        invalidate(ble);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (strcmp(interface, GATT_IFACE) == 0 &&
               strcmp(path, POSE_PATH) == 0 && strcmp(method, "WriteValue") == 0) {
        write_pose(ble, sender, parameters, invocation);
    } else if (strcmp(interface, GATT_IFACE) == 0 &&
               strcmp(path, CLOCK_PATH) == 0 && strcmp(method, "ReadValue") == 0) {
        read_clock(ble, sender, parameters, invocation);
    } else {
        reject(invocation, "org.bluez.Error.NotPermitted", "Operation not supported");
    }
}

static const GDBusInterfaceVTable vtable = {
    .method_call = method_call,
    .get_property = get_property
};

static void registration_finished(GObject *source, GAsyncResult *result,
                                   gpointer user_data)
{
    Registration *registration = user_data;
    aruco_ble_t *ble = registration->ble;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);
    if (ble->started && ble->generation == registration->generation) {
        gchar *remote_error = error == NULL ? NULL : g_dbus_error_get_remote_error(error);
        gboolean success = reply != NULL ||
            g_strcmp0(remote_error, "org.bluez.Error.AlreadyExists") == 0;
        ble->registering = FALSE;
        if (success) {
            if (registration->advertisement) {
                ble->ad_registered = TRUE;
                g_message("ArUco BLE ready: %s on %s", ble->local_name, ble->adapter_path);
            } else {
                ble->app_registered = TRUE;
                try_register(ble);
            }
        } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            invalidate(ble);
            g_warning("ArUco BLE %s registration failed (retry in 3 s): %s",
                      registration->advertisement ? "advertisement" : "GATT",
                      error == NULL ? "unknown error" : error->message);
        }
        g_free(remote_error);
    }
    g_clear_pointer(&reply, g_variant_unref);
    g_clear_error(&error);
    release_ref(ble);
    g_free(registration);
}

static void try_register(aruco_ble_t *ble)
{
    Registration *registration;
    GVariantBuilder options;
    gboolean advertisement;
    if (!ble->started || ble->registering || ble->ad_registered ||
        ble->owner == NULL || !ble->adapter_available ||
        g_dbus_connection_is_closed(ble->connection))
        return;
    advertisement = ble->app_registered;
    registration = g_new0(Registration, 1);
    registration->ble = ble;
    registration->generation = ble->generation;
    registration->advertisement = advertisement;
    ++ble->refs;
    ble->registering = TRUE;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    /* Async calls are essential: BlueZ calls back into our object manager and
     * properties while processing registration on this same main context. */
    g_dbus_connection_call(ble->connection, ble->owner, ble->adapter_path,
        advertisement ? AD_MANAGER : GATT_MANAGER,
        advertisement ? "RegisterAdvertisement" : "RegisterApplication",
        g_variant_new("(oa{sv})", advertisement ? AD_PATH : APP_PATH, &options),
        G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000,
        ble->cancellable, registration_finished, registration);
}

static gboolean retry_registration(gpointer user_data)
{
    aruco_ble_t *ble = user_data;
    refresh_state(ble);
    try_register(ble);
    return G_SOURCE_CONTINUE;
}

static gboolean valid_address(const gchar *address)
{
    if (address == NULL || strlen(address) != 17)
        return FALSE;
    for (guint i = 0; i < 17; ++i) {
        if (i % 3 == 2 ? address[i] != ':' : !g_ascii_isxdigit(address[i]))
            return FALSE;
    }
    return TRUE;
}

aruco_ble_t *aruco_ble_new(const aruco_ble_config_t *config,
                         aruco_ble_write_fn write_fn,
                         aruco_ble_read_fn read_fn,
                         aruco_ble_state_fn state_fn,
                         gpointer user_data, GError **error)
{
    aruco_ble_t *ble;
    const gchar *name, *adapter, *service, *pose, *clock;
    if (config == NULL || !valid_address(config->allowed_peer_address) ||
        write_fn == NULL || read_fn == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Valid allowed PC Bluetooth address and read/write callbacks required");
        return NULL;
    }
    name = config->local_name == NULL ? "ACE-Pose" : config->local_name;
    adapter = config->adapter_path == NULL ? "/org/bluez/hci0" : config->adapter_path;
    service = config->service_uuid == NULL ? ARUCO_BLE_SERVICE_UUID : config->service_uuid;
    pose = config->pose_uuid == NULL ? ARUCO_BLE_POSE_UUID : config->pose_uuid;
    clock = config->clock_uuid == NULL ? ARUCO_BLE_CLOCK_UUID : config->clock_uuid;
    if (strlen(name) == 0 || strlen(name) > 10 || !g_str_is_ascii(name) ||
        !g_variant_is_object_path(adapter) || !g_str_has_prefix(adapter, "/org/bluez/") ||
        !g_uuid_string_is_valid(service) || !g_uuid_string_is_valid(pose) ||
        !g_uuid_string_is_valid(clock) || g_ascii_strcasecmp(pose, clock) == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Need 1-10 byte ASCII name, BlueZ adapter path and valid distinct UUIDs");
        return NULL;
    }
    ble = g_new0(aruco_ble_t, 1);
    ble->refs = 1;
    ble->adapter_path = g_strdup(adapter);
    ble->local_name = g_strdup(name);
    ble->allowed_peer_address = g_ascii_strup(config->allowed_peer_address, -1);
    ble->service_uuid = g_strdup(service);
    ble->pose_uuid = g_strdup(pose);
    ble->clock_uuid = g_strdup(clock);
    ble->write_fn = write_fn;
    ble->read_fn = read_fn;
    ble->state_fn = state_fn;
    ble->user_data = user_data;
    ble->introspection = g_dbus_node_info_new_for_xml(introspection_xml, error);
    if (ble->introspection == NULL) {
        release_ref(ble);
        return NULL;
    }
    return ble;
}

gboolean aruco_ble_start(aruco_ble_t *ble, GError **error)
{
    const gchar *paths[] = { APP_PATH, SERVICE_PATH, POSE_PATH, CLOCK_PATH, AD_PATH };
    const gchar *interfaces[] = { OBJECT_MANAGER, SERVICE_IFACE, GATT_IFACE,
                                  GATT_IFACE, AD_IFACE };
    gchar *address;
    g_return_val_if_fail(ble != NULL, FALSE);
    if (ble->started)
        return TRUE;
    address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    if (address == NULL)
        return FALSE;
    /* A private connection makes closing this transport unregister its own
     * BlueZ objects without disconnecting another module's shared system bus. */
    ble->connection = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, error);
    g_free(address);
    if (ble->connection == NULL)
        return FALSE;
    g_dbus_connection_set_exit_on_close(ble->connection, FALSE);
    ble->manager = g_dbus_object_manager_client_new_sync(ble->connection,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, BLUEZ, "/",
        NULL, NULL, NULL, NULL, error);
    if (ble->manager == NULL)
        goto fail;
    for (guint i = 0; i < G_N_ELEMENTS(paths); ++i) {
        ble->exports[i] = g_dbus_connection_register_object(ble->connection,
            paths[i], g_dbus_node_info_lookup_interface(ble->introspection, interfaces[i]),
            &vtable, ble, NULL, error);
        if (ble->exports[i] == 0)
            goto fail;
    }
    ble->started = TRUE;
    reset_registration(ble);
    ble->owner = g_dbus_object_manager_client_get_name_owner(
                                            G_DBUS_OBJECT_MANAGER_CLIENT(ble->manager));
    g_signal_connect(ble->connection, "closed", G_CALLBACK(bus_closed), ble);
    g_signal_connect(ble->manager, "notify::name-owner", G_CALLBACK(owner_changed), ble);
    g_signal_connect(ble->manager, "object-added", G_CALLBACK(objects_changed), ble);
    g_signal_connect(ble->manager, "object-removed", G_CALLBACK(objects_changed), ble);
    g_signal_connect(ble->manager, "interface-added", G_CALLBACK(interfaces_changed), ble);
    g_signal_connect(ble->manager, "interface-removed", G_CALLBACK(interfaces_changed), ble);
    g_signal_connect(ble->manager, "interface-proxy-properties-changed",
                     G_CALLBACK(properties_changed), ble);
    ble->retry_source = g_timeout_add_seconds(3, retry_registration, ble);
    refresh_state(ble);
    try_register(ble);
    if (ble->owner == NULL || !ble->adapter_available)
        g_message("ArUco BLE waiting for powered adapter %s with GATT and advertising support",
                   ble->adapter_path);
    return TRUE;
fail:
    aruco_ble_stop(ble);
    return FALSE;
}

gboolean aruco_ble_is_ready(const aruco_ble_t *ble)
{
    return ble != NULL && ble->started && ble->adapter_available && ble->app_registered &&
           ble->ad_registered && ble->owner != NULL &&
           ble->connection != NULL && !g_dbus_connection_is_closed(ble->connection);
}

gboolean aruco_ble_needs_restart(const aruco_ble_t *ble)
{
    return ble != NULL && ble->started && ble->connection != NULL &&
           g_dbus_connection_is_closed(ble->connection);
}

void aruco_ble_stop(aruco_ble_t *ble)
{
    if (ble == NULL)
        return;
    if (ble->started)
        invalidate(ble);
    ble->started = FALSE;
    ++ble->generation;
    if (ble->retry_source != 0) {
        g_source_remove(ble->retry_source);
        ble->retry_source = 0;
    }
    if (ble->cancellable != NULL)
        g_cancellable_cancel(ble->cancellable);
    g_clear_object(&ble->cancellable);
    if (ble->manager != NULL)
        g_signal_handlers_disconnect_by_data(ble->manager, ble);
    g_clear_object(&ble->manager);
    if (ble->connection != NULL) {
        g_signal_handlers_disconnect_by_data(ble->connection, ble);
        for (guint i = 0; i < G_N_ELEMENTS(ble->exports); ++i) {
            if (ble->exports[i] != 0)
                g_dbus_connection_unregister_object(ble->connection, ble->exports[i]);
            ble->exports[i] = 0;
        }
        if (!g_dbus_connection_is_closed(ble->connection))
            g_dbus_connection_close_sync(ble->connection, NULL, NULL);
    }
    g_clear_object(&ble->connection);
    g_clear_pointer(&ble->owner, g_free);
    ble->app_registered = ble->ad_registered = ble->registering = FALSE;
    ble->adapter_available = ble->peer_connected = FALSE;
}

void aruco_ble_free(aruco_ble_t *ble)
{
    if (ble == NULL)
        return;
    aruco_ble_stop(ble);
    release_ref(ble);
}
