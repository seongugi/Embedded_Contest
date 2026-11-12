#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * ==================================================
 * 사고차량용 ArUco 수신 + BNO086 충돌 프로세스
 * ==================================================
 *
 * main, ArUco 위치 전달, BNO 충돌 판정과 P2P·영상 IPC를 이 파일에서 처리한다.
 *
 * Thread 역할:
 *
 *     BNO086 I/O Thread
 *          - BNO086을 유일하게 소유
 *          - startup, feature 설정, SH-2 packet drain
 *
 *     BLE 수신 Main Thread
 *          - PC가 구한 ArUco testbed X/Y/heading을 한 번에 공개
 *          - 촬영 시각과 만료 시각을 그대로 보존
 *          - 검출 실패나 연결 단절을 위치 publication에 반영
 *
 *     Collision Detector Thread
 *          - BNO086 event ring의 독립 cursor로 모든 가속도 sample을 관측
 *          - 실측으로 확정한 복수 지표가 시간 창 안에서 일치할 때 1회 판정
 *          - 사고 인식 후 처음 확보한 실측 위치를 선택
 *
 *     IPC Sender Thread
 *          - 판정 경로와 분리된 bounded queue에서 사고 이벤트를 재전송
 *          - 영상은 즉시, P2P는 위치 확보 후 같은 사고 ID로 전달
 *          - 대상별 성공 여부를 유지하고 미전송 대상만 재시도
 *
 * 이 모듈은 사고차량 전용이다. BNO 충돌 판정은 마커 인식 여부와 독립적이다.
 * 영상 프로세스에 즉시 알리고 사고 후 위치를 확보하면 P2P에 보낸다. BNO 장치는 I/O Thread 한 곳에서
 * 소유하고 충돌 Thread는 독립 event cursor로 가속도 sample을 읽는다.
 *
 * 쉽게 말하면 이 프로세스에는 서로 다른 두 일이 있다.
 *
 * 1. PC 카메라가 알려준 차량의 위치와 방향이 아직 유효한지 확인한다.
 * 2. BNO 충돌을 영상 IPC에 먼저 알리고, 사고 후 위치를 확보하면 P2P에도 알린다.
 *
 * 마커 소실은 충돌 evidence/latch나 대기 중 사고를 지우지 않는다.
 * 대기 중 반복 충격은 첫 사고로 묶는다. IPC가 느려져도 가속도 읽기가
 * 밀리지 않도록 사고 송신은 별도 queue와 Thread에서 처리한다.
 */

#include "bno086.h"
#include "aruco_receiver.h"
#include "ipc.h"
#include "protocol.h"
#include "accident_protocol.h"
#include "video_processor_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/un.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

/* 사고차 안에서만 쓰는 상태와 함수는 별도 public 헤더로 노출하지 않는다. */
/* ArUco 위치를 받는 사고차 runtime이다. BNO 가속도와 사고 IPC를 맡는다. */
typedef struct accident_collision accident_collision_t;

/* 시작 실패 시 내부 자원까지 정리하고 NULL과 오류 설명을 돌려준다. */
static accident_collision_t *accident_collision_start(char *error, size_t error_size);
static void accident_collision_stop(accident_collision_t *runtime);

/*
 * testbed mm 좌표, 0도=+Y/시계방향 증가 규약이다. 시각은 Pi CLOCK_MONOTONIC이다.
 * 같은 pose를 다시 전달해도 촬영 시각/만료 시각은 원래 값 그대로 유지한다.
 */
static void accident_collision_update_pose(
    accident_collision_t *runtime,
    double x_mm,
    double y_mm,
    double heading_deg,
    uint64_t capture_ns,
    uint64_t expires_ns
);
/* 검출 실패나 연결 단절은 위치만 무효화한다. IMU 판정/대기 사고는 유지한다. */
static void accident_collision_invalidate_pose(accident_collision_t *runtime);
static bool accident_collision_detection_enabled(void);

#define SENSOR_BNO_UPDATE_INTERVAL_US      2000U
#define SENSOR_ACCEL_INTERVAL_US           10000U
#define SENSOR_BNO_RETRY_US                500000U
#define SENSOR_BNO_REPORT_TIMEOUT_NS       UINT64_C(2000000000)
#define SENSOR_P2P_SOCKET_PATH              "/tmp/p2p.sock"
#define SENSOR_STATION_ID                   1U
#define SENSOR_IPC_QUEUE_CAPACITY           8U
#define SENSOR_DV_QUEUE_CAPACITY            256U
#define SENSOR_POSE_HISTORY_CAPACITY         256U
#define SENSOR_POSE_HISTORY_NS               UINT64_C(2000000000)
#define SENSOR_POSE_MATCH_WAIT_NS            UINT64_C(2000000000)
#define SENSOR_ITS_EPOCH_UNIX_MS            UINT64_C(1072915200000)
#define SENSOR_ITS_LEAP_SECONDS_MS          UINT64_C(5000)

#define COLLISION_METRIC_RAW_ACCEL          (UINT32_C(1) << 0)
#define COLLISION_METRIC_LINEAR_ACCEL       (UINT32_C(1) << 1)
#define COLLISION_METRIC_RAW_JERK           (UINT32_C(1) << 2)
#define COLLISION_METRIC_LINEAR_JERK        (UINT32_C(1) << 3)
#define COLLISION_METRIC_LINEAR_DELTA_V     (UINT32_C(1) << 4)
#define COLLISION_METRIC_ALL                ((UINT32_C(1) << 5) - 1U)


/* BNO 장치의 startup과 지속 packet drain을 전담하고 적용 interval을 공개한다. */
typedef struct
{
    bno086_t imu;

    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t start_condition;

    bool thread_created;
    bool start_finished;
    bool ready;
    bool stop_requested;

    uint32_t raw_accel_interval_us;
    uint32_t linear_accel_interval_us;
    uint32_t gyro_interval_us;
    uint64_t ready_generation;
    uint64_t ready_since_ns;

    char start_error[BNO086_ERROR_TEXT_SIZE];
} imu_worker_t;

typedef enum
{
    COLLISION_DV_WINDOW_20_MS = 20,
    COLLISION_DV_WINDOW_50_MS = 50,
    COLLISION_DV_WINDOW_100_MS = 100,
    COLLISION_DV_WINDOW_200_MS = 200
} collision_dv_window_t;

typedef struct
{
    /* 어떤 지표를 쓸지와 RC-car 실측 후 확정할 모든 시간/크기 임계값이다. */
    bool enabled;
    uint32_t metric_mask;
    unsigned int minimum_evidence_count;
    collision_dv_window_t delta_v_window;
    unsigned int coincidence_window_ms;
    unsigned int cooldown_ms;
    unsigned int quiet_rearm_ms;
    bool require_position;
    unsigned int camera_read_delay_ms;

    double raw_accel_threshold_mps2;
    double linear_accel_threshold_mps2;
    double raw_jerk_threshold_mps3;
    double linear_jerk_threshold_mps3;
    double linear_delta_v_threshold_mps;
} collision_config_t;

/* 실측 세션 session_20260727_153124 기준: Linear jerk와 20ms delta-V가
 * 100ms 안에 함께 임계값을 넘으면 충돌로 판정한다. */
static const collision_config_t collision_config = {
    .enabled = true,
    .metric_mask = COLLISION_METRIC_LINEAR_JERK | COLLISION_METRIC_LINEAR_DELTA_V,
    .minimum_evidence_count = 2U,
    .delta_v_window = COLLISION_DV_WINDOW_20_MS,
    .coincidence_window_ms = 100U,
    .cooldown_ms = 2000U,
    .quiet_rearm_ms = 1000U,
    .require_position = true,
    .camera_read_delay_ms = 357U, /* Windows 4K 카메라 LED 점멸 실측 고정 지연. */
    .raw_accel_threshold_mps2 = 0.0,
    .linear_accel_threshold_mps2 = 0.0,
    .raw_jerk_threshold_mps3 = 0.0,
    .linear_jerk_threshold_mps3 = 1600.0,
    .linear_delta_v_threshold_mps = 0.16
};

typedef struct
{
    double x;
    double y;
    double z;
} collision_vector3_t;

typedef struct
{
    /* 한 report stream의 직전 sample로 jerk의 연속성을 검증한다. */
    bool valid;
    uint8_t sequence;
    uint64_t generation;
    uint64_t monotonic_ns;
    collision_vector3_t value;
} collision_stream_state_t;

typedef struct
{
    /* Linear acceleration을 사다리꼴 적분한 고정 시간창 delta-V 벡터다. */
    collision_vector3_t values[SENSOR_DV_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    size_t limit;
    collision_vector3_t sum;
} collision_delta_v_window_t;

typedef struct
{
    /* 검출 latch, 최근 evidence와 raw/linear stream을 한 lock 아래 보관한다. */
    collision_config_t config;
    bool armed;
    bool latched;
    uint64_t latched_at_ns;
    uint64_t last_evidence_ns;
    uint64_t last_event_ns;
    bool generation_valid;
    uint64_t generation;
    uint64_t evidence_ns[5];
    double evidence_values[5];
    collision_stream_state_t raw;
    collision_stream_state_t linear;
    collision_delta_v_window_t delta_v;
    double raw_interval_s;
    double linear_interval_s;
} collision_detector_state_t;

typedef struct
{
    double x_cm;
    double y_cm;
    double heading_deg;
    uint64_t capture_ns;
    uint64_t received_ns;
    uint64_t expires_ns;
} position_sample_t;

typedef struct
{
    /* BLE 수신 Thread가 쓰고 충돌 Thread가 일관된 사본으로 읽는 위치 상태다. */
    pthread_mutex_t mutex;
    bool initialized;
    bool valid;
    bool heading_valid;
    double x_cm;
    double y_cm;
    double heading_deg;
    uint64_t capture_ns;
    uint64_t expires_ns;
    uint64_t generation;
    position_sample_t history[SENSOR_POSE_HISTORY_CAPACITY];
    size_t history_head;
    size_t history_count;
} position_publication_t;

typedef struct
{
    accident_ipc_event_t payload;
    uint64_t monotonic_ns;
} collision_ipc_item_t;

typedef struct
{
    /* Collision Thread 소유. 좌표를 기다릴 때도 첫 사고 시각을 바꾸지 않는다. */
    bool active;
    bool video_queued;
    bool pose_resolved;
    bool pose_confirmed;
    bool p2p_queued;
    bool candidate_valid;
    position_sample_t candidate;
    uint64_t candidate_measurement_ns;
    collision_ipc_item_t item;
} collision_pending_t;

typedef struct
{
    collision_ipc_item_t items[SENSOR_IPC_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} collision_ipc_queue_t;

typedef struct
{
    /* 대상별 bounded queue로 영상·P2P 재시도를 독립시켜 센서 sample 처리를 막지 않는다. */
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    collision_ipc_queue_t p2p;
    collision_ipc_queue_t video;
    bool initialized;
    bool thread_created;
    bool stop_requested;
    uint32_t next_sequence;
    uint64_t enqueue_blocked;
    uint64_t sent; /* 성공한 대상별 IPC 전송 횟수. */
} collision_ipc_worker_t;

typedef struct
{
    /* BNO event ring의 독립 cursor로 가속도 sample을 순서대로 소비한다. */
    pthread_t thread;
    pthread_mutex_t mutex;
    imu_worker_t *imu_worker;
    position_publication_t *position;
    collision_ipc_worker_t *ipc_worker;
    collision_detector_state_t detector;
    bool initialized;
    bool thread_created;
    bool stop_requested;
    bool arm_requested;
    bool startup_ready_seen;
    bool imu_generation_seen;
    uint64_t imu_generation;
    bool start_finished;
    bool ready;
    pthread_cond_t start_condition;
    collision_pending_t pending;
} collision_worker_t;


static uint64_t monotonic_time_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }

    return
        (uint64_t)now.tv_sec * 1000000000ULL
        + (uint64_t)now.tv_nsec;
}


static void sleep_microseconds(unsigned int microseconds)
{
    struct timespec duration;

    duration.tv_sec = (time_t)(microseconds / 1000000U);
    duration.tv_nsec = (long)(microseconds % 1000000U) * 1000L;

    while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
    {
    }
}


static int64_t timestamp_its_ms(void)
{
    struct timespec now;
    uint64_t unix_ms;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    {
        return 0;
    }

    unix_ms = (uint64_t)now.tv_sec * 1000ULL
            + (uint64_t)now.tv_nsec / 1000000ULL;
    if (unix_ms < SENSOR_ITS_EPOCH_UNIX_MS)
    {
        return 0;
    }
    return (int64_t)(
        unix_ms - SENSOR_ITS_EPOCH_UNIX_MS + SENSOR_ITS_LEAP_SECONDS_MS
    );
}

/* ========================================================================== */
/* 충돌 지표 계산과 rising-edge 판정                                           */
/* ========================================================================== */

/*
 * raw acceleration은 중력을 포함하고 linear acceleration은 SH-2가 중력을
 * 제거한 값이다. jerk는 같은 stream의 연속 두 sample 차 / 실제 설정 interval,
 * delta-V는 linear acceleration의 사다리꼴 적분 벡터 크기다. 서로 다른 물리량을
 * 임계값 하나로 섞지 않고 metric별 evidence로 유지한다.
 *
 * - acceleration: 지금 순간에 얼마나 큰 힘이 관측됐는지 본다.
 * - jerk: 가속도가 얼마나 갑자기 변했는지 본다.
 * - delta-V: 짧은 시간 동안 속도가 얼마나 변했는지 본다.
 *
 * RC카의 평상시 진동 하나만으로 사고가 되지 않도록, 선택한 지표 여러 개가
 * 가까운 시간 안에 함께 임계값을 넘는지를 확인한다.
 */

typedef struct
{
    double raw_accel_mps2;
    double linear_accel_mps2;
    double raw_jerk_mps3;
    double linear_jerk_mps3;
    double linear_delta_v_mps;
    bool raw_jerk_valid;
    bool linear_jerk_valid;
    bool linear_delta_v_valid;
} collision_observation_t;

static collision_vector3_t collision_vector_add(
    collision_vector3_t left,
    collision_vector3_t right
)
{
    collision_vector3_t result = {
        left.x + right.x,
        left.y + right.y,
        left.z + right.z
    };
    return result;
}

static collision_vector3_t collision_vector_subtract(
    collision_vector3_t left,
    collision_vector3_t right
)
{
    collision_vector3_t result = {
        left.x - right.x,
        left.y - right.y,
        left.z - right.z
    };
    return result;
}

static collision_vector3_t collision_vector_scale(
    collision_vector3_t value,
    double scale
)
{
    collision_vector3_t result = {
        value.x * scale,
        value.y * scale,
        value.z * scale
    };
    return result;
}

static double collision_vector_magnitude(collision_vector3_t value)
{
    return sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

static void collision_reset_evidence(collision_detector_state_t *state)
{
    memset(state->evidence_ns, 0, sizeof(state->evidence_ns));
    memset(state->evidence_values, 0, sizeof(state->evidence_values));
    state->last_evidence_ns = 0U;
}

static void collision_reset_delta_v(collision_detector_state_t *state)
{
    state->delta_v.head = 0U;
    state->delta_v.count = 0U;
    state->delta_v.sum = (collision_vector3_t){0.0, 0.0, 0.0};
}

static unsigned int collision_delta_v_duration_ms(
    collision_dv_window_t window
)
{
    switch (window)
    {
    case COLLISION_DV_WINDOW_20_MS:
        return 20U;
    case COLLISION_DV_WINDOW_50_MS:
        return 50U;
    case COLLISION_DV_WINDOW_200_MS:
        return 200U;
    case COLLISION_DV_WINDOW_100_MS:
    default:
        return 100U;
    }
}

static bool collision_config_is_valid(const collision_config_t *config)
{
    uint32_t mask;
    unsigned int selected = 0U;
    size_t index;

    if (config == NULL || !config->enabled
        || config->coincidence_window_ms == 0U
        || config->cooldown_ms == 0U
        || config->quiet_rearm_ms == 0U)
    {
        return false;
    }
    if (config->delta_v_window != COLLISION_DV_WINDOW_20_MS
        && config->delta_v_window != COLLISION_DV_WINDOW_50_MS
        && config->delta_v_window != COLLISION_DV_WINDOW_100_MS
        && config->delta_v_window != COLLISION_DV_WINDOW_200_MS)
    {
        return false;
    }
    mask = config->metric_mask & COLLISION_METRIC_ALL;
    for (index = 0U; index < 5U; index++)
    {
        if ((mask & (UINT32_C(1) << index)) != 0U)
        {
            selected++;
        }
    }
    if (mask == 0U || mask != config->metric_mask
        || config->minimum_evidence_count == 0U
        || config->minimum_evidence_count > selected)
    {
        return false;
    }
    if (((mask & COLLISION_METRIC_RAW_ACCEL) != 0U
         && (!isfinite(config->raw_accel_threshold_mps2)
             || config->raw_accel_threshold_mps2 <= 0.0))
        || ((mask & COLLISION_METRIC_LINEAR_ACCEL) != 0U
            && (!isfinite(config->linear_accel_threshold_mps2)
                || config->linear_accel_threshold_mps2 <= 0.0))
        || ((mask & COLLISION_METRIC_RAW_JERK) != 0U
            && (!isfinite(config->raw_jerk_threshold_mps3)
                || config->raw_jerk_threshold_mps3 <= 0.0))
        || ((mask & COLLISION_METRIC_LINEAR_JERK) != 0U
            && (!isfinite(config->linear_jerk_threshold_mps3)
                || config->linear_jerk_threshold_mps3 <= 0.0))
        || ((mask & COLLISION_METRIC_LINEAR_DELTA_V) != 0U
            && (!isfinite(config->linear_delta_v_threshold_mps)
                || config->linear_delta_v_threshold_mps <= 0.0)))
    {
        return false;
    }
    return true;
}

static void collision_detector_initialize(
    collision_detector_state_t *state,
    const collision_config_t *config,
    uint32_t raw_interval_us,
    uint32_t linear_interval_us
)
{
    double samples;

    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->raw_interval_s = (double)raw_interval_us / 1000000.0;
    state->linear_interval_s = (double)linear_interval_us / 1000000.0;
    if (state->raw_interval_s <= 0.0)
    {
        state->raw_interval_s = 0.01;
    }
    if (state->linear_interval_s <= 0.0)
    {
        state->linear_interval_s = 0.01;
    }

    samples = ceil(
        (double)collision_delta_v_duration_ms(config->delta_v_window)
        / 1000.0 / state->linear_interval_s
    );
    if (samples < 1.0)
    {
        samples = 1.0;
    }
    if (samples > (double)SENSOR_DV_QUEUE_CAPACITY)
    {
        samples = (double)SENSOR_DV_QUEUE_CAPACITY;
    }
    state->delta_v.limit = (size_t)samples;
}

static void collision_detector_set_armed(
    collision_detector_state_t *state,
    bool armed
)
{
    /* arm/disarm은 이전 sample, evidence, latch를 모두 끊는 측정 구간 경계다. */
    state->armed = armed && collision_config_is_valid(&state->config);
    state->latched = false;
    state->latched_at_ns = 0U;
    state->raw.valid = false;
    state->linear.valid = false;
    state->last_event_ns = 0U;
    state->generation_valid = false;
    state->generation = 0U;
    collision_reset_delta_v(state);
    collision_reset_evidence(state);
}

static bool collision_stream_is_contiguous(
    collision_stream_state_t *stream,
    const bno086_event_t *event
)
{
    uint8_t expected;

    /* 직전 번호 다음 report가 아니면 두 값을 빼서 jerk를 만들 근거가 없다. */
    if (!stream->valid || stream->generation != event->bno_generation)
    {
        return false;
    }
    expected = (uint8_t)((unsigned int)stream->sequence + 1U);
    return event->report_sequence == expected
        && event->monotonic_ns >= stream->monotonic_ns;
}

static void collision_stream_update(
    collision_stream_state_t *stream,
    const bno086_event_t *event,
    collision_vector3_t value
)
{
    stream->valid = true;
    stream->sequence = event->report_sequence;
    stream->generation = event->bno_generation;
    stream->monotonic_ns = event->monotonic_ns;
    stream->value = value;
}

static double collision_delta_v_push(
    collision_detector_state_t *state,
    collision_vector3_t contribution,
    bool *valid
)
{
    size_t tail;

    /* 가장 오래된 기여 벡터를 빼고 새 기여를 더해 O(1) sliding sum을 유지한다. */
    if (state->delta_v.count == state->delta_v.limit)
    {
        collision_vector3_t removed =
            state->delta_v.values[state->delta_v.head];
        state->delta_v.sum = collision_vector_subtract(
            state->delta_v.sum,
            removed
        );
        state->delta_v.head =
            (state->delta_v.head + 1U) % SENSOR_DV_QUEUE_CAPACITY;
        state->delta_v.count--;
    }

    tail = (state->delta_v.head + state->delta_v.count)
         % SENSOR_DV_QUEUE_CAPACITY;
    state->delta_v.values[tail] = contribution;
    state->delta_v.sum = collision_vector_add(
        state->delta_v.sum,
        contribution
    );
    state->delta_v.count++;
    *valid = state->delta_v.count >= state->delta_v.limit;
    return *valid ? collision_vector_magnitude(state->delta_v.sum) : 0.0;
}

static void collision_record_evidence(
    collision_detector_state_t *state,
    uint32_t metric,
    size_t evidence_index,
    double value,
    double threshold,
    bool value_valid,
    uint64_t now_ns,
    bool *any_evidence
)
{
    if ((state->config.metric_mask & metric) != 0U
        && value_valid && value >= threshold)
    {
        state->evidence_ns[evidence_index] = now_ns;
        state->evidence_values[evidence_index] = value;
        state->last_evidence_ns = now_ns;
        *any_evidence = true;
    }
}

/*
 * 한 sensor report를 처리하는 순수 판정 경로다. false이면 이벤트 없음,
 * true이면 latch의 rising edge 한 번이다. sequence/generation 단절은 jerk,
 * delta-V와 이전 evidence를 모두 끊어 서로 다른 구간을 결합하지 않는다.
 * 선택된 지표 중 minimum_evidence_count개가 coincidence window 안에 있을 때만
 * 충돌 후보가 된다. 한 충돌 파형의 여러 peak가 중복 전송되지 않도록 latch하고,
 * cooldown과 quiet 시간이 모두 지난 뒤에만 다음 사고를 받을 수 있다.
 * 예를 들어 minimum=2이면 acceleration peak 하나만으로는 부족하고, 같은
 * coincidence 구간에 jerk 또는 delta-V 같은 두 번째 근거가 있어야 true다.
 */
static bool collision_detector_process(
    collision_detector_state_t *state,
    const bno086_event_t *event,
    collision_observation_t *observation
)
{
    collision_stream_state_t *stream;
    collision_vector3_t value;
    collision_vector3_t difference;
    bool contiguous;
    bool any_evidence = false;
    bool boundary_discontinuity = false;
    uint64_t coincidence_ns;
    unsigned int evidence_count = 0U;
    size_t index;

    memset(observation, 0, sizeof(*observation));
    if (!state->armed || !state->config.enabled
        || (event->type != BNO086_EVENT_ACCELEROMETER
            && event->type != BNO086_EVENT_LINEAR_ACCELERATION))
    {
        return false;
    }

    value = (collision_vector3_t){event->x, event->y, event->z};
    stream = event->type == BNO086_EVENT_ACCELEROMETER
           ? &state->raw : &state->linear;

    /* reset 전후 report를 연결하면 정상 startup 차이가 거대한 jerk처럼 보일 수 있다. */
    if (state->generation_valid
        && event->bno_generation != state->generation)
    {
        state->raw.valid = false;
        state->linear.valid = false;
        collision_reset_delta_v(state);
        collision_reset_evidence(state);
        state->last_evidence_ns = event->monotonic_ns;
        boundary_discontinuity = true;
    }
    state->generation_valid = true;
    state->generation = event->bno_generation;

    if (state->last_event_ns != 0U
        && event->monotonic_ns < state->last_event_ns)
    {
        state->raw.valid = false;
        state->linear.valid = false;
        collision_reset_delta_v(state);
        collision_reset_evidence(state);
        state->last_evidence_ns = event->monotonic_ns;
        boundary_discontinuity = true;
    }
    state->last_event_ns = event->monotonic_ns;
    if (boundary_discontinuity)
    {
        collision_stream_update(stream, event, value);
        return false;
    }

    contiguous = collision_stream_is_contiguous(stream, event);
    if (stream->valid && !contiguous)
    {
        collision_reset_evidence(state);
        state->last_evidence_ns = event->monotonic_ns;
        collision_reset_delta_v(state);
        collision_stream_update(stream, event, value);
        return false;
    }

    if (event->type == BNO086_EVENT_ACCELEROMETER)
    {
        observation->raw_accel_mps2 = collision_vector_magnitude(value);
        if (contiguous)
        {
            difference = collision_vector_subtract(value, stream->value);
            observation->raw_jerk_mps3 =
                collision_vector_magnitude(difference) / state->raw_interval_s;
            observation->raw_jerk_valid = true;
        }
    }
    else
    {
        observation->linear_accel_mps2 = collision_vector_magnitude(value);
        if (contiguous)
        {
            collision_vector3_t average = collision_vector_scale(
                collision_vector_add(value, stream->value),
                0.5
            );
            collision_vector3_t contribution = collision_vector_scale(
                average,
                state->linear_interval_s
            );

            difference = collision_vector_subtract(value, stream->value);
            observation->linear_jerk_mps3 =
                collision_vector_magnitude(difference)
                / state->linear_interval_s;
            observation->linear_jerk_valid = true;
            observation->linear_delta_v_mps = collision_delta_v_push(
                state,
                contribution,
                &observation->linear_delta_v_valid
            );
        }
    }
    collision_stream_update(stream, event, value);

    collision_record_evidence(
        state, COLLISION_METRIC_RAW_ACCEL, 0U,
        observation->raw_accel_mps2,
        state->config.raw_accel_threshold_mps2,
        event->type == BNO086_EVENT_ACCELEROMETER,
        event->monotonic_ns, &any_evidence
    );
    collision_record_evidence(
        state, COLLISION_METRIC_LINEAR_ACCEL, 1U,
        observation->linear_accel_mps2,
        state->config.linear_accel_threshold_mps2,
        event->type == BNO086_EVENT_LINEAR_ACCELERATION,
        event->monotonic_ns, &any_evidence
    );
    collision_record_evidence(
        state, COLLISION_METRIC_RAW_JERK, 2U,
        observation->raw_jerk_mps3,
        state->config.raw_jerk_threshold_mps3,
        observation->raw_jerk_valid,
        event->monotonic_ns, &any_evidence
    );
    collision_record_evidence(
        state, COLLISION_METRIC_LINEAR_JERK, 3U,
        observation->linear_jerk_mps3,
        state->config.linear_jerk_threshold_mps3,
        observation->linear_jerk_valid,
        event->monotonic_ns, &any_evidence
    );
    collision_record_evidence(
        state, COLLISION_METRIC_LINEAR_DELTA_V, 4U,
        observation->linear_delta_v_mps,
        state->config.linear_delta_v_threshold_mps,
        observation->linear_delta_v_valid,
        event->monotonic_ns, &any_evidence
    );

    if (state->latched)
    {
        uint64_t cooldown_ns =
            (uint64_t)state->config.cooldown_ms * 1000000ULL;
        uint64_t quiet_ns =
            (uint64_t)state->config.quiet_rearm_ms * 1000000ULL;
        bool cooldown_done = event->monotonic_ns >= state->latched_at_ns
            && event->monotonic_ns - state->latched_at_ns >= cooldown_ns;
        bool quiet_done = !any_evidence && state->last_evidence_ns != 0U
            && event->monotonic_ns >= state->last_evidence_ns
            && event->monotonic_ns - state->last_evidence_ns >= quiet_ns;

        /* 시간만 지났어도 충격 진동이 계속되면 rearm하지 않는다. */
        if (cooldown_done && quiet_done)
        {
            state->latched = false;
            collision_reset_evidence(state);
        }
        return false;
    }

    coincidence_ns =
        (uint64_t)state->config.coincidence_window_ms * 1000000ULL;
    for (index = 0U; index < 5U; index++)
    {
        uint32_t metric = UINT32_C(1) << index;
        if ((state->config.metric_mask & metric) != 0U
            && state->evidence_ns[index] != 0U
            && event->monotonic_ns >= state->evidence_ns[index]
            && event->monotonic_ns - state->evidence_ns[index]
               <= coincidence_ns)
        {
            evidence_count++;
        }
    }

    if (evidence_count >= state->config.minimum_evidence_count)
    {
        state->latched = true;
        state->latched_at_ns = event->monotonic_ns;
        return true;
    }
    return false;
}

/* ========================================================================== */
/* 위치 publication과 P2P IPC payload                                          */
/* ========================================================================== */

/*
 * ArUco의 testbed mm 좌표는 기존 충돌 계산용 publication에서 cm로 보관했다가
 * P2P payload의 mm로 바꾼다. 좌표/heading을 mutex 아래 함께 공개해 충돌
 * Thread가 판정 순간의 일관된 사본을 얻는다. 유효하지 않은 GNSS 필드는 ETSI
 * unavailable sentinel을 쓰며, 알려진 local_x/local_y만 실제 위치로 채운다.
 * 즉 latitude/longitude sentinel은 좌표가 0이라는 뜻이 아니라 "이 차량은
 * GNSS 좌표를 제공하지 않는다"는 protocol 표현이다.
 */

static int position_publication_initialize(position_publication_t *publication)
{
    memset(publication, 0, sizeof(*publication));
    if (pthread_mutex_init(&publication->mutex, NULL) != 0)
    {
        return -1;
    }
    publication->initialized = true;
    return 0;
}

static void position_publication_deinitialize(
    position_publication_t *publication
)
{
    if (publication->initialized)
    {
        pthread_mutex_destroy(&publication->mutex);
        publication->initialized = false;
    }
}


static void position_publication_invalidate(position_publication_t *publication)
{
    pthread_mutex_lock(&publication->mutex);
    if (publication->valid)
    {
        publication->generation++;
    }
    publication->valid = false;
    publication->heading_valid = false;
    pthread_mutex_unlock(&publication->mutex);
}

static void position_publication_snapshot(
    position_publication_t *publication,
    position_publication_t *snapshot
)
{
    pthread_mutex_lock(&publication->mutex);
    snapshot->valid = publication->valid;
    snapshot->heading_valid = publication->heading_valid;
    snapshot->x_cm = publication->x_cm;
    snapshot->y_cm = publication->y_cm;
    snapshot->heading_deg = publication->heading_deg;
    snapshot->capture_ns = publication->capture_ns;
    snapshot->expires_ns = publication->expires_ns;
    snapshot->generation = publication->generation;
    snapshot->history_head = publication->history_head;
    snapshot->history_count = publication->history_count;
    memcpy(snapshot->history, publication->history, sizeof(snapshot->history));
    {
        uint64_t now_ns = monotonic_time_ns();
        if (now_ns == 0U || now_ns < snapshot->capture_ns
            || now_ns >= snapshot->expires_ns)
        {
            snapshot->valid = false;
            snapshot->heading_valid = false;
        }
    }
    pthread_mutex_unlock(&publication->mutex);
}

static bool collision_position_to_mm(double centimeters, int32_t *millimeters)
{
    double value = centimeters * 10.0;

    /* ETSI LocalCoordinates 계약 범위: -1,000,000 .. +1,000,000 mm. */
    if (!isfinite(value) || value < -1000000.0 || value > 1000000.0)
    {
        return false;
    }
    *millimeters = (int32_t)llround(value);
    return true;
}

static uint16_t collision_heading_value(
    bool heading_valid,
    double heading_deg
)
{
    double normalized;
    long value;

    /* ETSI heading은 0.1도 단위이고 3601은 사용할 수 없음을 뜻한다. */
    if (!heading_valid || !isfinite(heading_deg))
    {
        return 3601U;
    }
    normalized = fmod(heading_deg, 360.0);
    if (normalized < 0.0)
    {
        normalized += 360.0;
    }
    value = lround(normalized * 10.0);
    if (value >= 3600L)
    {
        value = 0L;
    }
    return (uint16_t)value;
}

static bool collision_fill_payload(
    const collision_config_t *config,
    const position_publication_t *position,
    int64_t detection_time,
    accident_ipc_event_t *payload
)
{
    bool position_in_range;
    int32_t x_mm = 0;
    int32_t y_mm = 0;

    if (config == NULL || position == NULL || payload == NULL
        || detection_time <= 0)
    {
        return false;
    }

    position_in_range = position->valid
        && collision_position_to_mm(position->x_cm, &x_mm)
        && collision_position_to_mm(position->y_cm, &y_mm);
    if (config->require_position && !position_in_range)
    {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    /* pending에 확정된 사고별 sequence는 호출자가 보존한다. */
    payload->station_id = SENSOR_STATION_ID;
    payload->detection_time = detection_time;
    payload->heading_value = collision_heading_value(
        position->heading_valid,
        position->heading_deg
    );
    payload->local_x_mm = position_in_range ? x_mm : 0;
    payload->local_y_mm = position_in_range ? y_mm : 0;
    return true;
}

static void collision_ipc_wait_retry(collision_ipc_worker_t *worker)
{
    struct timespec deadline;

    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 500000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&worker->mutex);
    if (!worker->stop_requested)
    {
        (void)pthread_cond_clockwait(
            &worker->condition,
            &worker->mutex, CLOCK_MONOTONIC,
            &deadline
        );
    }
    pthread_mutex_unlock(&worker->mutex);
}

/* A full listen backlog is a retry, not a blocking operation. */
static int collision_ipc_connect(const char *path)
{
    struct sockaddr_un address;
    int fd;
    if (strlen(path) >= sizeof(address.sun_path)) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1U);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        fcntl(fd, F_SETFL, 0) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool collision_ipc_send_to_endpoint(
    int *fd,
    const char *socket_path,
    uint16_t message_type,
    const void *payload,
    size_t payload_size,
    uint32_t sequence,
    uint64_t monotonic_ns
)
{
    ipc_header_t header;

    if (*fd < 0)
    {
        struct timeval send_timeout = {0, 200000};

        *fd = collision_ipc_connect(socket_path);
        if (*fd < 0)
        {
            return false;
        }
        if (setsockopt(*fd, SOL_SOCKET, SO_SNDTIMEO,
                       &send_timeout, sizeof(send_timeout)) != 0)
        {
            ipc_close(*fd);
            *fd = -1;
            return false;
        }
    }

    memset(&header, 0, sizeof(header));
    header.type = message_type;
    header.flags = IPC_FLAG_NONE;
    header.sequence = sequence;
    header.timestamp = monotonic_ns;
    if (ipc_send(*fd, &header, payload, payload_size) != IPC_SUCCESS)
    {
        ipc_close(*fd);
        *fd = -1;
        return false;
    }
    return true;
}

/* 각 대상의 queue head만 처리한다. 한 대상의 재시도는 다른 대상을 막지 않는다. */
static bool collision_ipc_send_head(
    collision_ipc_worker_t *worker,
    bool video,
    int *fd
)
{
    collision_ipc_queue_t *queue = video ? &worker->video : &worker->p2p;
    collision_ipc_item_t item;
    bool sent;

    pthread_mutex_lock(&worker->mutex);
    if (queue->count == 0U)
    {
        pthread_mutex_unlock(&worker->mutex);
        return true;
    }
    item = queue->items[queue->head];
    pthread_mutex_unlock(&worker->mutex);

    if (video)
    {
        video_processor_event_t event = {0};
        event.station_id = item.payload.station_id;
        event.sequence_number = item.payload.sequence_number;
        event.detection_time = item.payload.detection_time;
        sent = collision_ipc_send_to_endpoint(
            fd, VIDEO_PROCESSOR_SOCKET_PATH, IPC_MSG_VIDEO_PROCESSOR_EVENT,
            &event, sizeof(event), event.sequence_number, item.monotonic_ns
        );
        /* Video Processor는 한 연결에서 사고 한 건을 받은 뒤 닫는다. */
        if (*fd >= 0)
        {
            ipc_close(*fd);
            *fd = -1;
        }
    }
    else
    {
        sent = collision_ipc_send_to_endpoint(
            fd, SENSOR_P2P_SOCKET_PATH, IPC_MSG_ACCIDENT_EVENT,
            &item.payload, sizeof(item.payload), item.payload.sequence_number,
            item.monotonic_ns
        );
    }
    if (sent)
    {
        pthread_mutex_lock(&worker->mutex);
        queue->head = (queue->head + 1U) % SENSOR_IPC_QUEUE_CAPACITY;
        queue->count--;
        worker->sent++;
        pthread_mutex_unlock(&worker->mutex);
    }
    return sent;
}

static void *collision_ipc_thread_main(void *argument)
{
    collision_ipc_worker_t *worker = argument;
    int p2p_fd = -1;

    for (;;)
    {
        int video_fd = -1;
        bool p2p_sent;
        bool video_sent;

        aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_IPC, 10000U);

        pthread_mutex_lock(&worker->mutex);
        while (worker->p2p.count == 0U && worker->video.count == 0U
               && !worker->stop_requested)
        {
            struct timespec deadline;
            (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec++;
            aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_IPC, 10000U);
            (void)pthread_cond_clockwait(&worker->condition, &worker->mutex,
                                          CLOCK_MONOTONIC, &deadline);
        }
        if (worker->stop_requested)
        {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        pthread_mutex_unlock(&worker->mutex);

        video_sent = collision_ipc_send_head(worker, true, &video_fd);
        p2p_sent = collision_ipc_send_head(worker, false, &p2p_fd);
        if (!p2p_sent || !video_sent)
        {
            collision_ipc_wait_retry(worker);
        }
    }

    if (p2p_fd >= 0)
    {
        ipc_close(p2p_fd);
    }
    return NULL;
}

static int collision_ipc_worker_start(collision_ipc_worker_t *worker)
{
    int result;

    memset(worker, 0, sizeof(*worker));
    if (pthread_mutex_init(&worker->mutex, NULL) != 0)
    {
        return -1;
    }
    if (pthread_cond_init(&worker->condition, NULL) != 0)
    {
        pthread_mutex_destroy(&worker->mutex);
        return -1;
    }
    worker->initialized = true;
    worker->next_sequence = 1U;
    result = pthread_create(
        &worker->thread,
        NULL,
        collision_ipc_thread_main,
        worker
    );
    if (result != 0)
    {
        pthread_cond_destroy(&worker->condition);
        pthread_mutex_destroy(&worker->mutex);
        worker->initialized = false;
        return -1;
    }
    worker->thread_created = true;
    return 0;
}

static bool collision_ipc_enqueue(
    collision_ipc_worker_t *worker,
    const collision_ipc_item_t *item,
    bool video
)
{
    collision_ipc_queue_t *queue = video ? &worker->video : &worker->p2p;
    size_t tail;
    bool queued = false;

    pthread_mutex_lock(&worker->mutex);
    /* 센서 Thread는 기다리지 않는다. 실패한 항목은 호출자의 pending에 남긴다. */
    if (!worker->stop_requested && queue->count < SENSOR_IPC_QUEUE_CAPACITY)
    {
        tail = (queue->head + queue->count) % SENSOR_IPC_QUEUE_CAPACITY;
        queue->items[tail] = *item;
        queue->count++;
        queued = true;
        pthread_cond_signal(&worker->condition);
    }
    else
    {
        worker->enqueue_blocked++;
    }
    pthread_mutex_unlock(&worker->mutex);
    return queued;
}

static bool collision_pending_record(
    collision_pending_t *pending,
    uint64_t event_ns,
    int64_t detection_time,
    collision_ipc_worker_t *ipc
)
{
    if (pending->active || event_ns == 0U || detection_time <= 0)
    {
        return false;
    }
    memset(pending, 0, sizeof(*pending));
    pending->active = true;
    pending->item.monotonic_ns = event_ns;
    pending->item.payload.station_id = SENSOR_STATION_ID;
    pending->item.payload.detection_time = detection_time;
    /* 사고를 등록할 때 한 번 배정한다. 대상별 대기/재시도 중에는 바뀌지 않는다. */
    pthread_mutex_lock(&ipc->mutex);
    pending->item.payload.sequence_number = (uint16_t)ipc->next_sequence;
    ipc->next_sequence = ipc->next_sequence >= UINT16_MAX ? 1U : ipc->next_sequence + 1U;
    pthread_mutex_unlock(&ipc->mutex);
    pending->video_queued = collision_ipc_enqueue(ipc, &pending->item, true);
    return true;
}

static uint64_t collision_pose_distance(uint64_t capture_ns, uint64_t event_ns)
{
    return capture_ns >= event_ns ? capture_ns - event_ns : event_ns - capture_ns;
}

static void collision_pending_consider_history(
    collision_pending_t *pending,
    const collision_config_t *config,
    const position_publication_t *position,
    uint64_t now_ns
)
{
    const uint64_t event_ns = pending->item.monotonic_ns;
    const uint64_t camera_delay_ns = (uint64_t)config->camera_read_delay_ms * UINT64_C(1000000);
    size_t index;

    if (position == NULL)
        return;
    for (index = 0U; index < position->history_count; index++)
    {
        const position_sample_t *sample = &position->history[
            (position->history_head + index) % SENSOR_POSE_HISTORY_CAPACITY
        ];
        uint64_t measurement_ns;

        if (sample->capture_ns <= camera_delay_ns)
            continue;
        measurement_ns = sample->capture_ns - camera_delay_ns;
        /* 사고 인식 후 측정한 위치 중 수신 기한 안에 확보한 최초 위치를 쓴다. */
        if (sample->capture_ns == 0U || sample->received_ns < sample->capture_ns
            || sample->received_ns > now_ns
            || sample->received_ns >= sample->expires_ns
            || (sample->received_ns > event_ns
                && sample->received_ns - event_ns > SENSOR_POSE_MATCH_WAIT_NS)
            || measurement_ns < event_ns
            || (measurement_ns > event_ns
                && measurement_ns - event_ns > SENSOR_POSE_MATCH_WAIT_NS)
            || !isfinite(sample->x_cm) || !isfinite(sample->y_cm)
            || fabs(sample->x_cm) > 100000.0 || fabs(sample->y_cm) > 100000.0
            || !isfinite(sample->heading_deg)
            || sample->heading_deg < 0.0 || sample->heading_deg >= 360.0)
            continue;
        if (!pending->candidate_valid
            || measurement_ns < pending->candidate_measurement_ns)
        {
            pending->candidate = *sample;
            pending->candidate_measurement_ns = measurement_ns;
            pending->candidate_valid = true;
        }
    }
}

static bool collision_pending_try_enqueue(
    collision_pending_t *pending,
    const collision_config_t *config,
    const position_publication_t *position,
    uint64_t now_ns,
    collision_ipc_worker_t *ipc
)
{
    bool newly_confirmed = false;
    bool p2p_enqueued = false;
    uint64_t event_ns;

    if (!pending->active || now_ns < pending->item.monotonic_ns)
        return false;
    event_ns = pending->item.monotonic_ns;
    if (!pending->video_queued)
        pending->video_queued = collision_ipc_enqueue(ipc, &pending->item, true);
    if (!pending->pose_resolved)
    {
        position_publication_t selected = {0};
        accident_ipc_event_t payload;

        /* 만료/검출 실패 이후에도 수신해 둔 좌표와 pending 후보는 보존한다. */
        collision_pending_consider_history(pending, config, position, now_ns);
        if (!pending->candidate_valid && now_ns - event_ns < SENSOR_POSE_MATCH_WAIT_NS)
            return false;
        if (!pending->candidate_valid)
        {
            fprintf(stderr, "\n[!] 사고 위치 미확보. P2P 전송만 생략하고 자신의 영상 요청은 유지합니다. "
                    "event_ns=%llu detection_time=%lld\n",
                    (unsigned long long)event_ns,
                    (long long)pending->item.payload.detection_time);
            pending->pose_resolved = true;
        }
        else
        {
            selected.valid = true;
            selected.heading_valid = true;
            selected.x_cm = pending->candidate.x_cm;
            selected.y_cm = pending->candidate.y_cm;
            selected.heading_deg = pending->candidate.heading_deg;
            if (!collision_fill_payload(
                    config, &selected, pending->item.payload.detection_time, &payload
                ))
                return false;
            payload.sequence_number = pending->item.payload.sequence_number;
            pending->item.payload = payload;
            pending->pose_resolved = true;
            pending->pose_confirmed = true;
            newly_confirmed = true;
        }
    }

    /* IPC가 밀려도 선택한 좌표와 원래 사고 시각을 유지한다. */
    if (pending->pose_confirmed && !pending->p2p_queued)
    {
        if (!collision_ipc_enqueue(ipc, &pending->item, false))
        {
            if (newly_confirmed)
                fprintf(stderr, "\n[!] 사고 후 위치 확보 완료. P2P queue가 빌 때까지 같은 메시지를 보관합니다.\n");
            return false;
        }
        pending->p2p_queued = true;
        p2p_enqueued = true;
        fprintf(stderr, "\n[+] 사고 후 위치를 P2P IPC queue에 넣었습니다. "
                "wait=%.1fms pose_delta=%+.1fms measurement_ns=%llu read_ns=%llu X=%d Y=%dmm\n",
                (double)(now_ns - event_ns) / 1000000.0,
                ((pending->candidate_measurement_ns >= event_ns) ? 1.0 : -1.0)
                    * (double)collision_pose_distance(pending->candidate_measurement_ns, event_ns) / 1000000.0,
                (unsigned long long)pending->candidate_measurement_ns,
                (unsigned long long)pending->candidate.capture_ns,
                (int)pending->item.payload.local_x_mm, (int)pending->item.payload.local_y_mm);
    }
    if (pending->video_queued && pending->pose_resolved
        && (!pending->pose_confirmed || pending->p2p_queued))
        memset(pending, 0, sizeof(*pending));
    return p2p_enqueued;
}

static void collision_ipc_worker_stop(collision_ipc_worker_t *worker)
{
    if (!worker->initialized)
    {
        return;
    }
    pthread_mutex_lock(&worker->mutex);
    worker->stop_requested = true;
    pthread_cond_broadcast(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);
    if (worker->thread_created)
    {
        pthread_join(worker->thread, NULL);
    }
    pthread_cond_destroy(&worker->condition);
    pthread_mutex_destroy(&worker->mutex);
    worker->initialized = false;
    worker->thread_created = false;
}

/* ========================================================================== */
/* BNO event ring 소비 사고 판정 Thread                                        */
/* ========================================================================== */

/*
 * cursor는 Thread 시작 이후 event부터 읽어 startup 전 sample을
 * 사고 지표에 섞지 않는다. startup_ready loss나 ring overwrite는 sample
 * 연속성이 깨진 것이므로 detector를 재초기화한다. 이미 판정한 pending 사고는
 * 유지한다. 매 반복에서 위치를 확인하고 IPC 송신 자체는 전용 Thread에 넘긴다.
 * cursor.dropped가 늘었다면 중간 sample이 사라진 것이므로 직전 sample과의
 * jerk/delta-V 계산을 이어가지 않고 detector 상태를 깨끗하게 다시 시작한다.
 */

static bool collision_worker_should_stop(collision_worker_t *worker)
{
    bool stop;

    pthread_mutex_lock(&worker->mutex);
    stop = worker->stop_requested;
    pthread_mutex_unlock(&worker->mutex);
    return stop;
}

static void collision_worker_set_armed(
    collision_worker_t *worker,
    bool armed
)
{
    if (worker == NULL || !worker->initialized)
    {
        return;
    }
    /* 위치와 무관하게 판정을 요청한다. 실제 arm은 BNO startup과 설정을 따른다. */
    pthread_mutex_lock(&worker->mutex);
    worker->arm_requested = armed;
    collision_detector_set_armed(
        &worker->detector,
        armed && worker->startup_ready_seen
    );
    pthread_mutex_unlock(&worker->mutex);
}

static bool collision_worker_apply_startup_locked(
    collision_worker_t *worker,
    bool startup_ready
)
{
    /* SH-2 reset 동안은 무조건 disarm하고 새 startup 경계에서만 요청 상태로 복구한다. */
    if (!startup_ready)
    {
        collision_detector_set_armed(&worker->detector, false);
        worker->startup_ready_seen = false;
        return false;
    }
    if (!worker->startup_ready_seen)
    {
        worker->startup_ready_seen = true;
        collision_detector_set_armed(
            &worker->detector,
            worker->arm_requested
        );
    }
    return worker->detector.armed;
}

/* Worker mutex 아래 호출하며, 완전히 설정된 BNO 구간만 판정에 사용한다. */
static bool collision_worker_apply_imu_ready_locked(
    collision_worker_t *worker,
    const bno086_snapshot_t *snapshot,
    uint64_t *ready_since_ns
)
{
    imu_worker_t *imu = worker->imu_worker;
    bool ready;
    uint64_t generation;
    uint32_t raw_interval_us;
    uint32_t linear_interval_us;

    pthread_mutex_lock(&imu->mutex);
    ready = imu->ready;
    generation = imu->ready_generation;
    *ready_since_ns = imu->ready_since_ns;
    raw_interval_us = imu->raw_accel_interval_us;
    linear_interval_us = imu->linear_accel_interval_us;
    pthread_mutex_unlock(&imu->mutex);

    ready = ready && snapshot->startup_ready
        && snapshot->bno_generation == generation
        && raw_interval_us != 0U && linear_interval_us != 0U;
    if (ready && (!worker->imu_generation_seen
                  || worker->imu_generation != generation))
    {
        collision_config_t config = worker->detector.config;
        collision_detector_initialize(
            &worker->detector, &config, raw_interval_us, linear_interval_us
        );
        worker->imu_generation_seen = true;
        worker->imu_generation = generation;
        worker->startup_ready_seen = false;
    }
    return collision_worker_apply_startup_locked(worker, ready);
}

static void collision_worker_service_pending(collision_worker_t *worker)
{
    position_publication_t position = {0};

    pthread_mutex_lock(&worker->mutex);
    if (!worker->stop_requested && worker->pending.active)
    {
        uint64_t now_ns = monotonic_time_ns();
        position_publication_snapshot(worker->position, &position);
        (void)collision_pending_try_enqueue(
            &worker->pending, &worker->detector.config, &position,
            now_ns, worker->ipc_worker
        );
    }
    pthread_mutex_unlock(&worker->mutex);
}

static void *collision_worker_thread_main(void *argument)
{
    collision_worker_t *worker = argument;
    bno086_event_cursor_t cursor;
    uint64_t previous_dropped = 0U;

    memset(&cursor, 0, sizeof(cursor));
    pthread_mutex_lock(&worker->mutex);
    worker->ready = bno086_event_cursor_init(
            &worker->imu_worker->imu,
            &cursor,
            false
        ) > 0;
    worker->start_finished = true;
    pthread_cond_broadcast(&worker->start_condition);
    pthread_mutex_unlock(&worker->mutex);
    if (!worker->ready)
    {
        return NULL;
    }

    while (!collision_worker_should_stop(worker))
    {
        aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_COLLISION, 10000U);
        bno086_event_t event;
        collision_observation_t observation;
        bno086_snapshot_t snapshot;
        int next_result;
        bool detected = false;
        uint64_t ready_since_ns = 0U;

        next_result = bno086_event_next(
                &worker->imu_worker->imu,
                &cursor,
                &event,
                100
            );
        memset(&snapshot, 0, sizeof(snapshot));
        (void)bno086_get_snapshot(&worker->imu_worker->imu, &snapshot);
        pthread_mutex_lock(&worker->mutex);
        if (collision_worker_apply_imu_ready_locked(
                worker,
                &snapshot,
                &ready_since_ns
            )
            && next_result > 0
            && event.bno_generation == worker->imu_generation
            && event.monotonic_ns > ready_since_ns)
        {
            if (cursor.dropped != previous_dropped)
            {
                bool was_armed = worker->detector.armed;
                collision_detector_set_armed(&worker->detector, was_armed);
                previous_dropped = cursor.dropped;
            }
            detected = collision_detector_process(
                &worker->detector, &event, &observation
            );
        }
        if (detected)
        {
            /* REALTIME을 사고 판정 순간 한 번만 읽고 좌표 대기/IPC 재시도 중 보존한다. */
            int64_t detection_time = timestamp_its_ms();
            if (collision_pending_record(
                    &worker->pending, event.monotonic_ns, detection_time,
                    worker->ipc_worker
                ))
            {
                fprintf(
                    stderr,
                    "\n[!] 사고 판정 저장 및 자신의 영상 요청. 사고 후 위치를 확보하면 P2P로 전달합니다. "
                    "evidence rawA=%.3f linA=%.3f rawJ=%.3f linJ=%.3f dV=%.3f\n",
                    worker->detector.evidence_values[0],
                    worker->detector.evidence_values[1],
                    worker->detector.evidence_values[2],
                    worker->detector.evidence_values[3],
                    worker->detector.evidence_values[4]
                );
            }
            else if (!worker->pending.active)
            {
                fprintf(stderr, "\n[!] 사고 시각을 읽을 수 없어 사고를 등록하지 못했습니다.\n");
            }
        }
        pthread_mutex_unlock(&worker->mutex);

        /* 새 IMU sample이 없거나 BNO가 재시작 중이어도 위치 선택과 IPC 재시도를 처리한다. */
        collision_worker_service_pending(worker);
        if (next_result <= 0)
            sleep_microseconds(SENSOR_BNO_UPDATE_INTERVAL_US);
    }
    return NULL;
}

static int collision_worker_start(
    collision_worker_t *worker,
    imu_worker_t *imu_worker,
    position_publication_t *position,
    collision_ipc_worker_t *ipc_worker
)
{
    int result;

    memset(worker, 0, sizeof(*worker));
    if (pthread_mutex_init(&worker->mutex, NULL) != 0)
    {
        return -1;
    }
    if (pthread_cond_init(&worker->start_condition, NULL) != 0)
    {
        pthread_mutex_destroy(&worker->mutex);
        return -1;
    }
    worker->initialized = true;
    worker->imu_worker = imu_worker;
    worker->position = position;
    worker->ipc_worker = ipc_worker;
    worker->arm_requested = true;
    pthread_mutex_lock(&imu_worker->mutex);
    collision_detector_initialize(
        &worker->detector,
        &collision_config,
        imu_worker->raw_accel_interval_us,
        imu_worker->linear_accel_interval_us
    );
    pthread_mutex_unlock(&imu_worker->mutex);
    result = pthread_create(
        &worker->thread,
        NULL,
        collision_worker_thread_main,
        worker
    );
    if (result != 0)
    {
        pthread_cond_destroy(&worker->start_condition);
        pthread_mutex_destroy(&worker->mutex);
        worker->initialized = false;
        return -1;
    }
    worker->thread_created = true;

    pthread_mutex_lock(&worker->mutex);
    while (!worker->start_finished)
    {
        pthread_cond_wait(&worker->start_condition, &worker->mutex);
    }
    result = worker->ready ? 0 : -1;
    pthread_mutex_unlock(&worker->mutex);
    if (result != 0)
    {
        pthread_join(worker->thread, NULL);
        pthread_cond_destroy(&worker->start_condition);
        pthread_mutex_destroy(&worker->mutex);
        worker->initialized = false;
        worker->thread_created = false;
        return -1;
    }
    return 0;
}

static void collision_worker_stop(collision_worker_t *worker)
{
    if (!worker->initialized)
    {
        return;
    }
    collision_worker_set_armed(worker, false);
    pthread_mutex_lock(&worker->mutex);
    worker->stop_requested = true;
    pthread_mutex_unlock(&worker->mutex);
    if (worker->thread_created)
    {
        pthread_join(worker->thread, NULL);
    }
    pthread_cond_destroy(&worker->start_condition);
    pthread_mutex_destroy(&worker->mutex);
    worker->initialized = false;
    worker->thread_created = false;
}

/* ========================================================================== */
/* BNO086 단일 소유 I/O Thread                                                 */
/* ========================================================================== */

/*
 * BNO API 호출을 이 Thread에 집중시켜 SHTP transaction 순서를 보존한다.
 * collision Thread는 driver의 mutex-protected snapshot/event cursor만
 * 읽는다. start_condition은 가속도 feature의 FC와 실제 input report가 확인된
 * 이후에만 ready를 공개해 부분 초기화 상태로 측정을 시작하지 않게 한다.
 * 이 구조 덕분에 BLE 위치 갱신과 충돌 판정이 함께 실행되어도 BNO 장치의
 * packet을 여러 Thread가 나누어 읽는 상황이 생기지 않는다.
 */

static bool imu_worker_stop_requested(imu_worker_t *worker)
{
    bool stop;

    pthread_mutex_lock(&worker->mutex);
    stop = worker->stop_requested;
    pthread_mutex_unlock(&worker->mutex);
    return stop;
}

static void imu_worker_publish_start(
    imu_worker_t *worker,
    bool ready,
    const char *error
)
{
    pthread_mutex_lock(&worker->mutex);
    worker->ready = ready;
    worker->start_finished = true;
    if (error != NULL)
    {
        snprintf(worker->start_error, sizeof(worker->start_error), "%s", error);
    }
    pthread_cond_broadcast(&worker->start_condition);
    pthread_mutex_unlock(&worker->mutex);
}

static int setup_one_feature(
    bno086_t *imu,
    const char *name,
    uint8_t sensor_id,
    uint32_t interval_us,
    uint32_t *applied_interval_us,
    unsigned int report_timeout_ms,
    char *error,
    size_t error_size
)
{
    if (bno086_enable_feature(imu, sensor_id, interval_us, 750U) <= 0)
    {
        snprintf(error, error_size, "%s FC 설정 응답 실패", name);
        return -1;
    }

    if (bno086_wait_for_sensor_report(
            imu,
            sensor_id,
            report_timeout_ms
        ) <= 0)
    {
        snprintf(error, error_size, "%s 실제 input report 실패", name);
        return -1;
    }

    if (applied_interval_us != NULL
        && (bno086_get_feature_interval(
                imu,
                sensor_id,
                applied_interval_us
            ) <= 0
            || *applied_interval_us == 0U))
    {
        snprintf(error, error_size, "%s 실제 FC interval 없음", name);
        return -1;
    }

    return 0;
}

static int imu_configure_session(imu_worker_t *worker, char *error, size_t error_size)
{
    uint32_t raw_interval_us = 0U;
    uint32_t linear_interval_us = 0U;
    bno086_snapshot_t snapshot = {0};
    char summary[BNO086_ERROR_TEXT_SIZE] = "";

    pthread_mutex_lock(&worker->mutex);
    worker->ready = false;
    pthread_mutex_unlock(&worker->mutex);

    if (bno086_begin(&worker->imu) <= 0)
    {
        bno086_status_summary(&worker->imu, summary, sizeof(summary));
        snprintf(error, error_size, "BNO086 begin 실패: %.120s", summary);
        return -1;
    }

    /*
     * FD -> FE/FC -> 실제 Channel 3 report를 센서별로 확인한 뒤 다음 feature를
     * 설정한다. ArUco가 위치와 방향을 주므로 BNO에는 충돌 판정에 필요한
     * raw/linear acceleration만 요청한다. 각 report의 실제 interval을 확인해
     * jerk와 delta-V 계산에 사용한다.
     */
    if (
        setup_one_feature(
            &worker->imu,
            "Accelerometer",
            BNO086_SENSOR_ACCELEROMETER,
            SENSOR_ACCEL_INTERVAL_US,
            &raw_interval_us,
            1000U,
            error,
            error_size
        ) != 0
        || setup_one_feature(
            &worker->imu,
            "Linear Acceleration",
            BNO086_SENSOR_LINEAR_ACCELERATION,
            SENSOR_ACCEL_INTERVAL_US,
            &linear_interval_us,
            1000U,
            error,
            error_size
        ) != 0
    )
    {
        return -1;
    }

    if (bno086_get_snapshot(&worker->imu, &snapshot) <= 0
        || !snapshot.startup_ready
        || !snapshot.accel_interval_valid || !snapshot.linear_accel_interval_valid)
    {
        snprintf(error, error_size, "BNO086 feature 설정 중 startup 상실");
        return -1;
    }
    pthread_mutex_lock(&worker->mutex);
    worker->raw_accel_interval_us = raw_interval_us;
    worker->linear_accel_interval_us = linear_interval_us;
    worker->ready_generation = snapshot.bno_generation;
    worker->ready_since_ns = monotonic_time_ns();
    worker->ready = true;
    pthread_mutex_unlock(&worker->mutex);
    return 0;
}

/* Each enabled 100 Hz stream has its own silence deadline. */
static bool imu_report_timed_out(
    uint64_t report_ns, uint64_t ready_since_ns, uint64_t now_ns
)
{
    if (ready_since_ns == 0U || now_ns < ready_since_ns)
    {
        return false;
    }
    /* Missing, previous-session or invalid future stamps start at session ready. */
    if (report_ns < ready_since_ns || report_ns > now_ns)
    {
        report_ns = ready_since_ns;
    }
    return now_ns - report_ns >= SENSOR_BNO_REPORT_TIMEOUT_NS;
}

static void *imu_thread_main(void *argument)
{
    imu_worker_t *worker = argument;
    char error[BNO086_ERROR_TEXT_SIZE] = "";

    aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_IMU, 30000U);
    if (imu_configure_session(worker, error, sizeof(error)) != 0)
    {
        imu_worker_publish_start(worker, false, error);
        bno086_close(&worker->imu);
        return NULL;
    }

    imu_worker_publish_start(worker, true, NULL);

    while (!imu_worker_stop_requested(worker))
    {
        aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_IMU, 30000U);
        bno086_snapshot_t snapshot = {0};
        (void)bno086_update(&worker->imu);
        (void)bno086_get_snapshot(&worker->imu, &snapshot);
        uint64_t now_ns = monotonic_time_ns();
        bool raw_timeout = imu_report_timed_out(
            snapshot.last_accel_update_ns, worker->ready_since_ns, now_ns);
        bool linear_timeout = imu_report_timed_out(
            snapshot.last_linear_accel_update_ns, worker->ready_since_ns, now_ns);
        if (!snapshot.startup_ready
            || snapshot.bno_generation != worker->ready_generation
            || raw_timeout || linear_timeout)
        {
            if (raw_timeout || linear_timeout)
                fprintf(stderr, "[BNO] 2초간 가속도 report 갱신 없음 (raw=%s, linear=%s). 설정을 복구합니다.\n",
                        raw_timeout ? "stalled" : "ok", linear_timeout ? "stalled" : "ok");
            else
                fprintf(stderr, "[BNO] 재시작 감지. 가속도 설정을 복구합니다.\n");
            while (!imu_worker_stop_requested(worker))
            {
                aruco_receiver_watchdog_progress(ARUCO_WATCHDOG_IMU, 30000U);
                if (imu_configure_session(worker, error, sizeof(error)) == 0)
                {
                    fprintf(stderr, "[BNO] 가속도 복구 완료: raw=%u us, linear=%u us\n",
                            worker->raw_accel_interval_us,
                            worker->linear_accel_interval_us);
                    break;
                }
                fprintf(stderr, "[BNO] 복구 실패: %s (0.5초 후 재시도)\n", error);
                for (unsigned int elapsed = 0U; elapsed < SENSOR_BNO_RETRY_US
                     && !imu_worker_stop_requested(worker);
                     elapsed += SENSOR_BNO_UPDATE_INTERVAL_US)
                    sleep_microseconds(SENSOR_BNO_UPDATE_INTERVAL_US);
            }
        }
        sleep_microseconds(SENSOR_BNO_UPDATE_INTERVAL_US);
    }

    bno086_close(&worker->imu);
    return NULL;
}

static int imu_worker_start(imu_worker_t *worker)
{
    bno086_config_t config;
    int result;

    memset(worker, 0, sizeof(*worker));
    if (pthread_mutex_init(&worker->mutex, NULL) != 0)
    {
        return -1;
    }
    if (pthread_cond_init(&worker->start_condition, NULL) != 0)
    {
        pthread_mutex_destroy(&worker->mutex);
        return -1;
    }

    bno086_default_config(&config);
    if (bno086_init(&worker->imu, &config) <= 0)
    {
        pthread_cond_destroy(&worker->start_condition);
        pthread_mutex_destroy(&worker->mutex);
        return -1;
    }

    result = pthread_create(&worker->thread, NULL, imu_thread_main, worker);
    if (result != 0)
    {
        fprintf(stderr, "[!] BNO086 Thread 생성 실패: %s\n", strerror(result));
        bno086_deinit(&worker->imu);
        pthread_cond_destroy(&worker->start_condition);
        pthread_mutex_destroy(&worker->mutex);
        return -1;
    }
    worker->thread_created = true;

    pthread_mutex_lock(&worker->mutex);
    while (!worker->start_finished)
    {
        pthread_cond_wait(&worker->start_condition, &worker->mutex);
    }
    result = worker->ready ? 0 : -1;
    pthread_mutex_unlock(&worker->mutex);

    return result;
}

static void imu_worker_stop(imu_worker_t *worker)
{
    if (!worker->thread_created)
    {
        return;
    }

    pthread_mutex_lock(&worker->mutex);
    worker->stop_requested = true;
    pthread_cond_broadcast(&worker->start_condition);
    pthread_mutex_unlock(&worker->mutex);

    pthread_join(worker->thread, NULL);
    bno086_deinit(&worker->imu);
    pthread_cond_destroy(&worker->start_condition);
    pthread_mutex_destroy(&worker->mutex);
    worker->thread_created = false;
}

/* ========================================================================== */
/* 사고차 내부 충돌 runtime의 시작·종료와 위치 갱신                            */
/* ========================================================================== */

/*
 * 기존 충돌 판정과 IPC queue를 ArUco 위치와 연결한다. ArUco가 이미 testbed
 * 절대 좌표와 방향을 주므로 시작 offset이나 BNO 방향 보정을 다시 적용하지
 * 않는다. 위치는 카메라가 알려주고 충돌 근거는 BNO 가속도에서 얻는다.
 */
struct accident_collision
{
    imu_worker_t imu;
    position_publication_t position;
    collision_worker_t collision;
    collision_ipc_worker_t ipc;
};

static bool accident_collision_detection_enabled(void)
{
    return collision_config_is_valid(&collision_config);
}

static void accident_collision_stop(accident_collision_t *runtime)
{
    if (runtime == NULL)
    {
        return;
    }
    /* consumer를 먼저 멈춰 닫힌 BNO나 파기된 위치 mutex를 읽지 않게 한다. */
    collision_worker_stop(&runtime->collision);
    collision_ipc_worker_stop(&runtime->ipc);
    imu_worker_stop(&runtime->imu);
    position_publication_deinitialize(&runtime->position);
    free(runtime);
}

static accident_collision_t *accident_collision_start(char *error, size_t error_size)
{
    accident_collision_t *runtime = calloc(1U, sizeof(*runtime));
    const char *failure = NULL;

    if (runtime == NULL)
    {
        failure = "collision runtime allocation failed";
    }
    else if (position_publication_initialize(&runtime->position) != 0)
    {
        failure = "position publication initialization failed";
    }
    else if (imu_worker_start(&runtime->imu) != 0)
    {
        failure = runtime->imu.start_error[0] != '\0'
            ? runtime->imu.start_error : "BNO acceleration startup failed";
    }
    else if (collision_ipc_worker_start(&runtime->ipc) != 0)
    {
        failure = "Accident IPC worker initialization failed";
    }
    else if (collision_worker_start(
            &runtime->collision, &runtime->imu,
            &runtime->position, &runtime->ipc
        ) != 0)
    {
        failure = "collision detector initialization failed";
    }

    if (failure != NULL)
    {
        /* 공통 BLE runner는 start 실패 시 stop을 부르지 않으므로 여기서 정리한다. */
        if (error != NULL && error_size != 0U)
        {
            snprintf(error, error_size, "%s", failure);
        }
        accident_collision_stop(runtime);
        return NULL;
    }
    fprintf(stderr, "[+] 사고 위치 시각 보정: camera_read_delay=%ums\n",
            collision_config.camera_read_delay_ms);
    if (error != NULL && error_size != 0U)
    {
        error[0] = '\0';
    }
    return runtime;
}

static void accident_collision_invalidate_pose(accident_collision_t *runtime)
{
    if (runtime != NULL)
    {
        position_publication_invalidate(&runtime->position);
    }
}

static void accident_collision_update_pose(
    accident_collision_t *runtime,
    double x_mm,
    double y_mm,
    double heading_deg,
    uint64_t capture_ns,
    uint64_t expires_ns
)
{
    uint64_t now_ns = monotonic_time_ns();
    position_publication_t *position;

    if (runtime == NULL)
    {
        return;
    }
    if (!isfinite(x_mm) || !isfinite(y_mm) || !isfinite(heading_deg)
        || fabs(x_mm) > 1000000.0 || fabs(y_mm) > 1000000.0
        || heading_deg < 0.0 || heading_deg >= 360.0
        || capture_ns == 0U || capture_ns > now_ns || now_ns >= expires_ns)
    {
        accident_collision_invalidate_pose(runtime);
        return;
    }

    position = &runtime->position;
    pthread_mutex_lock(&position->mutex);
    if (position->valid && now_ns >= position->expires_ns)
    {
        /* 두 번의 충돌 polling 사이에 만료/재수신되어도 측정 구간이 끊겼음을 남긴다. */
        position->generation++;
    }
    /* 기존 사고 payload 변환이 cm를 받으므로 여기서만 mm -> cm로 맞춘다. */
    position->x_cm = x_mm / 10.0;
    position->y_cm = y_mm / 10.0;
    position->heading_deg = heading_deg;
    position->capture_ns = capture_ns;
    position->expires_ns = expires_ns;
    position->valid = true;
    position->heading_valid = true;
    {
        size_t index;
        bool duplicate = false;

        /* 새 촬영 시각 기준 2초를 벗어난 이력은 제거한다. pending 후보는 별도 보존한다. */
        while (position->history_count > 0U)
        {
            uint64_t oldest_ns = position->history[position->history_head].capture_ns;
            if (capture_ns < oldest_ns || capture_ns - oldest_ns <= SENSOR_POSE_HISTORY_NS)
                break;
            position->history_head = (position->history_head + 1U) % SENSOR_POSE_HISTORY_CAPACITY;
            position->history_count--;
        }
        /* 같은 촬영 시각의 주기적 callback은 이력에 한 번만 보관한다. */
        for (index = 0U; index < position->history_count; index++)
        {
            size_t slot = (position->history_head + index) % SENSOR_POSE_HISTORY_CAPACITY;
            if (position->history[slot].capture_ns == capture_ns)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            size_t slot;
            if (position->history_count == SENSOR_POSE_HISTORY_CAPACITY)
            {
                position->history_head = (position->history_head + 1U) % SENSOR_POSE_HISTORY_CAPACITY;
                position->history_count--;
            }
            slot = (position->history_head + position->history_count) % SENSOR_POSE_HISTORY_CAPACITY;
            position->history[slot] = (position_sample_t){
                .x_cm = position->x_cm,
                .y_cm = position->y_cm,
                .heading_deg = position->heading_deg,
                .capture_ns = capture_ns,
                .received_ns = now_ns,
                .expires_ns = expires_ns
            };
            position->history_count++;
        }
    }
    pthread_mutex_unlock(&position->mutex);

    /* IMU sample을 기다리지 않고 사고 후 위치를 확보하는 즉시 전달한다. */
    if (runtime->collision.initialized)
        collision_worker_service_pending(&runtime->collision);
}

/* ========================================================================== */
/* ArUco 수신 callback과 사고차 main                                           */
/* ========================================================================== */

/*
 * 사고차는 ArUco로 받은 위치와 BNO 가속도를 함께 사용한다.
 * 위치만 왔다고 사고를 보내지는 않고, 기존 충돌 판정이 성립할 때 P2P·영상 IPC로 알린다.
 */
typedef struct
{
    accident_collision_t *collision;
} accident_aruco_context_t;

static int accident_start(void *context)
{
    accident_aruco_context_t *state = context;
    char error[256];

    /* BNO와 충돌/IPC thread를 먼저 준비한다. PAA 장치는 열지 않는다. */
    state->collision = accident_collision_start(error, sizeof(error));
    if (state->collision == NULL)
    {
        fprintf(stderr, "[-] BNO collision startup failed: %s\n", error);
        return -1;
    }
    puts("[+] BNO raw/linear acceleration collision worker ready; ArUco supplies pose.");
    if (!accident_collision_detection_enabled())
    {
        puts("[!] Collision IPC is disabled until measured RC-car thresholds are configured in accident_aruco.c.");
    }
    return 0;
}

static void accident_sample(
    const aruco_pose_t *pose,
    uint64_t now_ns,
    void *context
)
{
    accident_aruco_context_t *state = context;

    if (pose == NULL || !pose->valid || now_ns >= pose->expires_ns)
    {
        /* 마커를 잃거나 좌표가 오래되면 마지막 위치로 사고를 꾸며 보내지 않는다. */
        accident_collision_invalidate_pose(state->collision);
        return;
    }
    /* 수신 heading의 0.01도 단위를 기존 충돌 모듈이 받는 도 단위로 바꾼다. */
    accident_collision_update_pose(
        state->collision,
        (double)pose->x_mm,
        (double)pose->y_mm,
        (double)pose->heading_cdeg / 100.0,
        pose->capture_ns,
        pose->expires_ns
    );
}

static void accident_stop(void *context)
{
    accident_aruco_context_t *state = context;

    /* 위치를 참조하는 thread부터 멈춘 뒤 BNO와 publication을 정리한다. */
    accident_collision_stop(state->collision);
    state->collision = NULL;
}

int main(int argc, char **argv)
{
    accident_aruco_context_t context = {0};
    const aruco_receiver_callbacks_t callbacks = {
        .start = accident_start,
        .sample = accident_sample,
        .stop = accident_stop
    };

    return aruco_receiver_main(argc, argv, "accident", &callbacks, &context);
}
