/*
 * Hardware-free white-box regression of the retained BNO collision detector,
 * external-pose publication, and accident IPC payload. No Bluetooth, optical
 * motion sensor, or production IPC endpoint is opened by these tests.
 */
#define main accident_aruco_program_main
#include "../accident_aruco.c"
#undef main

#include <assert.h>
#include <signal.h>

#include <sys/socket.h>
#include <unistd.h>

static bool close_enough(double actual, double expected, double tolerance)
{
    return fabs(actual - expected) <= tolerance;
}

static collision_config_t make_collision_config(void)
{
    collision_config_t config;

    memset(&config, 0, sizeof(config));
    config.enabled = true;
    config.metric_mask = COLLISION_METRIC_RAW_ACCEL
                       | COLLISION_METRIC_LINEAR_ACCEL;
    config.minimum_evidence_count = 2U;
    config.delta_v_window = COLLISION_DV_WINDOW_20_MS;
    config.coincidence_window_ms = 100U;
    config.cooldown_ms = 100U;
    config.quiet_rearm_ms = 50U;
    config.require_position = true;
    config.raw_accel_threshold_mps2 = 5.0;
    config.linear_accel_threshold_mps2 = 5.0;
    return config;
}

static bno086_event_t make_collision_event(
    bno086_event_type_t type,
    uint8_t sequence,
    uint64_t generation,
    uint64_t timestamp_ns,
    double x
)
{
    bno086_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.report_sequence = sequence;
    event.bno_generation = generation;
    event.monotonic_ns = timestamp_ns;
    event.x = x;
    return event;
}

static bool test_detector_disabled_and_single_evidence(void)
{
    collision_config_t config = make_collision_config();
    collision_detector_state_t detector;
    collision_observation_t observation;
    bno086_event_t raw = make_collision_event(
        BNO086_EVENT_ACCELEROMETER,
        1U,
        1U,
        UINT64_C(1000000000),
        10.0
    );

    if (!collision_config_is_valid(&collision_config))
    {
        return false;
    }

    config.enabled = false;
    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, true);
    if (detector.armed
        || collision_detector_process(&detector, &raw, &observation))
    {
        return false;
    }

    config.enabled = true;
    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, false);
    if (collision_detector_process(&detector, &raw, &observation))
    {
        return false;
    }
    collision_detector_set_armed(&detector, true);
    return !collision_detector_process(&detector, &raw, &observation)
        && detector.evidence_ns[0] == raw.monotonic_ns
        && detector.evidence_ns[1] == 0U;
}

static bool test_detector_coincidence_latch_and_rearm(void)
{
    collision_config_t config = make_collision_config();
    collision_detector_state_t detector;
    collision_observation_t observation;
    bno086_event_t event;

    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, true);

    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 1U, 1U,
        UINT64_C(1000000000), 10.0
    );
    if (collision_detector_process(&detector, &event, &observation))
    {
        return false;
    }
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 1U, 1U,
        UINT64_C(1050000000), 8.0
    );
    if (!collision_detector_process(&detector, &event, &observation))
    {
        return false;
    }

    /* latch 중 추가 peak는 두 번째 이벤트가 아니다. */
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 2U, 1U,
        UINT64_C(1060000000), 11.0
    );
    if (collision_detector_process(&detector, &event, &observation))
    {
        return false;
    }

    /* cooldown과 quiet가 모두 지난 낮은 sample에서만 latch를 푼다. */
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 3U, 1U,
        UINT64_C(1200000000), 0.0
    );
    if (collision_detector_process(&detector, &event, &observation)
        || detector.latched)
    {
        return false;
    }
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 4U, 1U,
        UINT64_C(1210000000), 10.0
    );
    if (collision_detector_process(&detector, &event, &observation))
    {
        return false;
    }
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 2U, 1U,
        UINT64_C(1220000000), 8.0
    );
    return collision_detector_process(&detector, &event, &observation);
}

static bool test_detector_gap_and_generation_reset(void)
{
    collision_config_t config = make_collision_config();
    collision_detector_state_t detector;
    collision_observation_t observation;
    bno086_event_t event;

    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, true);
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 1U, 1U,
        UINT64_C(1000000000), 0.0
    );
    (void)collision_detector_process(&detector, &event, &observation);
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 1U, 1U,
        UINT64_C(1010000000), 10.0
    );
    (void)collision_detector_process(&detector, &event, &observation);
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 3U, 1U,
        UINT64_C(1020000000), 10.0
    );
    if (collision_detector_process(&detector, &event, &observation)
        || observation.linear_jerk_valid
        || detector.delta_v.count != 0U
        || detector.evidence_ns[0] != 0U)
    {
        return false;
    }

    collision_detector_set_armed(&detector, true);
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 1U, 1U,
        UINT64_C(2000000000), 0.0
    );
    (void)collision_detector_process(&detector, &event, &observation);
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 1U, 1U,
        UINT64_C(2010000000), 10.0
    );
    (void)collision_detector_process(&detector, &event, &observation);
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 2U, 2U,
        UINT64_C(2020000000), 10.0
    );
    return !collision_detector_process(&detector, &event, &observation)
        && !observation.linear_jerk_valid
        && detector.delta_v.count == 0U
        && detector.evidence_ns[0] == 0U;
}

static bool test_discontinuity_sample_cannot_trigger(void)
{
    collision_config_t config = make_collision_config();
    collision_detector_state_t detector;
    collision_observation_t observation;
    bno086_event_t event;

    config.metric_mask = COLLISION_METRIC_RAW_ACCEL;
    config.minimum_evidence_count = 1U;
    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, true);

    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 1U, 1U,
        UINT64_C(1000000000), 0.0
    );
    (void)collision_detector_process(&detector, &event, &observation);
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 3U, 1U,
        UINT64_C(1010000000), 10.0
    );
    if (collision_detector_process(&detector, &event, &observation)
        || detector.evidence_ns[0] != 0U)
    {
        return false;
    }
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 4U, 1U,
        UINT64_C(1020000000), 10.0
    );
    return collision_detector_process(&detector, &event, &observation);
}

static bool test_startup_loss_and_pose_independent_arming(void)
{
    collision_config_t config = make_collision_config();
    accident_collision_t runtime = {0};
    collision_worker_t *worker = &runtime.collision;
    collision_observation_t observation;
    bno086_event_t event;
    position_publication_t snapshot = {0};
    uint64_t now = monotonic_time_ns();
    bool passed = false;

    if (position_publication_initialize(&runtime.position) != 0)
    {
        return false;
    }
    collision_detector_initialize(&worker->detector, &config, 10000U, 10000U);
    worker->arm_requested = true;
    if (!collision_worker_apply_startup_locked(worker, true))
    {
        goto done;
    }
    accident_collision_update_pose(
        &runtime, 123.4, -56.7, 359.0,
        now - UINT64_C(1000000), now + UINT64_C(1000000000)
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    if (!worker->detector.armed)
    {
        goto done;
    }
    event = make_collision_event(
        BNO086_EVENT_ACCELEROMETER, 1U, 1U, now, 10.0
    );
    (void)collision_detector_process(&worker->detector, &event, &observation);
    if (worker->detector.evidence_ns[0] == 0U
        || collision_worker_apply_startup_locked(worker, false)
        || worker->detector.armed
        || worker->detector.evidence_ns[0] != 0U)
    {
        goto done;
    }
    if (!collision_worker_apply_startup_locked(worker, true)
        || !worker->detector.armed)
    {
        goto done;
    }
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 1U, 2U,
        now + UINT64_C(10000000), 10.0
    );
    if (collision_detector_process(&worker->detector, &event, &observation))
    {
        goto done;
    }

    /* Pose loss does not erase IMU evidence or disarm the detector. */
    accident_collision_invalidate_pose(&runtime);
    position_publication_snapshot(&runtime.position, &snapshot);
    if (snapshot.valid || !worker->detector.armed
        || worker->detector.evidence_ns[0] != 0U
        || worker->detector.evidence_ns[1] != event.monotonic_ns)
    {
        goto done;
    }
    accident_collision_update_pose(
        &runtime, 124.0, -56.0, 359.0,
        now - UINT64_C(1000000), now + UINT64_C(1000000000)
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    passed = snapshot.valid && worker->detector.armed
        && worker->detector.evidence_ns[0] == 0U
        && worker->detector.evidence_ns[1] == event.monotonic_ns;

done:
    position_publication_deinitialize(&runtime.position);
    return passed;
}

static bool test_detector_delta_v_20ms_parity(void)
{
    collision_config_t config = make_collision_config();
    collision_detector_state_t detector;
    collision_observation_t observation;
    bno086_event_t event;

    config.metric_mask = COLLISION_METRIC_LINEAR_DELTA_V;
    config.minimum_evidence_count = 1U;
    config.linear_delta_v_threshold_mps = 0.014;
    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, true);

    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 1U, 1U,
        UINT64_C(1000000000), 0.0
    );
    if (collision_detector_process(&detector, &event, &observation))
    {
        return false;
    }
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 2U, 1U,
        UINT64_C(1010000000), 1.0
    );
    if (collision_detector_process(&detector, &event, &observation)
        || observation.linear_delta_v_valid)
    {
        return false;
    }
    event = make_collision_event(
        BNO086_EVENT_LINEAR_ACCELERATION, 3U, 1U,
        UINT64_C(1020000000), 1.0
    );
    return collision_detector_process(&detector, &event, &observation)
        && observation.linear_delta_v_valid
        && close_enough(observation.linear_delta_v_mps, 0.015, 1.0e-12);
}

static bool test_payload_mapping_and_ipc_frame(void)
{
    collision_config_t config = make_collision_config();
    accident_collision_t runtime = {0};
    position_publication_t position;
    uint64_t now = monotonic_time_ns();
    accident_ipc_event_t sent_payload;
    accident_ipc_event_t received_payload;
    ipc_header_t sent_header;
    ipc_header_t received_header;
    int sockets[2];
    bool passed;

    memset(&position, 0, sizeof(position));
    if (position_publication_initialize(&runtime.position) != 0)
    {
        return false;
    }
    accident_collision_update_pose(
        &runtime, 123.4, -56.7, 359.0,
        now - UINT64_C(1000000), now + UINT64_C(1000000000)
    );
    position_publication_snapshot(&runtime.position, &position);
    position_publication_deinitialize(&runtime.position);
    if (!position.valid || !position.heading_valid
        || !close_enough(position.heading_deg, 359.0, 1.0e-12))
    {
        return false;
    }
    /* Preserve the payload helper's heading-normalization regression too. */
    position.heading_deg = -1.0;
    if (!collision_fill_payload(
            &config,
            &position,
            INT64_C(123456),
            &sent_payload
        ))
    {
        return false;
    }
    if (sent_payload.station_id != 1U
        || sent_payload.sequence_number != 0U
        || sent_payload.detection_time != INT64_C(123456)
        || sent_payload.heading_value != 3590U
        || sent_payload.local_x_mm != 123
        || sent_payload.local_y_mm != -57)
    {
        return false;
    }

    memset(&position, 0, sizeof(position));
    if (collision_fill_payload(
            &config,
            &position,
            INT64_C(1),
            &received_payload
        ))
    {
        return false;
    }
    position.valid = true;
    position.x_cm = 100000.1;
    position.y_cm = 0.0;
    if (collision_fill_payload(
            &config,
            &position,
            INT64_C(1),
            &received_payload
        ))
    {
        return false;
    }
    position.x_cm = 0.0;
    if (collision_fill_payload(
            &config,
            &position,
            INT64_C(0),
            &received_payload
        ))
    {
        return false;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        return false;
    }
    memset(&sent_header, 0, sizeof(sent_header));
    sent_header.type = IPC_MSG_ACCIDENT_EVENT;
    sent_header.sequence = 7U;
    sent_payload.sequence_number = 7U;
    sent_header.timestamp = UINT64_C(987654321);
    memset(&received_header, 0, sizeof(received_header));
    memset(&received_payload, 0, sizeof(received_payload));

    passed = ipc_send(
            sockets[0],
            &sent_header,
            &sent_payload,
            sizeof(sent_payload)
        ) == IPC_SUCCESS
        && ipc_recv(
            sockets[1],
            &received_header,
            &received_payload,
            sizeof(received_payload)
        ) == IPC_SUCCESS
        && received_header.type == IPC_MSG_ACCIDENT_EVENT
        && received_header.sequence == 7U
        && received_header.timestamp == UINT64_C(987654321)
        && received_header.length == sizeof(received_payload)
        && received_payload.station_id == 1U
        && received_payload.sequence_number == received_header.sequence
        && received_payload.detection_time == sent_payload.detection_time
        && received_payload.local_x_mm == sent_payload.local_x_mm
        && received_payload.local_y_mm == sent_payload.local_y_mm
        && received_payload.heading_value == sent_payload.heading_value;
    close(sockets[0]);
    close(sockets[1]);
    return passed;
}

static bool run_collision_core_tests(void)
{
    bool disabled = test_detector_disabled_and_single_evidence();
    bool coincidence = test_detector_coincidence_latch_and_rearm();
    bool gap = test_detector_gap_and_generation_reset();
    bool boundary = test_discontinuity_sample_cannot_trigger();
    bool startup = test_startup_loss_and_pose_independent_arming();
    bool delta_v = test_detector_delta_v_20ms_parity();
    bool payload = test_payload_mapping_and_ipc_frame();
    bool passed = disabled && coincidence && gap
               && boundary && startup && delta_v && payload;

    puts("=== C ACCIDENT COLLISION REGRESSION ===");
    printf("disabled/disarmed + single evidence                : %s\n",
           disabled ? "PASS" : "FAIL");
    printf("coincidence -> one event -> cooldown/quiet rearm   : %s\n",
           coincidence ? "PASS" : "FAIL");
    printf("sequence/generation gap resets evidence and DV    : %s\n",
           gap ? "PASS" : "FAIL");
    printf("discontinuity sample cannot trigger min=1         : %s\n",
           boundary ? "PASS" : "FAIL");
    printf("BNO startup resets; marker loss preserves evidence : %s\n",
           startup ? "PASS" : "FAIL");
    printf("20 ms trapezoid sliding delta-V = 0.015 m/s       : %s\n",
           delta_v ? "PASS" : "FAIL");
    printf("ArUco pose/sensor payload + socketpair IPC frame   : %s\n",
           payload ? "PASS" : "FAIL");
    printf("FINAL RESULT=%s\n", passed ? "PASS" : "FAIL");
    return passed;
}

static collision_config_t test_config(void)
{
    collision_config_t config = collision_config;

    /* Synthetic thresholds for the regression cases below. */
    config.enabled = true;
    config.metric_mask = COLLISION_METRIC_RAW_ACCEL | COLLISION_METRIC_LINEAR_ACCEL;
    config.minimum_evidence_count = 2U;
    config.raw_accel_threshold_mps2 = 5.0;
    config.linear_accel_threshold_mps2 = 5.0;
    return config;
}

static void test_absolute_pose_payload(void)
{
    accident_collision_t runtime = {0};
    position_publication_t snapshot = {0};
    accident_ipc_event_t payload;
    uint64_t now = monotonic_time_ns();

    assert(position_publication_initialize(&runtime.position) == 0);
    accident_collision_update_pose(
        &runtime, 1234.0, -5678.0, 123.45, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(snapshot.valid && snapshot.heading_valid);
    assert(collision_fill_payload(&collision_config, &snapshot, 12345, &payload));
    assert(payload.local_x_mm == 1234 && payload.local_y_mm == -5678);
    assert(payload.heading_value == 1235U);
    assert(payload.station_id == 1U);
    assert(payload.detection_time == 12345);
    position_publication_deinitialize(&runtime.position);
    puts("absolute ArUco testbed pose -> sensor accident payload: PASS");
}

static void test_expiry_and_invalid_pose(void)
{
    accident_collision_t runtime = {0};
    position_publication_t snapshot = {0};
    accident_ipc_event_t payload;
    uint64_t now = monotonic_time_ns();

    assert(position_publication_initialize(&runtime.position) == 0);
    accident_collision_update_pose(
        &runtime, 10.0, 20.0, 90.0, now - 1000000U, now + 1000000000U
    );
    /* Simulate time expiry without waiting or calling the BLE sampling timer. */
    runtime.position.expires_ns = now - 1U;
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(!snapshot.valid && !snapshot.heading_valid);
    assert(!collision_fill_payload(&collision_config, &snapshot, 12345, &payload));

    accident_collision_update_pose(
        &runtime, NAN, 20.0, 90.0, now - 1000000U, now + 1000000000U
    );
    assert(!runtime.position.valid);
    accident_collision_update_pose(
        &runtime, 1000001.0, 20.0, 90.0, now - 1000000U, now + 1000000000U
    );
    assert(!runtime.position.valid);
    accident_collision_update_pose(
        &runtime, 10.0, 20.0, 360.0, now - 1000000U, now + 1000000000U
    );
    assert(!runtime.position.valid);
    accident_collision_update_pose(
        &runtime, 10.0, 20.0, 90.0, now + 1000000000U, now + 2000000000U
    );
    assert(!runtime.position.valid);
    accident_collision_update_pose(
        &runtime, 10.0, 20.0, 90.0, now - 1000000U, now - 1U
    );
    assert(!runtime.position.valid);
    position_publication_deinitialize(&runtime.position);
    puts("expiry without BLE timer + invalid/range/future pose rejection: PASS");
}

static void test_pose_updates_preserve_collision_latch(void)
{
    accident_collision_t runtime = {0};
    collision_config_t config = test_config();
    position_publication_t snapshot = {0};
    collision_observation_t observation;
    bno086_event_t event = {0};
    collision_worker_t *worker = &runtime.collision;
    uint64_t now = monotonic_time_ns();

    assert(position_publication_initialize(&runtime.position) == 0);
    collision_detector_initialize(&worker->detector, &config, 10000U, 10000U);
    worker->arm_requested = true;
    assert(collision_worker_apply_startup_locked(worker, true));
    accident_collision_update_pose(
        &runtime, 10.0, 20.0, 90.0, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(worker->detector.armed);

    event.type = BNO086_EVENT_ACCELEROMETER;
    event.report_sequence = 1U;
    event.bno_generation = 1U;
    event.monotonic_ns = now;
    event.x = 6.0;
    assert(!collision_detector_process(&worker->detector, &event, &observation));

    /* A new camera pose must preserve the first metric's coincidence evidence. */
    accident_collision_update_pose(
        &runtime, 11.0, 20.0, 91.0, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(worker->detector.evidence_ns[0] == now);
    event.type = BNO086_EVENT_LINEAR_ACCELERATION;
    event.monotonic_ns = now + 1000000U;
    assert(collision_detector_process(&worker->detector, &event, &observation));
    assert(worker->detector.latched);

    accident_collision_update_pose(
        &runtime, 12.0, 20.0, 92.0, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    event.report_sequence = 2U;
    event.monotonic_ns = now + 2000000U;
    assert(!collision_detector_process(&worker->detector, &event, &observation));
    assert(worker->detector.latched);

    /* Loss and reacquisition cannot turn a latched collision into a new event. */
    accident_collision_invalidate_pose(&runtime);
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(!snapshot.valid && worker->detector.armed && worker->detector.latched);
    accident_collision_update_pose(
        &runtime, 13.0, 20.0, 93.0, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(snapshot.valid && worker->detector.armed && worker->detector.latched);
    assert(worker->detector.evidence_ns[0] == now);
    assert(worker->detector.evidence_ns[1] == now + 2000000U);

    /* Expiry affects pose validity, not the independent IMU detector. */
    runtime.position.expires_ns = now - 1U;
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(!snapshot.valid && worker->detector.armed && worker->detector.latched);
    position_publication_deinitialize(&runtime.position);
    puts("camera update/loss/reacquisition/expiry retain IMU evidence and latch: PASS");
}

static void test_stale_gap_preserves_latch_and_bno_rearms_cleanly(void)
{
    accident_collision_t runtime = {0};
    collision_config_t config = test_config();
    position_publication_t snapshot = {0};
    uint64_t now = monotonic_time_ns();

    assert(position_publication_initialize(&runtime.position) == 0);
    collision_detector_initialize(&runtime.collision.detector, &config, 10000U, 10000U);
    runtime.collision.arm_requested = true;
    assert(collision_worker_apply_startup_locked(&runtime.collision, true));
    accident_collision_update_pose(
        &runtime, 10.0, 20.0, 90.0, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    runtime.collision.detector.latched = true;
    runtime.position.expires_ns = now - 1U;

    /* Expire and refresh between worker polls without resetting the IMU latch. */
    accident_collision_update_pose(
        &runtime, 11.0, 20.0, 90.0, now - 1000000U, now + 1000000000U
    );
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(snapshot.valid);
    assert(runtime.collision.detector.armed && runtime.collision.detector.latched);
    assert(!collision_worker_apply_startup_locked(&runtime.collision, false));
    assert(!runtime.collision.detector.armed && !runtime.collision.detector.latched);
    assert(collision_worker_apply_startup_locked(&runtime.collision, true));
    assert(runtime.collision.detector.armed && !runtime.collision.detector.latched);
    position_publication_deinitialize(&runtime.position);
    puts("stale camera gap preserves latch; BNO startup recovery resets segment: PASS");
}

/* Exercise the bounded queue without creating its production IPC thread. */
static void test_ipc_queue_initialize(collision_ipc_worker_t *ipc)
{
    memset(ipc, 0, sizeof(*ipc));
    assert(pthread_mutex_init(&ipc->mutex, NULL) == 0);
    assert(pthread_cond_init(&ipc->condition, NULL) == 0);
    ipc->initialized = true;
    ipc->next_sequence = 1U;
}

static void test_ipc_queue_deinitialize(collision_ipc_worker_t *ipc)
{
    assert(!ipc->thread_created);
    assert(pthread_cond_destroy(&ipc->condition) == 0);
    assert(pthread_mutex_destroy(&ipc->mutex) == 0);
    ipc->initialized = false;
}

static void add_history_pose(
    position_publication_t *position, uint64_t capture_ns, uint64_t received_ns,
    uint64_t expires_ns, double x_cm
)
{
    assert(position->history_count < SENSOR_POSE_HISTORY_CAPACITY);
    position->history[position->history_count++] = (position_sample_t){
        .x_cm = x_cm, .y_cm = -56.78, .heading_deg = 123.45,
        .capture_ns = capture_ns, .received_ns = received_ns, .expires_ns = expires_ns
    };
}

static void assert_pending_time(
    const collision_pending_t *pending, uint64_t event_ns, int64_t detection_time
)
{
    assert(pending->active);
    assert(pending->item.monotonic_ns == event_ns);
    assert(pending->item.payload.detection_time == detection_time);
}

/* Exercise the real sender and wire readers using private socket pairs. */
static void test_deliver_queue_head(
    collision_ipc_worker_t *ipc, bool video, const collision_ipc_item_t *expected
)
{
    int sockets[2];
    int fd;
    ipc_header_t header = {0};
    union {
        accident_ipc_event_t p2p;
        video_processor_event_t video;
    } received = {0};

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    fd = sockets[0];
    assert(collision_ipc_send_head(ipc, video, &fd));
    assert(ipc_recv(sockets[1], &header, &received, sizeof(received)) == IPC_SUCCESS);
    assert(header.sequence == expected->payload.sequence_number);
    assert(header.timestamp == expected->monotonic_ns);
    if (video)
    {
        assert(fd == -1);
        assert(header.type == IPC_MSG_VIDEO_PROCESSOR_EVENT);
        assert(header.length == sizeof(received.video));
        assert(received.video.station_id == expected->payload.station_id);
        assert(received.video.sequence_number == expected->payload.sequence_number);
        assert(received.video.detection_time == expected->payload.detection_time);
    }
    else
    {
        assert(header.type == IPC_MSG_ACCIDENT_EVENT);
        assert(header.length == sizeof(received.p2p));
        assert(memcmp(&received.p2p, &expected->payload, sizeof(received.p2p)) == 0);
    }
    if (fd >= 0)
        close(fd);
    close(sockets[1]);
}

static void test_collision_selects_post_impact_pose_immediately_and_preserves_event(void)
{
    collision_config_t config = make_collision_config();
    collision_detector_state_t detector;
    collision_pending_t pending = {0};
    collision_ipc_item_t original_event;
    collision_ipc_worker_t ipc;
    collision_observation_t observation;
    position_publication_t position = {0};
    bno086_event_t event;
    const uint64_t impact_ns = UINT64_C(3000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000123);

    test_ipc_queue_initialize(&ipc);
    collision_detector_initialize(&detector, &config, 10000U, 10000U);
    collision_detector_set_armed(&detector, true);
    event = make_collision_event(BNO086_EVENT_ACCELEROMETER, 1U, 1U,
                                 impact_ns - UINT64_C(10000000), 10.0);
    assert(!collision_detector_process(&detector, &event, &observation));
    event = make_collision_event(BNO086_EVENT_LINEAR_ACCELERATION, 1U, 1U, impact_ns, 8.0);
    assert(collision_detector_process(&detector, &event, &observation));
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    assert(!collision_pending_record(&pending, impact_ns + 1U, detection_time + 1, &ipc));
    assert_pending_time(&pending, impact_ns, detection_time);
    original_event = pending.item;
    assert(original_event.payload.station_id == SENSOR_STATION_ID);
    assert(original_event.payload.sequence_number == 1U && ipc.next_sequence == 2U);
    assert(ipc.video.count == 1U && ipc.p2p.count == 0U);
    /* Own video is already deliverable before the first pose lookup. */
    test_deliver_queue_head(&ipc, true, &original_event);
    assert(ipc.video.count == 0U && ipc.sent == 1U && !pending.pose_resolved);

    /* Expired live state still contains measurements accepted before the impact. */
    add_history_pose(&position, impact_ns - UINT64_C(200000000),
                     impact_ns - UINT64_C(100000000), impact_ns + UINT64_C(100000000), 11.0);
    assert(!position.valid);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, impact_ns, &ipc));
    assert(!pending.candidate_valid && !pending.pose_confirmed && ipc.p2p.count == 0U);

    /* The first post-impact frame is sent as soon as it arrives, before the deadline. */
    add_history_pose(&position, impact_ns + UINT64_C(50000000),
                     impact_ns + UINT64_C(250000000), impact_ns + UINT64_C(350000000), 12.34);
    assert(!collision_pending_try_enqueue(&pending, &config, &position,
                                          impact_ns + UINT64_C(250000000) - 1U, &ipc));
    assert(!pending.candidate_valid);
    assert(collision_pending_try_enqueue(&pending, &config, &position,
                                         impact_ns + UINT64_C(250000000), &ipc));
    assert(!pending.active && ipc.p2p.count == 1U);
    assert(ipc.p2p.items[0].monotonic_ns == impact_ns);
    assert(ipc.p2p.items[0].payload.detection_time == detection_time);
    assert(ipc.p2p.items[0].payload.local_x_mm == 123);
    assert(ipc.p2p.items[0].payload.local_y_mm == -568);
    assert(ipc.p2p.items[0].payload.heading_value == 1235U);
    assert(ipc.p2p.items[0].payload.sequence_number == original_event.payload.sequence_number);
    assert(ipc.video.count == 0U); /* Pose completion must not recreate the own-video request. */
    test_deliver_queue_head(&ipc, false, &ipc.p2p.items[0]);
    assert(ipc.p2p.count == 0U && ipc.sent == 2U);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns + 1U, &ipc));
    assert(ipc.video.count == 0U && ipc.p2p.count == 0U && ipc.next_sequence == 2U);
    test_ipc_queue_deinitialize(&ipc);
    puts("own video immediate; first post-impact pose sends P2P immediately with identical identity/time: PASS");
}

static void test_pending_post_impact_history_bounds(void)
{
    const uint64_t impact_ns = UINT64_C(5000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000123);
    collision_config_t config = make_collision_config();
    collision_pending_t pending = {0};
    collision_ipc_worker_t ipc;
    position_publication_t position = {0};

    test_ipc_queue_initialize(&ipc);
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    add_history_pose(&position, impact_ns + UINT64_C(50000000),
                     impact_ns + UINT64_C(100000000), deadline_ns + 1U, 1.0);
    add_history_pose(&position, impact_ns - UINT64_C(50000000),
                     impact_ns + UINT64_C(100000000), deadline_ns + 1U, 2.0);
    add_history_pose(&position, impact_ns + UINT64_C(20000000),
                     impact_ns + UINT64_C(100000000), deadline_ns + 1U, 7.0);
    assert(collision_pending_try_enqueue(&pending, &config, &position,
                                         impact_ns + UINT64_C(100000000), &ipc));
    assert(ipc.p2p.items[0].payload.local_x_mm == 70); /* Earliest post-impact measurement wins. */

    memset(&position, 0, sizeof(position));
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    add_history_pose(&position, impact_ns - 1U, impact_ns, deadline_ns + 1U, 3.0);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns - 1U, &ipc));
    assert(pending.active && !pending.candidate_valid);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns, &ipc));
    assert(!pending.active && ipc.p2p.count == 1U); /* Past-only history times out. */

    memset(&position, 0, sizeof(position));
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    add_history_pose(&position, impact_ns - UINT64_C(10000000), deadline_ns + 1U,
                     deadline_ns + UINT64_C(100000000), 4.0);
    add_history_pose(&position, deadline_ns + 1U, deadline_ns + 2U,
                     deadline_ns + UINT64_C(100000000), 5.0);
    assert(!collision_pending_try_enqueue(&pending, &config, &position,
                                          deadline_ns + UINT64_C(1000000000), &ipc));
    assert(!pending.active && ipc.p2p.count == 1U); /* Late processing cannot accept late arrival. */

    memset(&position, 0, sizeof(position));
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    add_history_pose(&position, impact_ns, deadline_ns, deadline_ns + 1U, 6.0);
    assert(collision_pending_try_enqueue(&pending, &config, &position,
                                         deadline_ns + UINT64_C(1000000000), &ipc));
    assert(ipc.p2p.items[1].payload.local_x_mm == 60); /* Equality at impact and receipt deadline works. */

    memset(&position, 0, sizeof(position));
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    add_history_pose(&position, deadline_ns, deadline_ns, deadline_ns + 1U, 8.0);
    assert(collision_pending_try_enqueue(&pending, &config, &position, deadline_ns, &ipc));
    assert(ipc.p2p.items[2].payload.local_x_mm == 80); /* Exact upper measurement bound is inclusive. */
    test_ipc_queue_deinitialize(&ipc);
    puts("earliest post-impact history, past-only timeout, inclusive bounds and late-arrival rejection: PASS");
}

static void test_camera_delay_keeps_measurements_post_impact_and_receipts_in_window(void)
{
    const uint64_t impact_ns = UINT64_C(5000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    collision_config_t config = make_collision_config();
    collision_pending_t pending = {0};
    collision_ipc_worker_t ipc;
    position_publication_t position = {0};

    config.camera_read_delay_ms = 357U;
    test_ipc_queue_initialize(&ipc);
    assert(collision_pending_record(&pending, impact_ns, INT64_C(1700000000123), &ipc));
    add_history_pose(&position, impact_ns - UINT64_C(10000000),
                     impact_ns + UINT64_C(10000000), impact_ns + UINT64_C(290000000), 1.0);
    add_history_pose(&position, impact_ns + UINT64_C(250000000),
                     impact_ns + UINT64_C(280000000), impact_ns + UINT64_C(550000000), 2.0);
    assert(!collision_pending_try_enqueue(&pending, &config, &position,
                                          impact_ns + UINT64_C(280000000), &ipc));
    assert(!pending.candidate_valid && pending.active);
    /* The corrected measurement is exactly at impact and is sent at receipt. */
    add_history_pose(&position, impact_ns + UINT64_C(357000000),
                     impact_ns + UINT64_C(400000000), impact_ns + UINT64_C(657000000), 3.0);
    assert(collision_pending_try_enqueue(&pending, &config, &position,
                                         impact_ns + UINT64_C(400000000), &ipc));
    assert(ipc.p2p.items[0].payload.local_x_mm == 30);
    assert(ipc.p2p.items[0].monotonic_ns == impact_ns);
    assert(ipc.p2p.items[0].payload.detection_time == INT64_C(1700000000123));
    assert(position.history[1].capture_ns == impact_ns + UINT64_C(250000000));
    assert(position.history[1].expires_ns == impact_ns + UINT64_C(550000000));
    memset(&position, 0, sizeof(position));
    assert(collision_pending_record(&pending, impact_ns, INT64_C(1700000000123), &ipc));
    add_history_pose(&position, UINT64_C(100), UINT64_C(200), UINT64_C(300), 4.0);
    add_history_pose(&position, impact_ns + UINT64_C(357000000) - 1U,
                     impact_ns + UINT64_C(400000000),
                     impact_ns + UINT64_C(657000000), 5.0);
    add_history_pose(&position, impact_ns + UINT64_C(357000000), deadline_ns + 1U,
                     deadline_ns + UINT64_C(100000000), 6.0);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns + 1U, &ipc));
    assert(!pending.active && ipc.p2p.count == 1U);
    test_ipc_queue_deinitialize(&ipc);
    puts("357ms correction rejects pre-impact measurements; original BLE age and receipt deadline remain: PASS");
}

static void test_pending_rejects_unusable_history_and_preserves_own_video(void)
{
    const uint64_t impact_ns = UINT64_C(5000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000123);
    collision_config_t config = make_collision_config();
    collision_pending_t pending = {0};
    collision_ipc_worker_t ipc;
    position_publication_t position;
    size_t scenario;

    test_ipc_queue_initialize(&ipc);
    assert(!collision_pending_record(&pending, 0U, detection_time, &ipc));
    assert(!collision_pending_record(&pending, impact_ns, 0, &ipc));
    assert(!collision_pending_record(&pending, impact_ns, -1, &ipc));
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    assert(!collision_pending_try_enqueue(&pending, &config, NULL, impact_ns - 1U, &ipc));
    for (scenario = 0U; scenario < 11U; scenario++)
    {
        position_sample_t *sample;
        memset(&position, 0, sizeof(position));
        add_history_pose(&position, impact_ns, impact_ns, deadline_ns + 1U, 12.34);
        sample = &position.history[0];
        switch (scenario)
        {
        case 0U: sample->capture_ns = 0U; break;
        case 1U: sample->received_ns = sample->capture_ns - 1U; break;
        case 2U: sample->capture_ns = impact_ns - 1U; break;
        case 3U: sample->received_ns = deadline_ns + 1U; break;
        case 4U: sample->expires_ns = sample->received_ns; break;
        case 5U: sample->x_cm = NAN; break;
        case 6U: sample->y_cm = 100000.1; break;
        case 7U: sample->heading_deg = NAN; break;
        case 8U: sample->heading_deg = -0.01; break;
        case 9U: sample->heading_deg = 360.0; break;
        case 10U: sample->y_cm = INFINITY; break;
        default: assert(false);
        }
        assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns - 1U, &ipc));
        assert(!pending.candidate_valid && !pending.pose_confirmed && ipc.p2p.count == 0U);
        assert_pending_time(&pending, impact_ns, detection_time);
    }
    config.require_position = false;
    assert(!collision_pending_try_enqueue(&pending, &config, NULL, deadline_ns, &ipc));
    assert(!pending.active && ipc.p2p.count == 0U);
    assert(ipc.video.count == 1U && ipc.next_sequence == 2U);
    assert(ipc.video.items[0].payload.detection_time == detection_time);
    test_deliver_queue_head(&ipc, true, &ipc.video.items[0]);
    assert(ipc.video.count == 0U && ipc.sent == 1U);
    memset(&position, 0, sizeof(position));
    add_history_pose(&position, deadline_ns + 2U, deadline_ns + 2U,
                     deadline_ns + UINT64_C(300000000), 99.0);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns + 2U, &ipc));
    assert(ipc.p2p.count == 0U); /* Moved car cannot resurrect an old accident. */
    assert(collision_pending_record(&pending, deadline_ns + 2U, detection_time + 302, &ipc));
    assert(collision_pending_try_enqueue(&pending, &config, &position,
                                         deadline_ns + 2U + SENSOR_POSE_MATCH_WAIT_NS, &ipc));
    assert(ipc.p2p.count == 1U); /* A new accident can proceed. */
    test_ipc_queue_deinitialize(&ipc);
    puts("invalid history suppresses only P2P; own video survives, late relocation cannot revive P2P: PASS");
}

static void test_pending_queue_backpressure_freezes_selected_pose(void)
{
    collision_pending_t pending = {0};
    collision_config_t config = make_collision_config();
    collision_ipc_worker_t ipc;
    collision_ipc_item_t occupied = {0};
    collision_ipc_item_t frozen;
    position_publication_t position = {0};
    const uint64_t impact_ns = UINT64_C(2000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000123);
    size_t index;
    size_t queued_index;

    test_ipc_queue_initialize(&ipc);
    ipc.p2p.head = SENSOR_IPC_QUEUE_CAPACITY - 1U;
    for (index = 0U; index < SENSOR_IPC_QUEUE_CAPACITY; index++)
    {
        occupied.monotonic_ns = index + 1U;
        assert(collision_ipc_enqueue(&ipc, &occupied, false));
    }
    assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
    add_history_pose(&position, impact_ns, impact_ns, deadline_ns, 12.34);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, impact_ns, &ipc));
    assert(pending.candidate_valid && pending.pose_confirmed);
    memset(&position, 0, sizeof(position)); /* Candidate remains if history is overwritten. */
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns, &ipc));
    assert(pending.pose_confirmed && ipc.p2p.count == SENSOR_IPC_QUEUE_CAPACITY);
    assert_pending_time(&pending, impact_ns, detection_time);
    assert(pending.item.payload.local_x_mm == 123);
    assert(pending.item.payload.local_y_mm == -568);
    assert(pending.item.payload.heading_value == 1235U);
    frozen = pending.item;

    add_history_pose(&position, impact_ns, impact_ns + 1U, deadline_ns, 999.0);
    assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns + 1U, &ipc));
    assert(memcmp(&pending.item, &frozen, sizeof(frozen)) == 0);
    pthread_mutex_lock(&ipc.mutex);
    ipc.p2p.head = (ipc.p2p.head + 1U) % SENSOR_IPC_QUEUE_CAPACITY;
    ipc.p2p.count--;
    pthread_mutex_unlock(&ipc.mutex);
    assert(collision_pending_try_enqueue(&pending, &config, NULL, deadline_ns + UINT64_C(1000000000), &ipc));
    assert(!pending.active && ipc.p2p.count == SENSOR_IPC_QUEUE_CAPACITY);
    queued_index = (ipc.p2p.head + ipc.p2p.count - 1U) % SENSOR_IPC_QUEUE_CAPACITY;
    assert(memcmp(&ipc.p2p.items[queued_index], &frozen, sizeof(frozen)) == 0);
    for (index = 0U; index + 1U < SENSOR_IPC_QUEUE_CAPACITY; index++)
    {
        size_t retained_index = (ipc.p2p.head + index) % SENSOR_IPC_QUEUE_CAPACITY;
        assert(ipc.p2p.items[retained_index].monotonic_ns == index + 2U);
    }
    assert(!collision_pending_try_enqueue(&pending, &config, NULL, deadline_ns + UINT64_C(2000000000), &ipc));
    test_ipc_queue_deinitialize(&ipc);
    puts("pending candidate and confirmed payload survive history overwrite and queue backpressure: PASS");
}

static void test_own_video_backpressure_survives_pose_resolution(void)
{
    const uint64_t impact_ns = UINT64_C(5000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000123);
    collision_config_t config = make_collision_config();

    for (unsigned int has_pose = 0U; has_pose < 2U; ++has_pose)
    {
        collision_ipc_worker_t ipc;
        collision_pending_t pending = {0};
        collision_ipc_item_t occupied = {0};
        position_publication_t position = {0};
        size_t tail;

        test_ipc_queue_initialize(&ipc);
        ipc.next_sequence = UINT16_MAX;
        for (size_t index = 0U; index < SENSOR_IPC_QUEUE_CAPACITY; ++index)
            assert(collision_ipc_enqueue(&ipc, &occupied, true));
        assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
        assert(!pending.video_queued && ipc.next_sequence == 1U);
        if (has_pose)
            add_history_pose(&position, impact_ns, impact_ns, deadline_ns + 1U, 12.34);
        assert(collision_pending_try_enqueue(&pending, &config, &position, deadline_ns, &ipc)
               == (has_pose != 0U));
        assert(pending.active && pending.pose_resolved && !pending.video_queued);
        assert(pending.pose_confirmed == (has_pose != 0U));
        assert(ipc.p2p.count == has_pose);
        assert(pending.item.payload.sequence_number == UINT16_MAX);

        /* Space becomes available after pose processing. Do not create a second P2P item. */
        ipc.video.head = (ipc.video.head + 1U) % SENSOR_IPC_QUEUE_CAPACITY;
        ipc.video.count--;
        memset(&position, 0, sizeof(position));
        add_history_pose(&position, deadline_ns + 1U, deadline_ns + 2U,
                         deadline_ns + UINT64_C(100000000), 99.0);
        assert(!collision_pending_try_enqueue(&pending, &config, &position, deadline_ns + 2U, &ipc));
        assert(!pending.active && ipc.video.count == SENSOR_IPC_QUEUE_CAPACITY);
        assert(ipc.p2p.count == has_pose && ipc.next_sequence == 1U);
        tail = (ipc.video.head + ipc.video.count - 1U) % SENSOR_IPC_QUEUE_CAPACITY;
        assert(ipc.video.items[tail].payload.sequence_number == UINT16_MAX);
        assert(ipc.video.items[tail].payload.detection_time == detection_time);
        assert(ipc.video.items[tail].monotonic_ns == impact_ns);
        test_ipc_queue_deinitialize(&ipc);
    }
    puts("own-video backpressure survives located/unlocated resolution; frozen ID wraps once: PASS");
}

static void test_ipc_targets_retry_independently(void)
{
    const uint64_t impact_ns = UINT64_C(5000000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000123);
    collision_config_t config = make_collision_config();
    struct sigaction ignored = {0};
    struct sigaction previous;

    /* Production's aruco_receiver_main also ignores SIGPIPE. */
    ignored.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignored.sa_mask) == 0);
    assert(sigaction(SIGPIPE, &ignored, &previous) == 0);
    for (unsigned int failed_video = 0U; failed_video < 2U; ++failed_video)
    {
        collision_ipc_worker_t ipc;
        collision_pending_t pending = {0};
        position_publication_t position = {0};
        collision_ipc_item_t expected;
        collision_ipc_queue_t *failed;
        collision_ipc_queue_t *successful;
        int sockets[2];
        int fd;
        int empty_fd = -1;

        test_ipc_queue_initialize(&ipc);
        assert(collision_pending_record(&pending, impact_ns, detection_time, &ipc));
        add_history_pose(&position, impact_ns, impact_ns, deadline_ns + 1U, 12.34);
        assert(collision_pending_try_enqueue(&pending, &config, &position, deadline_ns, &ipc));
        expected = ipc.p2p.items[0];
        failed = failed_video ? &ipc.video : &ipc.p2p;
        successful = failed_video ? &ipc.p2p : &ipc.video;

        /* A closed peer forces an actual IPC write failure without production sockets. */
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        fd = sockets[0];
        close(sockets[1]);
        assert(!collision_ipc_send_head(&ipc, failed_video != 0U, &fd));
        assert(fd == -1 && failed->count == 1U && successful->count == 1U);
        assert(ipc.next_sequence == 2U && ipc.sent == 0U);

        test_deliver_queue_head(&ipc, failed_video == 0U, &expected);
        assert(successful->count == 0U && failed->count == 1U);
        /* Successful target is a no-op during the other target's retry. */
        assert(collision_ipc_send_head(&ipc, failed_video == 0U, &empty_fd));
        assert(empty_fd == -1 && ipc.sent == 1U);
        test_deliver_queue_head(&ipc, failed_video != 0U, &expected);
        assert(ipc.video.count == 0U && ipc.p2p.count == 0U && ipc.sent == 2U);
        assert(ipc.next_sequence == 2U);
        test_ipc_queue_deinitialize(&ipc);
    }
    assert(sigaction(SIGPIPE, &previous, NULL) == 0);
    puts("P2P/video write failures retry independently with stable identity and no successful-target replay: PASS");
}

static void test_pose_history_callback_duplicate_and_expiry(void)
{
    accident_collision_t runtime = {0};
    collision_worker_t *worker = &runtime.collision;
    collision_ipc_worker_t ipc;
    collision_config_t config = make_collision_config();
    position_publication_t snapshot = {0};
    const uint64_t now = monotonic_time_ns();
    const uint64_t capture_ns = now - UINT64_C(10000000);
    const uint64_t impact_ns = now - UINT64_C(20000000);
    const uint64_t deadline_ns = impact_ns + SENSOR_POSE_MATCH_WAIT_NS;
    const int64_t detection_time = INT64_C(1700000000456);
    uint64_t received_ns;

    assert(position_publication_initialize(&runtime.position) == 0);
    assert(pthread_mutex_init(&worker->mutex, NULL) == 0);
    worker->initialized = true;
    test_ipc_queue_initialize(&ipc);
    worker->position = &runtime.position;
    worker->ipc_worker = &ipc;
    collision_detector_initialize(&worker->detector, &config, 10000U, 10000U);
    /* Publish without a pending event first, so duplicate/expiry history is observable. */
    accident_collision_update_pose(&runtime, 250.0, 360.0, 90.0,
                                   capture_ns, now + UINT64_C(1000000000));
    assert(runtime.position.history_count == 1U && ipc.p2p.count == 0U);
    received_ns = runtime.position.history[0].received_ns;
    accident_collision_update_pose(&runtime, 999.0, 888.0, 0.0,
                                   capture_ns, now + UINT64_C(1000000000));
    assert(runtime.position.history_count == 1U);
    assert(runtime.position.history[0].received_ns == received_ns);
    assert(runtime.position.history[0].x_cm == 25.0);
    accident_collision_update_pose(&runtime, 999.0, 888.0, 0.0,
                                   now - SENSOR_POSE_MATCH_WAIT_NS, now - 1U);
    assert(!runtime.position.valid && runtime.position.history_count == 1U);
    accident_collision_invalidate_pose(&runtime);
    position_publication_snapshot(&runtime.position, &snapshot);
    assert(!snapshot.valid && snapshot.history_count == 1U);
    assert(collision_pending_record(&worker->pending, impact_ns, detection_time, &ipc));
    assert(collision_pending_try_enqueue(&worker->pending, &config, &snapshot, deadline_ns, &ipc));
    assert(ipc.p2p.count == 1U && ipc.p2p.items[0].payload.local_x_mm == 250);
    assert(ipc.p2p.items[0].payload.local_y_mm == 360);
    assert(ipc.p2p.items[0].payload.heading_value == 900U);
    collision_worker_service_pending(worker);
    assert(ipc.p2p.count == 1U);

    /* A BLE callback also services a newly pending event immediately without IMU polling. */
    {
        const uint64_t next_impact_ns = monotonic_time_ns();
        assert(collision_pending_record(&worker->pending, next_impact_ns, detection_time + 1, &ipc));
        accident_collision_update_pose(&runtime, 500.0, 600.0, 180.0,
                                       next_impact_ns, next_impact_ns + UINT64_C(1000000000));
        assert(!worker->pending.active && ipc.p2p.count == 2U);
        assert(ipc.p2p.items[1].payload.local_x_mm == 500);
        assert(ipc.p2p.items[1].payload.local_y_mm == 600);
        assert(ipc.p2p.items[1].payload.heading_value == 1800U);
        assert(ipc.p2p.items[1].monotonic_ns == next_impact_ns);
    }
    test_ipc_queue_deinitialize(&ipc);
    assert(pthread_mutex_destroy(&worker->mutex) == 0);
    position_publication_deinitialize(&runtime.position);
    puts("post-impact history survives loss/expiry; duplicates retain receipt time; BLE callback sends immediately: PASS");
}

static void test_pose_history_capacity(void)
{
    accident_collision_t runtime = {0};
    const uint64_t now = monotonic_time_ns();
    const uint64_t first_capture_ns = now - UINT64_C(1000000);
    size_t index;

    assert(position_publication_initialize(&runtime.position) == 0);
    for (index = 0U; index < SENSOR_POSE_HISTORY_CAPACITY + 10U; index++)
        accident_collision_update_pose(&runtime, (double)index, 2.0, 3.0,
                                       first_capture_ns + index, now + UINT64_C(1000000000));
    assert(runtime.position.history_count == SENSOR_POSE_HISTORY_CAPACITY);
    assert(runtime.position.history_head == 10U);
    assert(runtime.position.history[runtime.position.history_head].capture_ns == first_capture_ns + 10U);
    /* A new capture removes entries older than 2s, preserving the exact boundary. */
    runtime.position.history_head = 0U;
    runtime.position.history_count = 2U;
    runtime.position.history[0].capture_ns = first_capture_ns - SENSOR_POSE_HISTORY_NS - 1U;
    runtime.position.history[1].capture_ns = first_capture_ns - SENSOR_POSE_HISTORY_NS;
    accident_collision_update_pose(&runtime, 1.0, 2.0, 3.0,
                                   first_capture_ns, now + UINT64_C(1000000000));
    assert(runtime.position.history_count == 2U);
    assert(runtime.position.history_head == 1U);
    assert(runtime.position.history[1].capture_ns == first_capture_ns - SENSOR_POSE_HISTORY_NS);
    assert(runtime.position.history[2].capture_ns == first_capture_ns);
    position_publication_deinitialize(&runtime.position);
    puts("pose history bounded ring and 2-second capture-based pruning: PASS");
}

int main(void)
{
    bool core_passed = run_collision_core_tests();
    test_absolute_pose_payload();
    test_expiry_and_invalid_pose();
    test_pose_updates_preserve_collision_latch();
    test_stale_gap_preserves_latch_and_bno_rearms_cleanly();
    test_collision_selects_post_impact_pose_immediately_and_preserves_event();
    test_pending_post_impact_history_bounds();
    test_camera_delay_keeps_measurements_post_impact_and_receipts_in_window();
    test_pending_rejects_unusable_history_and_preserves_own_video();
    test_pending_queue_backpressure_freezes_selected_pose();
    test_own_video_backpressure_survives_pose_resolution();
    test_ipc_targets_retry_independently();
    test_pose_history_callback_duplicate_and_expiry();
    test_pose_history_capacity();
    printf("ACCIDENT ARUCO RESULT=%s (7 collision + 4 external-pose + 9 history/pending-IPC groups)\n",
           core_passed ? "PASS" : "FAIL");
    return core_passed ? 0 : 1;
}
