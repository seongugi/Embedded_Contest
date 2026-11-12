#include "ipc.h"
#include "protocol.h"
#include "sensor_protocol.h"

#include <stdio.h>
#include <unistd.h>

#define SENSOR_SOCKET_PATH "/tmp/witness_sensor.sock"


/*
 * --------------------------------------------------
 * Main
 * --------------------------------------------------
 *
 * Witness Filter의 Sensor Thread에
 * 가상의 차량 상태를 계속 전송한다.
 *
 * 현재는 실제 BNO086 센서 대신
 * 테스트용 좌표와 Heading을 사용한다.
 */
int main(void)
{
    /*
     * Filter의 Sensor IPC 서버에 연결한다.
     */
    int fd =
        ipc_client_connect(
            SENSOR_SOCKET_PATH
        );

    if (fd < 0)
    {
        fprintf(
            stderr,
            "Sensor IPC 연결 실패\n"
        );

        return 1;
    }


    printf(
        "[MOCK SENSOR] Connected\n"
    );


    /*
     * 테스트용 차량 상태.
     *
     * 현재 차량:
     *
     *     X = 1000 mm
     *     Y = 1000 mm
     *     Heading = 0°
     *
     * Heading 0°은 현재 Filter에서 정의한
     * +Y 방향을 의미한다.
     */
    vehicle_state_t state =
    {
        .x_mm = 1000.0f,
        .y_mm = 1000.0f,
        .heading_deg = 0.0f
    };


    /*
     * 테스트용 IPC Header.
     */
    ipc_header_t header =
    {
        .type = IPC_MSG_SENSOR_DATA,
        .flags = IPC_FLAG_NONE,
        .length = 0,
        .sequence = 1,
        .timestamp = 0
    };


    /*
     * Sensor처럼 계속 최신 상태를 전송한다.
     */
    while (1)
    {
        int result =
            ipc_send(
                fd,
                &header,
                &state,
                sizeof(state)
            );

        if (result != IPC_SUCCESS)
        {
            fprintf(
                stderr,
                "[MOCK SENSOR] IPC send failed: %d\n",
                result
            );

            break;
        }


        printf(
            "[MOCK SENSOR] "
            "X=%.1f Y=%.1f Heading=%.1f\n",
            state.x_mm,
            state.y_mm,
            state.heading_deg
        );


        /*
         * 다음 메시지의 Sequence Number 증가.
         */
        header.sequence++;


        /*
         * 실제 센서가 주기적으로 데이터를 보내는 것을
         * 간단하게 흉내 낸다.
         */
        sleep(1);
    }


    ipc_close(fd);

    return 0;
}
