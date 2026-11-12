#define _POSIX_C_SOURCE 200809L

/*
 * 고정 배치 관찰차: BLE에서 검증한 자신의 X/Y/카메라 방향을 filter에 전달한다.
 * BNO/PAA 장치는 열지 않는다. 좌표계는 기존 계약대로 mm, 0도=+Y, 90도=+X다.
 * PC가 차체 heading을 보내면 카메라 장착 방향 보정은 PC에서 먼저 적용한다.
 * 최초 정상 측정 이후에는 마커 소실/BLE 단절 중에도 고정 배치 위치를 유지한다.
 * 원래 측정 시각을 바꾸지 않으며, 새 정상 측정이 오면 갱신한다.
 */
#include "aruco_receiver.h"
#include "ipc.h"
#include "sensor_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WITNESS_ARUCO_SOCKET "/tmp/witness_sensor.sock"
#define WITNESS_RECONNECT_NS UINT64_C(500000000)
#define WITNESS_PUBLISH_NS UINT64_C(50000000)

typedef struct
{
    int ipc_fd;
    uint64_t next_connect_ns;
    uint64_t next_publish_ns;
    uint32_t next_sequence;
    bool have_pose;
    bool holding;
    aruco_pose_t saved_pose;
} witness_aruco_t;

static int witness_open_socket(const char *path)
{
    struct sockaddr_un address;
    int fd;
    if (strlen(path) >= sizeof(address.sun_path)) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1U);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int witness_start(void *context)
{
    witness_aruco_t *state = context;
    memset(state, 0, sizeof(*state));
    state->ipc_fd = -1;
    state->next_sequence = 1U;
    return 0;
}

static void witness_drop_connection(witness_aruco_t *state)
{
    if (state->ipc_fd >= 0)
    {
        ipc_close(state->ipc_fd);
        state->ipc_fd = -1;
    }
}

static void witness_sample(const aruco_pose_t *pose, uint64_t now_ns, void *context)
{
    witness_aruco_t *state = context;
    vehicle_state_t vehicle;
    ipc_header_t header;
    bool fresh;
    bool first_pose = !state->have_pose;
    bool holding;

    if (now_ns == 0U) return;
    fresh = pose != NULL && pose->valid && pose->capture_ns != 0U
        && pose->capture_ns <= now_ns && now_ns < pose->expires_ns
        && now_ns >= pose->received_ns && pose->capture_ns <= (uint64_t)INT64_MAX
        && pose->heading_cdeg < 36000U
        && pose->x_mm >= -1000000 && pose->x_mm <= 1000000
        && pose->y_mm >= -1000000 && pose->y_mm <= 1000000;
    if (fresh && (!state->have_pose || pose->capture_ns > state->saved_pose.capture_ns))
    {
        /* rate gate 안에서도 최신 측정은 먼저 저장해 다음 송신에 사용한다. */
        state->saved_pose = *pose;
        state->have_pose = true;
    }
    if (!state->have_pose)
    {
        /* 최초 정상 측정 전에는 원점/방향 0을 관측차 상태로 만들어 보내지 않는다. */
        witness_drop_connection(state);
        return;
    }
    holding = !fresh || pose->capture_ns != state->saved_pose.capture_ns;
    if (first_pose)
    {
        fprintf(stderr, "[WITNESS] 고정 관측차의 최초 위치/카메라 방향을 확보했습니다.\n");
    }
    else if (holding != state->holding)
    {
        fprintf(stderr, "%s\n", holding
            ? "[WITNESS] 새 측정 없음. 마지막 정상 위치/카메라 방향을 고정값으로 유지합니다."
            : "[WITNESS] ArUco 정상 측정을 다시 수신합니다.");
    }
    state->holding = holding;
    if (now_ns < state->next_publish_ns)
    {
        return;
    }
    if (state->ipc_fd < 0)
    {
        if (now_ns < state->next_connect_ns) return;
        state->next_connect_ns = now_ns + WITNESS_RECONNECT_NS;
        state->ipc_fd = witness_open_socket(WITNESS_ARUCO_SOCKET);
        if (state->ipc_fd < 0) return;
    }
    memset(&vehicle, 0, sizeof(vehicle));
    /* 고정 위치를 재전송해도 새로 촬영한 것처럼 시각을 덮어쓰지 않는다. */
    vehicle.detection_time = (int64_t)state->saved_pose.capture_ns;
    vehicle.x_mm = (float)state->saved_pose.x_mm;
    vehicle.y_mm = (float)state->saved_pose.y_mm;
    vehicle.heading_deg = (float)state->saved_pose.heading_cdeg / 100.0f;
    memset(&header, 0, sizeof(header));
    header.type = IPC_MSG_SENSOR_DATA;
    header.sequence = state->next_sequence;
    header.timestamp = state->saved_pose.capture_ns;

    /*
     * AF_UNIX socket을 nonblocking으로 둔다. filter가 읽지 않아 send가 일부만
     * 끝나면 연결을 닫아 불완전 frame을 폐기한다. 느린 IPC 때문에 BLE 수신을
     * 막거나 과거 위치의 이력을 queue에 쌓아두지 않는다. 실패해도 고정 위치는 유지한다.
     */
    if (ipc_send(state->ipc_fd, &header, &vehicle, sizeof(vehicle)) != IPC_SUCCESS)
    {
        witness_drop_connection(state);
        return;
    }
    state->next_publish_ns = now_ns + WITNESS_PUBLISH_NS;
    state->next_sequence = state->next_sequence == UINT32_MAX
        ? 1U : state->next_sequence + 1U;
}

static void witness_stop(void *context)
{
    witness_aruco_t *state = context;
    witness_drop_connection(state);
    state->have_pose = false;
    state->holding = false;
    memset(&state->saved_pose, 0, sizeof(state->saved_pose));
}

int main(int argc, char **argv)
{
    witness_aruco_t state;
    const aruco_receiver_callbacks_t callbacks = {
        .start = witness_start,
        .sample = witness_sample,
        .stop = witness_stop
    };
    return aruco_receiver_main(argc, argv, "witness", &callbacks, &state);
}
