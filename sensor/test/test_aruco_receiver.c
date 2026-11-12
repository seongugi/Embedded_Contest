/* 실제 수신/패킷 코드와 관찰차 IPC를 시험하며 D-Bus는 private bus를 사용한다. */
#include "../aruco_receiver.c"
#define main witness_aruco_program_main
#include "../witness_aruco.c"
#undef main

#include <assert.h>

static void put_le(uint8_t *data, uint64_t value, size_t count)
{
    for (size_t index = 0U; index < count; ++index)
    {
        data[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void packet(uint8_t bytes[ARUCO_POSE_WIRE_SIZE], uint32_t sequence, uint64_t capture)
{
    memset(bytes, 0, ARUCO_POSE_WIRE_SIZE);
    memcpy(bytes, "ACP1", 4U);
    put_le(bytes + 4, 3U, 2U);
    put_le(bytes + 6, ARUCO_FLAG_VALID, 2U);
    put_le(bytes + 8, 123U, 8U);
    put_le(bytes + 16, sequence, 4U);
    put_le(bytes + 20, capture, 8U);
    put_le(bytes + 28, (uint32_t)-1234, 4U);
    put_le(bytes + 32, 5678U, 4U);
    put_le(bytes + 36, 12345U, 2U);
}

static void test_packet_decode(void)
{
    aruco_pose_store_t store;
    aruco_pose_t pose;
    uint8_t bytes[ARUCO_POSE_WIRE_SIZE];
    uint8_t clock[ARUCO_CLOCK_WIRE_SIZE];
    const uint64_t capture = UINT64_C(2000000000);
    aruco_pose_store_init(&store, 3U, 123U, UINT64_C(250000000));
    packet(bytes, 1U, capture);
    assert(aruco_pose_store_receive(&store, bytes, sizeof(bytes), capture + 10U) == ARUCO_ACCEPTED);
    assert(aruco_pose_store_snapshot(&store, capture + 100U, &pose));
    assert(pose.x_mm == -1234 && pose.y_mm == 5678 && pose.heading_cdeg == 12345U);
    assert(pose.capture_ns == capture && pose.received_ns == capture + 10U);
    assert(pose.expires_ns == capture + UINT64_C(250000000));
    assert(!aruco_pose_store_snapshot(&store, pose.expires_ns, &pose));
    assert(!aruco_pose_store_snapshot(&store, capture, &pose));
    aruco_clock_encode(clock, UINT64_C(0x1122334455667788), UINT64_C(0x99aabbccddeeff00));
    assert(memcmp(clock, "ACT1", 4U) == 0);
    assert(clock[4] == 0x88 && clock[11] == 0x11 && clock[12] == 0x00 && clock[19] == 0x99);
    puts("wire endian/signed axes/clock + capture-based expiry: PASS");
}

static void test_frame_rejection(void)
{
    aruco_pose_store_t store;
    uint8_t bytes[ARUCO_POSE_WIRE_SIZE + 1U];
    const uint64_t now = UINT64_C(2000000000);
    aruco_pose_store_init(&store, 3U, 123U, UINT64_C(250000000));
    packet(bytes, 1U, now - 1000U);
    assert(aruco_pose_store_receive(&store, bytes, 39U, now) == ARUCO_BAD_FRAME);
    assert(aruco_pose_store_receive(&store, bytes, 41U, now) == ARUCO_BAD_FRAME);
    assert(aruco_pose_store_receive(&store, NULL, 40U, now) == ARUCO_BAD_FRAME);
    bytes[0] = 'X';
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_FRAME);
    packet(bytes, 1U, now - 1000U);
    bytes[38] = 1U;
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_FRAME);
    bytes[38] = 0U; bytes[6] = 2U;
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_FRAME);
    bytes[6] = 1U; bytes[4] = 4U;
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_WRONG_VEHICLE);
    bytes[4] = 3U; bytes[8] = 124U;
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_WRONG_EPOCH);
    bytes[8] = 123U; put_le(bytes + 36, 36000U, 2U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_FRAME);
    packet(bytes, 1U, now - 1000U);
    put_le(bytes + 28, (uint32_t)INT32_MIN, 4U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_FRAME);
    packet(bytes, 1U, now + 1U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_TIME);
    packet(bytes, 1U, 0U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_BAD_TIME);
    assert(!store.have_sequence && !store.latest.valid);
    puts("malformed/id/epoch/range/future timestamp rejection: PASS");
}

static void test_sequence_and_loss(void)
{
    aruco_pose_store_t store;
    aruco_pose_t pose;
    uint8_t bytes[ARUCO_POSE_WIRE_SIZE];
    const uint64_t now = UINT64_C(2000000000);
    aruco_pose_store_init(&store, 3U, 123U, UINT64_C(250000000));
    packet(bytes, UINT32_MAX, now - 100U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_ACCEPTED);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now + 100U) == ARUCO_OUT_OF_ORDER);
    assert(store.latest.received_ns == now);
    packet(bytes, 0U, now - 99U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_ACCEPTED);
    packet(bytes, UINT32_MAX, now - 98U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_OUT_OF_ORDER);
    packet(bytes, 1U, now - 99U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_OUT_OF_ORDER);
    packet(bytes, 1U, now - 98U); bytes[6] = 0U;
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_INVALIDATED);
    assert(!aruco_pose_store_snapshot(&store, now, &pose));
    packet(bytes, 2U, now - 97U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_ACCEPTED);
    packet(bytes, 3U, now - 96U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now + UINT64_C(300000000)) == ARUCO_EXPIRED);
    assert(!store.latest.valid);
    aruco_pose_store_reset(&store, 456U);
    assert(!store.have_sequence && !store.latest.valid);
    packet(bytes, 0U, now);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_WRONG_EPOCH);
    put_le(bytes + 8, 456U, 8U);
    assert(aruco_pose_store_receive(&store, bytes, 40U, now) == ARUCO_ACCEPTED);
    puts("sequence wrap/duplicates/reorder/frozen frame/loss/new session: PASS");
}

static aruco_pose_t witness_pose(uint64_t capture_ns, uint64_t now_ns)
{
    aruco_pose_t pose = {0};

    pose.valid = true;
    pose.capture_ns = capture_ns;
    pose.received_ns = now_ns;
    pose.expires_ns = capture_ns + UINT64_C(250000000);
    pose.x_mm = -1234;
    pose.y_mm = 5678;
    pose.heading_cdeg = 12345U;
    return pose;
}

static void witness_test_socket(witness_aruco_t *witness, int pair[2])
{
    /* Nonblocking reads make missing output fail immediately rather than hang. */
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    witness->ipc_fd = pair[0];
}

static void assert_no_witness_frame(int fd)
{
    char probe;

    errno = 0;
    assert(recv(fd, &probe, 1U, MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void assert_no_initial_witness_payload(int fd)
{
    char probe;
    ssize_t received;

    errno = 0;
    received = recv(fd, &probe, 1U, MSG_DONTWAIT);
    assert(received == 0 || (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));
}

static void assert_witness_frame(int fd, uint32_t sequence, const aruco_pose_t *pose)
{
    ipc_header_t header;
    vehicle_state_t vehicle;

    assert(ipc_recv(fd, &header, &vehicle, sizeof(vehicle)) == IPC_SUCCESS);
    assert(header.type == IPC_MSG_SENSOR_DATA && header.sequence == sequence);
    assert(header.length == sizeof(vehicle));
    assert(header.timestamp == pose->capture_ns);
    assert(vehicle.detection_time == (int64_t)pose->capture_ns);
    assert(vehicle.x_mm == (float)pose->x_mm);
    assert(vehicle.y_mm == (float)pose->y_mm);
    assert(vehicle.heading_deg == (float)pose->heading_cdeg / 100.0f);
}

static void test_witness_waits_for_initial_valid_pose(void)
{
    witness_aruco_t witness;
    aruco_pose_t pose;
    int pair[2];
    const uint64_t now = UINT64_C(2000000000);
    size_t scenario;

    assert(witness_start(&witness) == 0);
    witness_test_socket(&witness, pair);
    witness_sample(NULL, now, &witness);
    assert(!witness.have_pose && witness.next_sequence == 1U);
    assert_no_initial_witness_payload(pair[1]);
    for (scenario = 0U; scenario < 10U; scenario++)
    {
        pose = witness_pose(now - 1000U, now);
        switch (scenario)
        {
        case 0U: pose.valid = false; break;
        case 1U: pose.capture_ns = 0U; break;
        case 2U: pose.capture_ns = now + 1U; break;
        case 3U: pose.received_ns = now + 1U; break;
        case 4U: pose.expires_ns = now; break;
        case 5U: pose.expires_ns = now - 1U; break;
        case 6U:
            pose.capture_ns = (uint64_t)INT64_MAX + 1U;
            pose.expires_ns = UINT64_MAX;
            break;
        case 7U: pose.x_mm = 1000001; break;
        case 8U: pose.y_mm = -1000001; break;
        case 9U: pose.heading_cdeg = 36000U; break;
        default: assert(false);
        }
        witness_sample(&pose, now, &witness);
        assert(!witness.have_pose && witness.next_sequence == 1U);
        assert_no_initial_witness_payload(pair[1]);
    }
    /* Separate signed-timestamp limit from the future-capture rejection. */
    pose = witness_pose(now - 1000U, now);
    pose.capture_ns = (uint64_t)INT64_MAX + 1U;
    pose.expires_ns = UINT64_MAX;
    witness_sample(&pose, pose.capture_ns + 1U, &witness);
    assert(!witness.have_pose && witness.next_sequence == 1U);
    assert_no_initial_witness_payload(pair[1]);
    close(pair[1]);
    witness_stop(&witness);
    puts("witness waits for first fresh valid pose; invalid/time-bound inputs never invent zeros: PASS");
}

static void test_witness_periodic_hold_and_reacquisition(void)
{
    witness_aruco_t witness;
    aruco_pose_t saved;
    aruco_pose_t incoming;
    int pair[2];
    const uint64_t now = UINT64_C(2000000000);
    const uint64_t late = now + UINT64_C(30000000000);

    assert(witness_start(&witness) == 0);
    witness_test_socket(&witness, pair);
    saved = witness_pose(now - 1000U, now);
    witness_sample(&saved, now, &witness);
    assert(witness.have_pose && !witness.holding);
    assert_witness_frame(pair[1], 1U, &saved);

    witness_sample(&saved, now + WITNESS_PUBLISH_NS - 1U, &witness);
    assert_no_witness_frame(pair[1]);
    assert(witness.next_sequence == 2U);
    witness_sample(&saved, now + WITNESS_PUBLISH_NS, &witness);
    assert_witness_frame(pair[1], 2U, &saved);
    assert(!witness.holding);

    incoming = witness_pose(now + WITNESS_PUBLISH_NS, now + 2U * WITNESS_PUBLISH_NS);
    incoming.valid = false;
    incoming.x_mm = 0;
    incoming.y_mm = 0;
    incoming.heading_cdeg = 0U;
    witness_sample(&incoming, now + 2U * WITNESS_PUBLISH_NS, &witness);
    assert(witness.holding && witness.have_pose && witness.ipc_fd == pair[0]);
    assert_witness_frame(pair[1], 3U, &saved);

    witness_sample(&saved, saved.expires_ns, &witness);
    assert(witness.holding);
    assert_witness_frame(pair[1], 4U, &saved);
    witness_sample(NULL, late, &witness);
    assert(witness.holding && witness.have_pose && witness.ipc_fd == pair[0]);
    assert_witness_frame(pair[1], 5U, &saved);

    incoming = witness_pose(late + WITNESS_PUBLISH_NS - 1U, late + WITNESS_PUBLISH_NS);
    incoming.x_mm = 900;
    incoming.y_mm = -800;
    incoming.heading_cdeg = 9000U;
    saved = incoming;
    witness_sample(&incoming, late + WITNESS_PUBLISH_NS, &witness);
    assert(!witness.holding && witness.have_pose);
    assert_witness_frame(pair[1], 6U, &saved);

    /* A delayed but still fresh older frame must not roll the cached pose back. */
    incoming.capture_ns = saved.capture_ns - 1U;
    incoming.x_mm = -777;
    incoming.y_mm = 888;
    incoming.heading_cdeg = 18000U;
    witness_sample(&incoming, late + 2U * WITNESS_PUBLISH_NS, &witness);
    assert(witness.holding);
    assert_witness_frame(pair[1], 7U, &saved);

    /* Equal capture confirms freshness but cannot overwrite the saved measurement. */
    incoming.capture_ns = saved.capture_ns;
    witness_sample(&incoming, late + 3U * WITNESS_PUBLISH_NS, &witness);
    assert(!witness.holding);
    assert_witness_frame(pair[1], 8U, &saved);
    close(pair[1]);
    witness_stop(&witness);
    puts("witness same-frame periodic IPC, 30-second hold, original clocks and fresh-only reacquisition: PASS");
}

static void test_witness_caches_new_pose_before_publish_gate(void)
{
    witness_aruco_t witness;
    aruco_pose_t pose;
    int pair[2];
    const uint64_t now = UINT64_C(2000000000);

    assert(witness_start(&witness) == 0);
    witness_test_socket(&witness, pair);
    pose = witness_pose(now - 1000U, now);
    witness_sample(&pose, now, &witness);
    assert_witness_frame(pair[1], 1U, &pose);

    pose = witness_pose(now + UINT64_C(10000000), now + UINT64_C(10000000));
    pose.x_mm = 10;
    pose.y_mm = 20;
    pose.heading_cdeg = 3000U;
    witness_sample(&pose, now + UINT64_C(10000000), &witness);
    assert_no_witness_frame(pair[1]);
    assert(witness.saved_pose.capture_ns == pose.capture_ns);
    assert(witness.saved_pose.x_mm == pose.x_mm && witness.next_sequence == 2U);

    /* The marker is already missing when the 50 ms send period comes due. */
    witness_sample(NULL, now + WITNESS_PUBLISH_NS, &witness);
    assert(witness.holding);
    assert_witness_frame(pair[1], 2U, &pose);
    witness_sample(NULL, now + WITNESS_PUBLISH_NS, &witness);
    assert_no_witness_frame(pair[1]);
    assert(witness.next_sequence == 3U);
    close(pair[1]);
    witness_stop(&witness);
    puts("fresh camera frame cached inside 50 ms rate gate survives immediate marker loss: PASS");
}

static void test_witness_connection_failure_and_restart(void)
{
    witness_aruco_t witness;
    aruco_pose_t pose;
    int pair[2];
    int restarted_pair[2];
    char probe;
    void (*previous_sigpipe)(int);
    const uint64_t now = UINT64_C(2000000000);
    const uint64_t failed_at = now + WITNESS_PUBLISH_NS;

    assert(witness_start(&witness) == 0);
    witness_test_socket(&witness, pair);
    pose = witness_pose(now - 1000U, now);
    witness_sample(&pose, now, &witness);
    assert_witness_frame(pair[1], 1U, &pose);
    close(pair[1]);

    /* Match receiver_main's SIGPIPE policy; no production socket endpoint is opened. */
    previous_sigpipe = signal(SIGPIPE, SIG_IGN);
    assert(previous_sigpipe != SIG_ERR);
    witness_sample(NULL, failed_at, &witness);
    assert(signal(SIGPIPE, previous_sigpipe) != SIG_ERR);
    assert(witness.ipc_fd == -1 && witness.have_pose && witness.holding);
    assert(witness.next_sequence == 2U);
    assert(witness.saved_pose.capture_ns == pose.capture_ns);
    assert(witness.saved_pose.x_mm == pose.x_mm);

    /* Verify a deferred reconnect tick without calling the production connector. */
    witness.next_connect_ns = failed_at + WITNESS_RECONNECT_NS;
    witness_sample(NULL, failed_at + 1U, &witness);
    assert(witness.ipc_fd == -1 && witness.have_pose && witness.next_sequence == 2U);

    witness_test_socket(&witness, pair);
    witness_sample(NULL, failed_at + WITNESS_RECONNECT_NS, &witness);
    assert_witness_frame(pair[1], 2U, &pose);
    assert(witness.next_sequence == 3U && witness.have_pose && witness.holding);
    witness_stop(&witness);
    assert(witness.ipc_fd == -1 && !witness.have_pose && !witness.holding);
    assert(witness.saved_pose.capture_ns == 0U);
    assert(recv(pair[1], &probe, 1U, MSG_DONTWAIT) == 0);
    close(pair[1]);

    assert(witness_start(&witness) == 0);
    assert(!witness.have_pose && !witness.holding && witness.next_sequence == 1U);
    assert(witness.next_connect_ns == 0U && witness.next_publish_ns == 0U);
    witness_test_socket(&witness, restarted_pair);
    witness_sample(NULL, failed_at + 2U * WITNESS_RECONNECT_NS, &witness);
    assert_no_initial_witness_payload(restarted_pair[1]);
    assert(!witness.have_pose && witness.next_sequence == 1U);
    close(restarted_pair[1]);
    witness_stop(&witness);
    puts("IPC failure preserves cached pose/sequence; injected reconnect resends; stop/start clears cache: PASS");
}

typedef struct
{
    GTestDBus *bus;
    unsigned int samples;
    unsigned int stops;
    bool bus_stopped;
    bool timed_out;
} receiver_bus_test_t;

static void bus_test_sample(const aruco_pose_t *pose, uint64_t now_ns, void *context)
{
    receiver_bus_test_t *test = context;
    assert(pose == NULL && now_ns != 0U);
    ++test->samples;
}

static void bus_test_stop(void *context)
{
    receiver_bus_test_t *test = context;
    ++test->stops;
}

static gboolean stop_private_bus(gpointer context)
{
    receiver_bus_test_t *test = context;
    test->bus_stopped = true;
    g_test_dbus_stop(test->bus);
    return G_SOURCE_REMOVE;
}

static gboolean bus_test_timeout(gpointer context)
{
    receiver_bus_test_t *test = context;
    test->timed_out = true;
    raise(SIGTERM);
    return G_SOURCE_REMOVE;
}

static void test_receiver_exits_after_system_bus_loss(void)
{
    receiver_bus_test_t test = {0};
    const aruco_receiver_callbacks_t callbacks = {
        .sample = bus_test_sample,
        .stop = bus_test_stop
    };
    char *args[] = {"test_receiver", "--vehicle-id", "3", NULL};
    char *saved_address = g_strdup(g_getenv("DBUS_SYSTEM_BUS_ADDRESS"));
    guint timeout_id;
    int result;

    test.bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test.bus);
    assert(g_setenv("DBUS_SYSTEM_BUS_ADDRESS",
                   g_test_dbus_get_bus_address(test.bus), TRUE));
    g_timeout_add(100U, stop_private_bus, &test);
    timeout_id = g_timeout_add(3000U, bus_test_timeout, &test);
    result = aruco_receiver_main(3, args, "witness", &callbacks, &test);
    if (!test.timed_out)
        g_source_remove(timeout_id);
    assert(test.bus_stopped && !test.timed_out);
    assert(result == EXIT_FAILURE && test.samples > 0U && test.stops == 1U);
    g_test_dbus_down(test.bus);
    g_object_unref(test.bus);
    if (saved_address != NULL)
        assert(g_setenv("DBUS_SYSTEM_BUS_ADDRESS", saved_address, TRUE));
    else
        g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
    g_free(saved_address);
    puts("system bus loss exits receiver with failure and runs consumer cleanup: PASS");
}

static void expiry_sample(const aruco_pose_t *pose, uint64_t now_ns, void *context)
{
    unsigned int *count = context;
    (void)pose;
    assert(now_ns != 0U);
    ++*count;
}

static void test_expired_write_keeps_session_and_next_pose(void)
{
    receiver_runtime_t runtime = {0};
    const aruco_receiver_callbacks_t callbacks = {.sample = expiry_sample};
    uint8_t bytes[ARUCO_POSE_WIRE_SIZE];
    aruco_pose_t pose;
    GError *error = NULL;
    unsigned int count = 0U;
    const uint64_t limit = (uint64_t)ARUCO_DEFAULT_STALE_MS * UINT64_C(1000000);
    uint64_t now = receiver_now_ns();

    assert(ARUCO_DEFAULT_STALE_MS == 300U);
    runtime.callbacks = &callbacks;
    runtime.consumer = &count;
    aruco_pose_store_init(&runtime.store, 3U, 123U, limit);

    packet(bytes, 1U, now - limit + 1U);
    assert(aruco_pose_store_receive(&runtime.store, bytes, sizeof(bytes), now) == ARUCO_ACCEPTED);
    assert(aruco_pose_store_snapshot(&runtime.store, now, &pose));
    packet(bytes, 2U, now - limit + 2U);
    assert(aruco_pose_store_receive(&runtime.store, bytes, sizeof(bytes), now + 2U) == ARUCO_EXPIRED);
    assert(!aruco_pose_store_snapshot(&runtime.store, now + 2U, &pose));

    now = receiver_now_ns();
    packet(bytes, 3U, now - limit + 3U);
    assert(receiver_write(NULL, bytes, sizeof(bytes), 0, &runtime, &error));
    assert(error == NULL && !runtime.store.latest.valid);
    assert(runtime.store.epoch == 123U && runtime.store.latest.sequence == 3U);

    now = receiver_now_ns();
    packet(bytes, 4U, now - UINT64_C(50000000));
    assert(receiver_write(NULL, bytes, sizeof(bytes), 0, &runtime, &error));
    assert(error == NULL && runtime.store.latest.sequence == 4U);
    assert(aruco_pose_store_snapshot(&runtime.store, receiver_now_ns(), &pose));
    assert(pose.capture_ns == now - UINT64_C(50000000) && count == 2U);
    assert(runtime.store.epoch == 123U && runtime.store.rejected == 2U);

    /* Duplicate frames are discarded without breaking the live session. */
    assert(receiver_write(NULL, bytes, sizeof(bytes), 0, &runtime, &error));
    assert(error == NULL && runtime.store.latest.sequence == 4U);
    now = receiver_now_ns();
    packet(bytes, 5U, now - UINT64_C(50000000));
    assert(receiver_write(NULL, bytes, sizeof(bytes), 0, &runtime, &error));
    assert(error == NULL && runtime.store.latest.sequence == 5U);
    /* Malformed packets retain the existing failure response. */
    bytes[0] = 'X';
    assert(!receiver_write(NULL, bytes, sizeof(bytes), 0, &runtime, &error));
    assert(error != NULL);
    g_clear_error(&error);
    puts("300ms expiry returns success without a session reset; next 50ms-old pose replaces it: PASS");
}

int main(void)
{
    test_expired_write_keeps_session_and_next_pose();
    test_packet_decode();
    test_frame_rejection();
    test_sequence_and_loss();
    test_witness_waits_for_initial_valid_pose();
    test_witness_periodic_hold_and_reacquisition();
    test_witness_caches_new_pose_before_publish_gate();
    test_witness_connection_failure_and_restart();
    test_receiver_exits_after_system_bus_loss();
    puts("ARUCO protocol + witness IPC: PASS");
    return 0;
}
