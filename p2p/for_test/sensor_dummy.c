/*
 * Sensor Dummy
 *
 * 실제 사고 감지 프로세스를 대신하는 테스트 프로그램이다.
 * 사용자가 s를 입력하면 기존 denm_sender.c의 시험값과 동일한
 * 원시 데이터를 IPC를 통해 broadcast_sender로 전달한다.
 */
#include "ipc.h"
#include "protocol.h"
#include "p2p_protocol.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define P2P_IPC_SOCKET_PATH "/tmp/p2p.sock"
#define RECONNECT_INTERVAL_SECONDS 1
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

static void fill_test_accident_event(
    accident_event_payload_t *payload
)
{
    memset(payload, 0, sizeof(*payload));

    payload->detection_time = get_timestamp_its_ms();

    payload->latitude = 375000000;
    payload->longitude = 1270000000;

    payload->speed_value = 320;
    payload->heading_value = 1234;

    payload->delta_latitude = 10;
    payload->delta_longitude = 20;
    payload->delta_altitude = 0;

    payload->local_x_mm = 1523;
    payload->local_y_mm = -840;
}

static int connect_to_p2p(void)
{

    while (1)
    {
        int fd = ipc_client_connect(
            P2P_IPC_SOCKET_PATH
        );

        if (fd >= 0)
        {
            printf("P2P 연결 완료\n");
            return fd;
        }

        sleep(RECONNECT_INTERVAL_SECONDS);
    }
}

static int send_accident_event(
    int *fd,
    uint32_t sequence
)
{
    ipc_header_t header;
    accident_event_payload_t payload;

    memset(&header, 0, sizeof(header));

    header.type = IPC_MSG_ACCIDENT_EVENT;
    header.flags = IPC_FLAG_NONE;
    header.sequence = sequence;
    header.timestamp = get_monotonic_ns();

    fill_test_accident_event(&payload);

    while (1)
    {
        int result = ipc_send(
            *fd,
            &header,
            &payload,
            sizeof(payload)
        );

        if (result == IPC_SUCCESS)
        {
            return IPC_SUCCESS;
        }

        ipc_close(*fd);
        *fd = connect_to_p2p();
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    uint32_t sequence = 1;
    int fd = connect_to_p2p();

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

        if (send_accident_event(&fd, sequence) != IPC_SUCCESS)
        {
            continue;
        }

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

    ipc_close(fd);
    return 0;
}
