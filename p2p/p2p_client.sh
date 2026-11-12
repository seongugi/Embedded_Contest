#!/bin/bash

set -u

# Wi-Fi Direct 제어에 사용할 물리 무선 인터페이스와 고정 WPS PIN이다.
MAIN_IF="wlan0"
PIN="12345670"

# 장치의 hostname에 따라 Client에 설정할 고정 IP 주소를 결정한다.
HOST_NAME=$(hostname -s)

# 각 Raspberry Pi가 서로 다른 IP를 사용하도록 hostname과 IP를 대응시킨다.
# 등록되지 않은 hostname이면 잘못된 IP 사용을 막기 위해 스크립트를 종료한다.
case "$HOST_NAME" in
    craft3)
        CLIENT_IP="192.168.49.2/24"
        ;;
    craft4)
        CLIENT_IP="192.168.49.3/24"
        ;;
    craft5|seongugi)
        CLIENT_IP="192.168.49.4/24"
        ;;
    craft6)
        CLIENT_IP="192.168.49.5/24"
        ;;
    *)
        echo "알 수 없는 hostname: $HOST_NAME"
        echo "CLIENT_IP를 결정할 수 없습니다."
        exit 1
        ;;
esac

# 연결 후 통신 상태를 확인할 GO의 고정 IP 주소다.
GO_IP="192.168.49.1"

# 연결할 실제 GO의 P2P Device MAC 주소다.
# 일반 wlan0 MAC 주소가 아니라 Wi-Fi Direct 검색에 표시되는 P2P Device 주소를 사용한다.
GO_MAC="8a:a2:9e:9f:da:8d"

# 현재 스크립트가 저장된 디렉터리의 절대 경로를 구한다.
SCRIPT_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &&
    pwd
)"

# 현재 스크립트 자신의 전체 경로다.
# 하위 프로세스 종료 후 스크립트를 처음부터 재실행할 때 사용한다.
SCRIPT_PATH="$SCRIPT_DIR/$(basename -- "${BASH_SOURCE[0]}")"

# Client에서 사용할 통합 Receiver 프로그램의 경로다.
# Heartbeat 기능은 broadcast_receiver 내부 Thread로 통합되어 있다.
RECEIVER_BIN="$SCRIPT_DIR/broadcast_receiver"

# 백그라운드로 실행한 Receiver 프로세스의 PID를 저장한다.
# 종료 처리와 실행 상태 감시에 사용한다.
RECEIVER_PID=""

# 이전 실행이 비정상 종료되어 남아 있을 수 있는 프로세스를 강제로 정리한다.
# 새 실행과 소켓·포트가 충돌하는 상황을 방지하기 위한 초기 정리 함수다.
cleanup_stale_processes() {

    local binary_path proc_exe process_path stale_pid
    binary_path="$(readlink -f -- "$RECEIVER_BIN")" || return 1

    # comm 이름은 15자로 잘리므로 /proc의 실제 실행 파일 경로를 대조한다.
    for proc_exe in /proc/[0-9]*/exe; do
        process_path="$(readlink -- "$proc_exe" 2>/dev/null)" || continue
        process_path="${process_path% (deleted)}"
        [ "$process_path" = "$binary_path" ] || continue
        stale_pid="${proc_exe#/proc/}"
        stale_pid="${stale_pid%/exe}"
        echo "[초기 정리] 기존 $RECEIVER_BIN 종료 (PID=$stale_pid)"
        kill -9 "$stale_pid" 2>/dev/null || true
    done

}


# 이번 스크립트가 직접 실행한 Receiver를 정상 종료한다.
# PID가 실제로 살아 있는 경우에만 종료 신호를 보내고 wait로 수거한다.
cleanup_processes() {
    if [ -n "${RECEIVER_PID:-}" ]; then
        if kill -0 "$RECEIVER_PID" 2>/dev/null; then
            echo "[정리] Receiver 프로세스 종료"
            kill "$RECEIVER_PID" 2>/dev/null || true
            wait "$RECEIVER_PID" 2>/dev/null || true
        fi
    fi

    RECEIVER_PID=""
}


# Ctrl+C(SIGINT) 또는 종료 신호(SIGTERM)를 받았을 때 호출된다.
# exit가 실행되면 EXIT trap에 의해 cleanup_processes도 이어서 실행된다.
handle_signal() {
    echo
    echo "종료 신호를 받았습니다."
    exit 130
}


# 스크립트가 어떤 경로로 종료되더라도 자식 프로세스를 정리한다.
trap cleanup_processes EXIT
trap handle_signal INT TERM


echo "========================================"
echo "    Wi-Fi Direct Client 시작"
echo "========================================"

# 네트워크 인터페이스와 wpa_supplicant를 제어하려면 root 권한이 필요하다.
if [ "$EUID" -ne 0 ]; then
    echo "sudo로 실행하세요."
    echo "예: sudo ./p2p_client.sh"
    exit 1
fi


#################################################
# 이전 실행에서 남은 프로세스 정리
#################################################

cleanup_stale_processes


#################################################
# 1. wlan0 활성화
#################################################

# wlan0가 내려가 있다면 Wi-Fi Direct 명령을 사용할 수 없으므로 먼저 활성화한다.
if ! ip link set "$MAIN_IF" up; then
    echo "wlan0 활성화 실패"
    exit 1
fi


#################################################
# 2. wpa_supplicant 실행
#################################################

# wpa_cli ping에 PONG이 돌아오면 이미 실행 중인 wpa_supplicant를 그대로 사용한다.
if ! wpa_cli -i "$MAIN_IF" ping 2>/dev/null |
    grep -q "PONG"; then

    echo "wpa_supplicant 수동 실행"

    if ! wpa_supplicant \
        -B \
        -i "$MAIN_IF" \
        -D nl80211 \
        -c /etc/wpa_supplicant/wpa_supplicant.conf; then

        echo "wpa_supplicant 실행 명령 실패"
        exit 1
    fi

    echo "wpa_supplicant 준비 중..."

    WPA_READY=0

    for i in $(seq 1 10); do
        if wpa_cli -i "$MAIN_IF" ping 2>/dev/null |
            grep -q "PONG"; then

            WPA_READY=1
            break
        fi

        sleep 1
    done

    if [ "$WPA_READY" -ne 1 ]; then
        echo "wpa_supplicant가 wlan0에 응답하지 않습니다."
        exit 1
    fi

    echo "wpa_supplicant 준비 완료"
fi


#################################################
# 3. 기존 Client 그룹 및 P2P 상태 정리
#################################################

# iw dev에서 type이 P2P-client인 기존 그룹 인터페이스를 찾아 하나씩 제거한다.
while read -r OLD_IF; do
    [ -z "$OLD_IF" ] && continue

    echo "기존 Client 그룹 제거: $OLD_IF"

    wpa_cli -i "$MAIN_IF" \
        p2p_group_remove "$OLD_IF" \
        >/dev/null 2>&1

done < <(
    iw dev | awk '
        $1=="Interface" {
            iface=$2
        }

        $1=="type" && $2=="P2P-client" {
            print iface
        }
    '
)

# 진행 중인 P2P 연결 요청과 탐색 상태도 함께 취소한다.
wpa_cli -i "$MAIN_IF" p2p_cancel \
    >/dev/null 2>&1 || true

wpa_cli -i "$MAIN_IF" p2p_stop_find \
    >/dev/null 2>&1 || true

# 그룹 제거가 비동기로 처리될 수 있으므로 P2P-client 인터페이스가
# iw dev에서 완전히 사라질 때까지 반복해서 제거를 요청한다.
while true; do
    STALE_IF=$(
        iw dev | awk '
            $1=="Interface" {
                iface=$2
            }

            $1=="type" && $2=="P2P-client" {
                print iface
                exit
            }
        '
    )

    if [ -z "$STALE_IF" ]; then
        break
    fi

    wpa_cli -i "$MAIN_IF" \
        p2p_group_remove "$STALE_IF" \
        >/dev/null 2>&1 || true

    sleep 1
done


#################################################
# 4. GO MAC 주소 형식 확인
#################################################

# GO_MAC이 콜론으로 구분된 6바이트 MAC 주소 형식인지 확인한다.
if ! echo "$GO_MAC" |
    grep -Eqi '^([0-9a-f]{2}:){5}[0-9a-f]{2}$'; then

    echo "GO_MAC 형식이 올바르지 않습니다."
    echo "현재 값: $GO_MAC"
    exit 1
fi

#################################################
# 5. GO 검색 및 연결 반복
#################################################

echo
echo "GO 검색"

# 연결이 실패하면 종료하지 않고 GO 검색 단계부터 다시 시도한다.
while true; do

    # 주변 Wi-Fi Direct 장치 검색을 시작한다.
    FIND_RESULT=$(
        wpa_cli -i "$MAIN_IF" p2p_find 2>/dev/null
    )

    if ! echo "$FIND_RESULT" | grep -q "OK"; then
        echo "p2p_find 실행 실패"
        echo "$FIND_RESULT"
        sleep 3
        continue
    fi

    FOUND=0

    # 최대 30초 동안 검색된 P2P Device 목록에서 지정한 GO MAC을 찾는다.
    for i in $(seq 1 30); do
        PEERS=$(
            wpa_cli -i "$MAIN_IF" p2p_peers 2>/dev/null
        )

        if echo "$PEERS" | grep -qi "$GO_MAC"; then
            FOUND=1
            break
        fi

        sleep 1
    done

    if [ "$FOUND" -ne 1 ]; then
        echo "GO를 발견하지 못했습니다."

        wpa_cli -i "$MAIN_IF" p2p_stop_find \
            >/dev/null 2>&1 || true

        sleep 3
        continue
    fi

    #################################################
    # 6. 고정 PIN으로 연결 요청
    #################################################

    # 지정한 GO에 고정 PIN을 입력하는 Client 역할로 연결을 요청한다.
    # join을 사용하므로 Client가 새 그룹을 만들지 않고 기존 GO 그룹에 참가한다.
    RESULT=$(
        wpa_cli -i "$MAIN_IF" \
            p2p_connect "$GO_MAC" "$PIN" keypad join \
            2>/dev/null
    )

    if ! echo "$RESULT" | grep -q "OK"; then
        echo "연결 요청 실패"

        wpa_cli -i "$MAIN_IF" p2p_cancel \
            >/dev/null 2>&1 || true

        sleep 3
        continue
    fi

    #################################################
    # 7. P2P-client 인터페이스 생성 대기
    #################################################

    # 연결 과정에서 새로 생성되는 P2P-client 인터페이스 이름을 저장한다.
    CLIENT_IF=""

    # 인터페이스 생성이 비동기로 진행되므로 최대 10초 동안 반복해서 찾는다.
    for i in $(seq 1 10); do
        CLIENT_IF=$(
            iw dev | awk '
                $1=="Interface" {
                    iface=$2
                }

                $1=="type" && $2=="P2P-client" {
                    print iface
                    exit
                }
            '
        )

        if [ -n "$CLIENT_IF" ]; then
            break
        fi

        sleep 1
    done

    if [ -z "$CLIENT_IF" ]; then
        echo "P2P-client 인터페이스가 생성되지 않았습니다."

        wpa_cli -i "$MAIN_IF" p2p_cancel \
            >/dev/null 2>&1 || true

        sleep 3
        continue
    fi

    #################################################
    # 8. 인증 완료 상태 확인
    #################################################

    # P2P-client 인터페이스가 생성돼도 인증이 끝난 것은 아니므로
    # wpa_state가 COMPLETED가 될 때까지 별도로 확인한다.
    COMPLETED=0

    # 최대 10초 동안 그룹 인터페이스의 연결 상태를 확인한다.
    for i in $(seq 1 10); do
        STATUS=$(
            wpa_cli -i "$CLIENT_IF" status 2>/dev/null
        )

        if echo "$STATUS" |
            grep -q "wpa_state=COMPLETED"; then

            COMPLETED=1
            break
        fi

        sleep 1
    done

    if [ "$COMPLETED" -ne 1 ]; then
        echo "인증 완료 상태를 확인하지 못했습니다."
        echo "현재 연결을 정리하고 GO 탐색부터 다시 시작합니다."

        # 인증 실패 상태가 남지 않도록 연결 요청, 검색, 그룹과 IP를 정리한다.
        wpa_cli -i "$MAIN_IF" p2p_cancel \
            >/dev/null 2>&1 || true

        wpa_cli -i "$MAIN_IF" p2p_stop_find \
            >/dev/null 2>&1 || true

        wpa_cli -i "$MAIN_IF" \
            p2p_group_remove "$CLIENT_IF" \
            >/dev/null 2>&1 || true

        ip -4 addr flush dev "$CLIENT_IF" \
            >/dev/null 2>&1 || true

        sleep 1
        continue
    fi

    echo
    echo "P2P 연결 완료"

    #################################################
    # 9. Client 고정 IP 설정
    #################################################

    # 생성된 Client 인터페이스를 활성화한 뒤 기존 IPv4 주소를 비우고
    # hostname에 따라 결정한 고정 IP를 설정한다.
    if ! ip link set "$CLIENT_IF" up; then
        echo "Client 인터페이스 활성화 실패"
        exit 1
    fi

    ip -4 addr flush dev "$CLIENT_IF"

    if ! ip addr add "$CLIENT_IP" dev "$CLIENT_IF"; then
        echo "Client IP 설정 실패"
        exit 1
    fi

    #################################################
    # 10. GO Ping 테스트
    #################################################

    # Wi-Fi Direct 인증과 IP 설정 후 실제 IP 통신이 가능한지 확인한다.
    PING_SUCCESS=0

    # 최대 10초 동안 1초 간격으로 GO의 Ping 응답을 기다린다.
    for i in $(seq 1 10); do
        if ping -c 1 -W 1 "$GO_IP" \
            >/dev/null 2>&1; then

            PING_SUCCESS=1
            break
        fi

        sleep 1
    done

    if [ "$PING_SUCCESS" -eq 1 ]; then
        echo
        echo "Ping 테스트 완료"

        echo
        echo "========================================"
        echo " Receiver 시작"
        echo "========================================"

        # 연결에 사용한 인터페이스를 Receiver의 링크 소실 감시에 전달한다.
        export ACE_P2P_INTERFACE="$CLIENT_IF"

        # Receiver 내부에서 DENM 처리, Heartbeat, UDP 수신을 Thread로 함께 수행한다.
        # Heartbeat 감시 대상인 GO IP를 실행 인자로 전달한다.
        # 서비스에서는 같은 PID로 실행하여 watchdog 알림을 MainPID에서 보낸다.
        if [ "${ACE_P2P_SYSTEMD:-0}" = 1 ]; then
            exec "$RECEIVER_BIN" "$GO_IP"
        fi
        "$RECEIVER_BIN" "$GO_IP" &
        RECEIVER_PID=$!

        # 직접 실행에서는 자식을 수거한 뒤 연결 과정부터 다시 시작한다.
        wait "$RECEIVER_PID"
        RECEIVER_STATUS=$?
        RECEIVER_PID=""
        echo "Receiver 프로세스가 종료되었습니다. (status=$RECEIVER_STATUS)"

        echo
        echo "Client 연결 과정을 처음부터 다시 시작합니다."

        sleep 2

        # 현재 셸 프로세스를 새 스크립트 실행으로 교체하여 처음 단계부터 다시 시작한다.
        exec bash "$SCRIPT_PATH"
    fi


    #################################################
    # 실패 시 그룹 제거 후 재시도
    #################################################

    echo "Ping 실패"
    echo "현재 연결을 정리하고 다시 시도합니다."

    # Ping 실패 시 현재 그룹과 Client 인터페이스의 IP를 정리한 뒤
    # 외부 반복문의 처음으로 돌아가 GO 검색부터 다시 시도한다.
    wpa_cli -i "$MAIN_IF" \
        p2p_group_remove "$CLIENT_IF" \
        >/dev/null 2>&1 || true

    ip -4 addr flush dev "$CLIENT_IF" \
        >/dev/null 2>&1 || true

    sleep 3
done
