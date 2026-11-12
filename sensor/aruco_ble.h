#ifndef ARUCO_BLE_H
#define ARUCO_BLE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define ARUCO_BLE_SERVICE_UUID "7b9c0001-6a54-4f1b-9f65-2c7b8d1e4a90"
#define ARUCO_BLE_POSE_UUID    "7b9c0002-6a54-4f1b-9f65-2c7b8d1e4a90"
#define ARUCO_BLE_CLOCK_UUID   "7b9c0003-6a54-4f1b-9f65-2c7b8d1e4a90"
#define ARUCO_BLE_POSE_SIZE 40u
#define ARUCO_BLE_CLOCK_SIZE 20u

typedef struct aruco_ble aruco_ble_t;

typedef struct {
    const char *adapter_path;         /* NULL: /org/bluez/hci0 */
    const char *local_name;           /* NULL: ACE-Pose; at most 10 ASCII bytes */
    const char *allowed_peer_address; /* Required paired PC identity address */
    const char *service_uuid;         /* NULL: ARUCO_BLE_SERVICE_UUID */
    const char *pose_uuid;            /* NULL: ARUCO_BLE_POSE_UUID */
    const char *clock_uuid;           /* NULL: ARUCO_BLE_CLOCK_UUID */
} aruco_ble_config_t;

/* Invoked in the default GLib main context. Input is borrowed for the call.
 * A TRUE write result acknowledges protocol acceptance, not vehicle actuation.
 * The timestamp is CLOCK_MONOTONIC in microseconds, sampled upon WriteValue.
 * Callbacks must not block, stop/free the transport, or retain borrowed input. */
typedef gboolean (*aruco_ble_write_fn)(const char *device_path,
                                      const guint8 *data, gsize length,
                                      gint64 monotonic_us, gpointer user_data,
                                      GError **error);
/* Append exactly ARUCO_BLE_CLOCK_SIZE bytes to the initially empty result. */
typedef gboolean (*aruco_ble_read_fn)(const char *device_path,
                                     GByteArray *result, gpointer user_data,
                                     GError **error);
/* Invalidates application state after allowed-peer disconnect/security loss,
 * BlueZ loss/restart, adapter removal/power loss, or registration failure. */
typedef void (*aruco_ble_state_fn)(gpointer user_data);

/* Config strings are copied. All API calls must use the same thread/default
 * main context. Encryption plus Paired/Trusted/Connected are always required.
 * No adapter settings, pairing, trust, or system services are changed here. */
aruco_ble_t *aruco_ble_new(const aruco_ble_config_t *config,
                         aruco_ble_write_fn write_fn,
                         aruco_ble_read_fn read_fn,
                         aruco_ble_state_fn state_fn,
                         gpointer user_data, GError **error);
/* Starts local D-Bus export. Registration completes asynchronously; call
 * is_ready for availability. BlueZ restart is retried every three seconds.
 * A system bus disconnect makes needs_restart return TRUE for clean shutdown.
 * BLE writes must fit one ATT request: 40 bytes requires negotiated MTU >= 43. */
gboolean aruco_ble_start(aruco_ble_t *ble, GError **error);
gboolean aruco_ble_is_ready(const aruco_ble_t *ble);
/* TRUE only while a started transport has lost its system bus connection. */
gboolean aruco_ble_needs_restart(const aruco_ble_t *ble);
void aruco_ble_stop(aruco_ble_t *ble);
void aruco_ble_free(aruco_ble_t *ble);

G_END_DECLS

#endif
