#ifndef ARUCO_RECEIVER_H
#define ARUCO_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

/* 관찰차와 사고차가 함께 받는 위치 사본과 callback 계약만 공개한다. */
typedef struct
{
    uint32_t sequence;
    uint64_t capture_ns;
    uint64_t received_ns;
    uint64_t expires_ns;
    int32_t x_mm;
    int32_t y_mm;
    uint16_t heading_cdeg;
    bool valid;
} aruco_pose_t;

/*
 * 두 차량 실행 파일은 이 공통 수신 loop를 사용하되 소비하는 역할은 다르다.
 * 관찰차 sample은 filter IPC, 사고차 sample은 collision 위치 갱신만 수행한다.
 * sample(NULL, now)는 검출 실패/시간 만료/연결 단절을 뜻한다. 마지막 좌표를
 * 다시 유효하게 만들거나 정지해 있다는 의미로 바꾸어 해석하면 안 된다.
 * callbacks는 GLib thread에서 실행되므로 blocking 작업은 짧게 제한해야 한다.
 */
typedef struct
{
    int (*start)(void *context);
    void (*sample)(const aruco_pose_t *pose, uint64_t now_ns, void *context);
    void (*stop)(void *context);
} aruco_receiver_callbacks_t;

enum {
    ARUCO_WATCHDOG_BLE = 0,
    ARUCO_WATCHDOG_IMU,
    ARUCO_WATCHDOG_COLLISION,
    ARUCO_WATCHDOG_IPC
};
void aruco_receiver_watchdog_progress(unsigned slot, uint64_t timeout_ms);

int aruco_receiver_main(
    int argc, char **argv, const char *role,
    const aruco_receiver_callbacks_t *callbacks, void *context
);

#endif
