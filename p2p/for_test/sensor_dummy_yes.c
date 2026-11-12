/*
 * Sensor Dummy
 *
 * 실제 사고 감지 프로세스를 대신하는 테스트 프로그램이다.
 * 사용자가 s를 입력하면 같은 사고 Event를 독립 IPC 연결로
 * broadcast_sender와 사고차량 video_processor에 각각 전달한다.
 */
#include "ipc.h"
#include "protocol.h"
#include "accident_protocol.h"
#include "video_processor_protocol.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define P2P_IPC_SOCKET_PATH "/tmp/p2p.sock"
#define RECONNECT_INTERVAL_SECONDS 1
#define TEST_STATION_ID 1U
#define ITS_EPOCH_UNIX_MS 1072915200000ULL
#define ITS_LEAP_SECONDS_MS 5000ULL

static uint64_t get_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
        return 0;
    }

    return
        (uint64_t)now.tv_sec * 1000000000ULL +
        (uint64_t)now.tv_nsec;
}

static uint64_t get_timestamp_its_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
    {
        return 0;
    }

    uint64_t unix_ms =
        (uint64_t)now.tv_sec * 1000ULL +
        (uint64_t)now.tv_nsec / 1000000ULL;

    if (unix_ms < ITS_EPOCH_UNIX_MS)
    {
        return 0;
    }

    return
        unix_ms
        - ITS_EPOCH_UNIX_MS
        + ITS_LEAP_SECONDS_MS;
}

static int fill_test_accident_event(
    accident_ipc_event_t *payload,
    uint32_t sequence
)
{
    uint64_t detection_time;

    if (payload == NULL || sequence == 0 || sequence > UINT16_MAX)
    {
        return -1;
    }

    memset(payload, 0, sizeof(*payload));

    /*
     * DENM detectionTime의 wire 단위는 ITS epoch millisecond다.
     * 수신 Pi가 이를 Unix epoch nanosecond로 복원해 Filter/Inference에 전달한다.
     */
    detection_time = get_timestamp_its_ms();

    if (detection_time == 0 || detection_time > INT64_MAX)
    {
        return -1;
    }

    payload->station_id = TEST_STATION_ID;
    payload->sequence_number = (uint16_t)sequence;
    payload->detection_time = (int64_t)detection_time;
    payload->heading_value = 1234;
    payload->local_x_mm = 1000;
    payload->local_y_mm = 2000;

    return 0;
}

static int connect_to_endpoint(const char *socket_path, const char *name)
{
    while (1)
    {
        int fd = ipc_client_connect(socket_path);

        if (fd >= 0)
        {
            printf("%s 연결 완료: %s\n", name, socket_path);
            return fd;
        }

        sleep(RECONNECT_INTERVAL_SECONDS);
    }
}

static int send_event_to_endpoint(
    int *fd,
    const char *socket_path,
    const char *name,
    uint16_t message_type,
    const void *payload,
    size_t payload_size,
    uint32_t sequence,
    uint64_t monotonic_ns
)
{
    ipc_header_t header;

    memset(&header, 0, sizeof(header));
    header.type = message_type;
    header.flags = IPC_FLAG_NONE;
    header.sequence = sequence;
    header.timestamp = monotonic_ns;

    while (1)
    {
        int result = ipc_send(
            *fd,
            &header,
            payload,
            payload_size
        );

        if (result == IPC_SUCCESS)
        {
            return IPC_SUCCESS;
        }

        ipc_close(*fd);
        *fd = connect_to_endpoint(socket_path, name);
    }
}

static int send_event_to_video_processor(
    const video_processor_event_t *event,
    uint32_t sequence,
    uint64_t monotonic_ns
)
{
    ipc_header_t header;

    memset(&header, 0, sizeof(header));
    header.type = IPC_MSG_VIDEO_PROCESSOR_EVENT;
    header.flags = IPC_FLAG_NONE;
    header.sequence = sequence;
    header.timestamp = monotonic_ns;

    while (1)
    {
        int fd = ipc_client_connect(VIDEO_PROCESSOR_SOCKET_PATH);

        if (fd < 0)
        {
            sleep(RECONNECT_INTERVAL_SECONDS);
            continue;
        }

        int result = ipc_send(
            fd,
            &header,
            event,
            sizeof(*event)
        );

        ipc_close(fd);

        if (result == IPC_SUCCESS)
        {
            return IPC_SUCCESS;
        }

        sleep(RECONNECT_INTERVAL_SECONDS);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    uint32_t sequence = 1;
    int p2p_fd = connect_to_endpoint(P2P_IPC_SOCKET_PATH, "P2P");

    printf(
        "s 입력 시 사고 이벤트 전송\n"
        "q 입력 시 종료\n"
    );

    while (1)
    {
        char input[32];

        printf("입력 (s/q): ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            break;
        }

        if (input[0] == 'q')
        {
            break;
        }

        if (input[0] != 's')
        {
            continue;
        }

        accident_ipc_event_t p2p_event;
        video_processor_event_t video_event;
        uint64_t monotonic_ns = get_monotonic_ns();

        if (monotonic_ns == 0 ||
            fill_test_accident_event(&p2p_event, sequence) != 0)
        {
            fprintf(stderr, "사고 이벤트 시각 생성 실패\n");
            continue;
        }

        memset(&video_event, 0, sizeof(video_event));
        video_event.station_id = p2p_event.station_id;
        video_event.sequence_number = p2p_event.sequence_number;
        video_event.detection_time = p2p_event.detection_time;

        if (send_event_to_endpoint(&p2p_fd,
                                   P2P_IPC_SOCKET_PATH,
                                   "P2P",
                                   IPC_MSG_ACCIDENT_EVENT,
                                   &p2p_event,
                                   sizeof(p2p_event),
                                   sequence,
                                   monotonic_ns) != IPC_SUCCESS)
            continue;

        if (send_event_to_video_processor(&video_event,
                                          sequence,
                                          monotonic_ns) != IPC_SUCCESS)
            continue;

        printf(
            "사고 이벤트 전송 완료 (sequence=%u)\n",
            sequence
        );

        sequence++;

        /* DENM ActionID.sequenceNumber 범위에 맞춰 시험 sequence를 순환시킨다. */
        if (sequence > 65535U)
        {
            sequence = 1;
        }
    }

    ipc_close(p2p_fd);
    return 0;
}
