#define _POSIX_C_SOURCE 200809L

#include "aruco_receiver.h"
#include "aruco_ble.h"
#include "runtime_watchdog.h"

#include <errno.h>
#include <glib-unix.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

static ace_watchdog_t receiver_watchdog;

void aruco_receiver_watchdog_progress(unsigned slot, uint64_t timeout_ms)
{
    ace_watchdog_progress(&receiver_watchdog, slot, timeout_ms);
}

/* ========================================================================== */
/* ACP1/ACT1 바이트 해석과 최신 위치 검증 — 이 파일 내부에서만 사용한다.       */
/* ========================================================================== */

/*
 * Windows가 전송하는 바이트열은 C 구조체를 그대로 복사한 것이 아니다.
 * 모든 정수는 little-endian이며 아래 고정 offset으로 해석한다.
 * 촬영 시각은 clock characteristic으로 동기화한 Pi CLOCK_MONOTONIC ns다.
 * 따라서 PC 부팅 시각이나 UTC 값을 그대로 넣으면 수신할 수 없다.
 */
#define ARUCO_POSE_WIRE_SIZE 40U
#define ARUCO_CLOCK_WIRE_SIZE 20U
#define ARUCO_FLAG_VALID UINT16_C(1)
#define ARUCO_DEFAULT_STALE_MS 300U
#define ARUCO_DEFAULT_PEER_ADDRESS "88:F4:DA:74:9E:14"
#define ARUCO_POSITION_LIMIT_MM INT32_C(1000000)

typedef enum
{
    ARUCO_ACCEPTED = 0,
    ARUCO_INVALIDATED,
    ARUCO_BAD_FRAME,
    ARUCO_WRONG_VEHICLE,
    ARUCO_WRONG_EPOCH,
    ARUCO_OUT_OF_ORDER,
    ARUCO_BAD_TIME,
    ARUCO_EXPIRED
} aruco_receive_result_t;

/* 이 저장소는 GLib 수신 loop 한 곳만 소유한다. consumer는 값의 사본을 쓴다. */
typedef struct
{
    uint16_t vehicle_id;
    uint64_t epoch;
    uint64_t stale_ns;
    bool have_sequence;
    aruco_pose_t latest;
    uint64_t accepted;
    uint64_t invalidated;
    uint64_t rejected;
} aruco_pose_store_t;

static void aruco_pose_store_init(
    aruco_pose_store_t *store, uint16_t vehicle_id,
    uint64_t epoch, uint64_t stale_ns
);
static void aruco_pose_store_reset(aruco_pose_store_t *store, uint64_t new_epoch);
static aruco_receive_result_t aruco_pose_store_receive(
    aruco_pose_store_t *store, const uint8_t *data, size_t length,
    uint64_t received_ns
);
static bool aruco_pose_store_snapshot(
    const aruco_pose_store_t *store, uint64_t now_ns, aruco_pose_t *pose
);
static void aruco_clock_encode(
    uint8_t output[ARUCO_CLOCK_WIRE_SIZE], uint64_t epoch, uint64_t now_ns
);
static const char *aruco_receive_result_name(aruco_receive_result_t result);

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U)
        | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64(const uint8_t *data)
{
    return (uint64_t)read_u32(data) | ((uint64_t)read_u32(data + 4) << 32U);
}

static int32_t read_i32(const uint8_t *data)
{
    uint32_t value = read_u32(data);
    /* 범위 밖 unsigned -> signed cast에 의존하지 않고 음수 좌표를 복원한다. */
    if (value <= (uint32_t)INT32_MAX)
    {
        return (int32_t)value;
    }
    return -1 - (int32_t)(UINT32_MAX - value);
}

static void write_u64(uint8_t *output, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
    {
        output[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void aruco_pose_store_init(
    aruco_pose_store_t *store, uint16_t vehicle_id,
    uint64_t epoch, uint64_t stale_ns
)
{
    memset(store, 0, sizeof(*store));
    store->vehicle_id = vehicle_id;
    store->epoch = epoch;
    store->stale_ns = stale_ns;
}

static void aruco_pose_store_reset(aruco_pose_store_t *store, uint64_t new_epoch)
{
    /* reconnect 때 기존 전송 queue의 패킷이 새 session으로 섞이지 않게 한다. */
    store->epoch = new_epoch;
    store->have_sequence = false;
    memset(&store->latest, 0, sizeof(store->latest));
}

static aruco_receive_result_t reject(
    aruco_pose_store_t *store, aruco_receive_result_t reason
)
{
    ++store->rejected;
    return reason;
}

static aruco_receive_result_t aruco_pose_store_receive(
    aruco_pose_store_t *store, const uint8_t *data, size_t length,
    uint64_t received_ns
)
{
    aruco_pose_t next;
    uint16_t flags;
    uint64_t epoch;

    if (store == NULL)
    {
        return ARUCO_BAD_FRAME;
    }
    if (data == NULL || length != ARUCO_POSE_WIRE_SIZE
        || memcmp(data, "ACP1", 4U) != 0 || read_u16(data + 38) != 0U)
    {
        return reject(store, ARUCO_BAD_FRAME);
    }
    flags = read_u16(data + 6);
    if ((flags & (uint16_t)~ARUCO_FLAG_VALID) != 0U)
    {
        return reject(store, ARUCO_BAD_FRAME);
    }
    if (read_u16(data + 4) != store->vehicle_id)
    {
        return reject(store, ARUCO_WRONG_VEHICLE);
    }
    epoch = read_u64(data + 8);
    if (epoch == 0U || epoch != store->epoch)
    {
        return reject(store, ARUCO_WRONG_EPOCH);
    }
    memset(&next, 0, sizeof(next));
    next.sequence = read_u32(data + 16);
    next.capture_ns = read_u64(data + 20);
    next.received_ns = received_ns;
    next.x_mm = read_i32(data + 28);
    next.y_mm = read_i32(data + 32);
    next.heading_cdeg = read_u16(data + 36);
    next.valid = (flags & ARUCO_FLAG_VALID) != 0U;

    if (store->have_sequence)
    {
        uint32_t advance = next.sequence - store->latest.sequence;
        /* modulo wrap는 허용하되 중복/역순/2^31 이상 점프는 거부한다. */
        if (advance == 0U || advance >= UINT32_C(0x80000000)
            || next.capture_ns <= store->latest.capture_ns)
        {
            return reject(store, ARUCO_OUT_OF_ORDER);
        }
    }
    if (next.x_mm < -ARUCO_POSITION_LIMIT_MM
        || next.x_mm > ARUCO_POSITION_LIMIT_MM
        || next.y_mm < -ARUCO_POSITION_LIMIT_MM
        || next.y_mm > ARUCO_POSITION_LIMIT_MM
        || next.heading_cdeg >= 36000U)
    {
        return reject(store, ARUCO_BAD_FRAME);
    }
    if (received_ns == 0U || next.capture_ns == 0U
        || next.capture_ns > received_ns || store->stale_ns == 0U
        || next.capture_ns > UINT64_MAX - store->stale_ns)
    {
        return reject(store, ARUCO_BAD_TIME);
    }
    next.expires_ns = next.capture_ns + store->stale_ns;
    /* 새 순번의 stale/invalid도 경계를 전진시킨다. 이전 valid로 복귀하지 않는다. */
    store->have_sequence = true;
    store->latest = next;
    if (received_ns >= next.expires_ns)
    {
        store->latest.valid = false;
        return reject(store, ARUCO_EXPIRED);
    }
    if (!next.valid)
    {
        ++store->invalidated;
        return ARUCO_INVALIDATED;
    }
    ++store->accepted;
    return ARUCO_ACCEPTED;
}

static bool aruco_pose_store_snapshot(
    const aruco_pose_store_t *store, uint64_t now_ns, aruco_pose_t *pose
)
{
    if (store == NULL || pose == NULL || !store->latest.valid
        || now_ns < store->latest.received_ns || now_ns >= store->latest.expires_ns)
    {
        return false;
    }
    *pose = store->latest;
    return true;
}

static void aruco_clock_encode(
    uint8_t output[ARUCO_CLOCK_WIRE_SIZE], uint64_t epoch, uint64_t now_ns
)
{
    memcpy(output, "ACT1", 4U);
    write_u64(output + 4, epoch);
    write_u64(output + 12, now_ns);
}

static const char *aruco_receive_result_name(aruco_receive_result_t result)
{
    switch (result)
    {
    case ARUCO_ACCEPTED: return "accepted";
    case ARUCO_INVALIDATED: return "tracking-invalid";
    case ARUCO_BAD_FRAME: return "invalid-frame";
    case ARUCO_WRONG_VEHICLE: return "wrong-vehicle";
    case ARUCO_WRONG_EPOCH: return "clock-session-mismatch";
    case ARUCO_OUT_OF_ORDER: return "duplicate-or-out-of-order";
    case ARUCO_BAD_TIME: return "invalid-or-future-time";
    case ARUCO_EXPIRED: return "capture-expired";
    }
    return "unknown";
}

/* ========================================================================== */
/* 두 차량이 함께 사용하는 BLE 수신 loop                                     */
/* ========================================================================== */

/*
 * 공통 BLE loop는 패킷 검증과 시간 만료만 맡는다. 관찰차/사고차의 IPC 계약과
 * 하드웨어 소유권은 각각의 main 파일에 남긴다. PC가 보내는 위치는 이미 world
 * 좌표이므로 여기서 quaternion 계산이나 optical flow 적분을 다시 하지 않는다.
 */
typedef struct
{
    aruco_pose_store_t store;
    aruco_ble_t *ble;
    GMainLoop *loop;
    const aruco_receiver_callbacks_t *callbacks;
    void *consumer;
    uint64_t next_log_ns;
    uint64_t next_reject_log_ns;
    bool stopping;
    bool failed;
} receiver_runtime_t;

static uint64_t receiver_now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static bool receiver_new_epoch(uint64_t *epoch)
{
    ssize_t length;
    do
    {
        length = getrandom(epoch, sizeof(*epoch), 0);
    } while (length < 0 && errno == EINTR);
    return length == (ssize_t)sizeof(*epoch) && *epoch != 0U;
}

static void receiver_deliver(receiver_runtime_t *runtime, uint64_t now_ns)
{
    aruco_pose_t pose;
    bool valid = aruco_ble_is_ready(runtime->ble)
        && aruco_pose_store_snapshot(&runtime->store, now_ns, &pose);
    runtime->callbacks->sample(valid ? &pose : NULL, now_ns, runtime->consumer);
}

static gboolean receiver_write(
    const char *device_path, const guint8 *data, gsize length,
    gint64 monotonic_us, gpointer user_data, GError **error
)
{
    receiver_runtime_t *runtime = user_data;
    uint64_t now_ns = receiver_now_ns();
    aruco_receive_result_t result;
    (void)device_path;
    (void)monotonic_us;

    result = aruco_pose_store_receive(&runtime->store, data, length, now_ns);
    if (result == ARUCO_ACCEPTED || result == ARUCO_INVALIDATED)
    {
        /* invalid 패킷은 다음 50ms timer까지 기다리지 않고 즉시 전달한다. */
        receiver_deliver(runtime, now_ns);
        return TRUE;
    }
    if (result == ARUCO_EXPIRED)
    {
        receiver_deliver(runtime, now_ns);
    }
    if (now_ns >= runtime->next_reject_log_ns)
    {
        fprintf(stderr, "[ARUCO] rejected: %s (total=%llu)\n",
                aruco_receive_result_name(result),
                (unsigned long long)runtime->store.rejected);
        runtime->next_reject_log_ns = now_ns + UINT64_C(1000000000);
    }
    /* The frame was received and discarded; keep GATT alive for the next pose. */
    if (result == ARUCO_EXPIRED || result == ARUCO_OUT_OF_ORDER)
        return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "%s", aruco_receive_result_name(result));
    return FALSE;
}

static gboolean receiver_read_clock(
    const char *device_path, GByteArray *result, gpointer user_data, GError **error
)
{
    receiver_runtime_t *runtime = user_data;
    uint8_t bytes[ARUCO_CLOCK_WIRE_SIZE];
    uint64_t now_ns = receiver_now_ns();
    (void)device_path;
    if (now_ns == 0U || runtime->store.epoch == 0U)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Pi monotonic clock unavailable");
        return FALSE;
    }
    aruco_clock_encode(bytes, runtime->store.epoch, now_ns);
    g_byte_array_append(result, bytes, (guint)sizeof(bytes));
    return TRUE;
}

static void receiver_connection_lost(gpointer user_data)
{
    receiver_runtime_t *runtime = user_data;
    uint64_t epoch = 0U;
    if (runtime->stopping)
    {
        return;
    }
    if (!receiver_new_epoch(&epoch))
    {
        runtime->failed = true;
        g_main_loop_quit(runtime->loop);
    }
    aruco_pose_store_reset(&runtime->store, epoch);
    runtime->callbacks->sample(NULL, receiver_now_ns(), runtime->consumer);
}

static gboolean receiver_tick(gpointer user_data)
{
    receiver_runtime_t *runtime = user_data;
    if (aruco_ble_needs_restart(runtime->ble))
    {
        runtime->failed = true;
        g_main_loop_quit(runtime->loop);
        return G_SOURCE_CONTINUE;
    }
    uint64_t now_ns = receiver_now_ns();
    aruco_pose_t pose;
    bool ready = aruco_ble_is_ready(runtime->ble);
    bool valid = ready && aruco_pose_store_snapshot(&runtime->store, now_ns, &pose);
    receiver_deliver(runtime, now_ns);
    if (now_ns >= runtime->next_log_ns)
    {
        printf("[ARUCO] vehicle=%u BLE=%s pose=%s accepted=%llu rejected=%llu",
               (unsigned int)runtime->store.vehicle_id,
               ready ? "ready" : "waiting", valid ? "fresh" : "unavailable",
               (unsigned long long)runtime->store.accepted,
               (unsigned long long)runtime->store.rejected);
        if (valid)
        {
            printf(" X=%d Y=%d mm H=%.2f age=%.1fms",
                   (int)pose.x_mm, (int)pose.y_mm,
                   (double)pose.heading_cdeg / 100.0,
                   (double)(now_ns - pose.capture_ns) / 1000000.0);
        }
        putchar('\n');
        fflush(stdout);
        runtime->next_log_ns = now_ns + UINT64_C(1000000000);
    }
    aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_BLE, 10000U);
    return G_SOURCE_CONTINUE;
}

static gboolean receiver_signal(gpointer user_data)
{
    receiver_runtime_t *runtime = user_data;
    g_main_loop_quit(runtime->loop);
    return G_SOURCE_CONTINUE;
}

static bool receiver_resolve_vehicle_id(
    const gchar *hostname, const char *role, const gchar *explicit_id,
    gint *vehicle_id, GError **error
)
{
    static const struct
    {
        const char *hostname;
        const char *role;
        gint vehicle_id;
    } defaults[] = {
        {"craft2", "accident", 1},
        {"craft3", "witness", 2},
        {"craft4", "witness", 3},
        {"seongugi", "witness", 4},
        {"craft5", "witness", 4},
        {"craft6", "witness", 5}
    };
    const char *default_role = NULL;
    gint default_id = 0;

    for (size_t index = 0U; index < G_N_ELEMENTS(defaults); ++index)
    {
        if (g_strcmp0(hostname, defaults[index].hostname) == 0)
        {
            default_role = defaults[index].role;
            default_id = defaults[index].vehicle_id;
            break;
        }
    }
    if (explicit_id != NULL)
    {
        gchar *end = NULL;
        gint64 parsed;
        errno = 0;
        parsed = g_ascii_strtoll(explicit_id, &end, 10);
        if (errno == ERANGE || end == explicit_id || *end != '\0'
            || parsed < 1 || parsed > 65535)
        {
            g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                                "--vehicle-id는 1..65535 정수여야 합니다.");
            return false;
        }
        *vehicle_id = (gint)parsed;
        if (default_role != NULL && g_strcmp0(role, default_role) != 0)
        {
            g_message("hostname %s 기본 역할은 %s이며, 명시한 ID %d로 %s를 실행합니다.",
                      hostname, default_role, *vehicle_id, role);
        }
        return true;
    }
    if (default_role == NULL)
    {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "등록되지 않은 hostname %s입니다. --vehicle-id를 지정하세요.",
                    hostname == NULL ? "(unknown)" : hostname);
        return false;
    }
    if (g_strcmp0(role, default_role) != 0)
    {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "hostname %s에는 %s_aruco를 실행하세요. 수동 시험은 --vehicle-id를 지정하세요.",
                    hostname, default_role);
        return false;
    }
    *vehicle_id = default_id;
    return true;
}

int aruco_receiver_main(
    int argc, char **argv, const char *role,
    const aruco_receiver_callbacks_t *callbacks, void *context
)
{
    gint vehicle_id = 0;
    gint stale_ms = (gint)ARUCO_DEFAULT_STALE_MS;
    gchar *vehicle_id_text = NULL;
    gchar *peer = NULL;
    gchar *adapter = NULL;
    gchar *adapter_path = NULL;
    gchar *local_name = NULL;
    GError *error = NULL;
    GOptionContext *options;
    receiver_runtime_t runtime = {0};
    aruco_ble_config_t config = {0};
    guint timer_id = 0U;
    guint interrupt_id = 0U;
    guint terminate_id = 0U;
    bool consumer_started = false;
    uint64_t epoch = 0U;
    int exit_status = EXIT_FAILURE;
    GOptionEntry entries[] = {
        {"vehicle-id", 0, 0, G_OPTION_ARG_STRING, &vehicle_id_text,
         "차량 ID (1..65535), 생략하면 hostname의 기본값", "ID"},
        {"peer", 0, 0, G_OPTION_ARG_STRING, &peer,
         "pair/trust한 Windows PC 주소 (기본 " ARUCO_DEFAULT_PEER_ADDRESS ")", "AA:BB:CC:DD:EE:FF"},
        {"adapter", 0, 0, G_OPTION_ARG_STRING, &adapter,
         "BlueZ adapter (기본 hci0)", "hci0"},
        {"stale-ms", 0, 0, G_OPTION_ARG_INT, &stale_ms,
         "촬영 시각부터 위치 유효기간 (50..1000, 기본 300ms)", "MS"},
        {NULL, 0, 0, 0, NULL, NULL, NULL}
    };

    /* GLib 도움말도 터미널의 UTF-8 locale을 사용하도록 초기화한다. */
    (void)setlocale(LC_ALL, "");
    options = g_option_context_new("- Windows ArUco BLE position receiver");
    g_option_context_add_main_entries(options, entries, NULL);
    if (!g_option_context_parse(options, &argc, &argv, &error))
    {
        fprintf(stderr, "%s\n", error->message);
        goto cleanup;
    }
    if (argc != 1 || stale_ms < 50 || stale_ms > 1000 || callbacks == NULL
        || callbacks->sample == NULL)
    {
        fprintf(stderr, "실행 인자가 올바르지 않습니다. --help를 확인하세요.\n");
        goto cleanup;
    }
    if (!receiver_resolve_vehicle_id(g_get_host_name(), role, vehicle_id_text,
                                    &vehicle_id, &error))
    {
        fprintf(stderr, "%s\n", error->message);
        goto cleanup;
    }
    if (peer == NULL)
    {
        peer = g_strdup(ARUCO_DEFAULT_PEER_ADDRESS);
    }
    if (adapter == NULL)
    {
        adapter = g_strdup("hci0");
    }
    if (!g_str_has_prefix(adapter, "hci") || adapter[3] == '\0'
        || strspn(adapter + 3, "0123456789") != strlen(adapter + 3))
    {
        fputs("--adapter는 hci0 형식이어야 합니다.\n", stderr);
        goto cleanup;
    }
    if (!receiver_new_epoch(&epoch))
    {
        fputs("session epoch를 생성하지 못했습니다.\n", stderr);
        goto cleanup;
    }
    aruco_pose_store_init(&runtime.store, (uint16_t)vehicle_id, epoch,
                         (uint64_t)stale_ms * UINT64_C(1000000));
    runtime.callbacks = callbacks;
    runtime.consumer = context;
    runtime.loop = g_main_loop_new(NULL, FALSE);
    adapter_path = g_strdup_printf("/org/bluez/%s", adapter);
    /* 128bit service UUID + 이름이 legacy advertisement 크기에 들어가게 짧게 쓴다. */
    local_name = g_strdup_printf("ACE%u", (unsigned int)vehicle_id);
    config.adapter_path = adapter_path;
    config.local_name = local_name;
    config.allowed_peer_address = peer;
    runtime.ble = aruco_ble_new(&config, receiver_write, receiver_read_clock,
                              receiver_connection_lost, &runtime, &error);
    if (runtime.ble == NULL)
    {
        fprintf(stderr, "BLE 설정 오류: %s\n", error->message);
        goto cleanup;
    }
    signal(SIGPIPE, SIG_IGN);
    {
        static const char *const names[] = {
            "BLE dispatch", "BNO I/O and recovery", "collision detection", "accident IPC"
        };
        unsigned count = strcmp(role, "accident") == 0 ? 4U : 1U;
        if (ace_watchdog_start(&receiver_watchdog, names, count) != 0) {
            perror("sensor watchdog start");
            goto cleanup;
        }
    }
    if (callbacks->start != NULL && callbacks->start(context) != 0)
    {
        goto cleanup;
    }
    consumer_started = true;
    if (!aruco_ble_start(runtime.ble, &error))
    {
        fprintf(stderr, "BLE 시작 실패: %s\n", error->message);
        goto cleanup;
    }
    printf("=== %s ArUco receiver ===\nvehicle=%d name=%s peer=%s stale=%dms\n",
           role, vehicle_id, local_name, peer, stale_ms);
    puts("PC는 clock read 동기화 후 ACP1 40-byte pose를 보내세요 (ATT MTU >= 43).");
    timer_id = g_timeout_add(50U, receiver_tick, &runtime);
    interrupt_id = g_unix_signal_add(SIGINT, receiver_signal, &runtime);
    terminate_id = g_unix_signal_add(SIGTERM, receiver_signal, &runtime);
    g_main_loop_run(runtime.loop);
    exit_status = runtime.failed ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
    runtime.stopping = true;
    if (timer_id != 0U) g_source_remove(timer_id);
    if (interrupt_id != 0U) g_source_remove(interrupt_id);
    if (terminate_id != 0U) g_source_remove(terminate_id);
    aruco_ble_free(runtime.ble);
    if (consumer_started)
    {
        callbacks->sample(NULL, receiver_now_ns(), context);
        if (callbacks->stop != NULL) callbacks->stop(context);
    }
    if (runtime.loop != NULL) g_main_loop_unref(runtime.loop);
    g_option_context_free(options);
    g_clear_error(&error);
    g_free(vehicle_id_text);
    g_free(peer);
    g_free(adapter);
    g_free(adapter_path);
    g_free(local_name);
    ace_watchdog_stop(&receiver_watchdog);
    return exit_status;
}
