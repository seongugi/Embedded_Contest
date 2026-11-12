#!/bin/bash

set -u

# Wi-Fi Direct 제어에 사용할 물리 무선 인터페이스와 고정 WPS PIN이다.
MAIN_IF="wlan0"
PIN="12345670"

# 생성된 GO 그룹 인터페이스에 설정할 고정 IP 주소다.
GO_IP="192.168.49.1/24"

# 연결을 확인할 Client IP 목록
# 테스트할 Client 수에 맞게 주소를 추가하거나 제거
CLIENT_IPS=(
    "192.168.49.2"
    "192.168.49.3"
    "192.168.49.4"
    "192.168.49.5"
)

# 현재 스크립트가 저장된 디렉터리의 절대 경로를 구한다.
SCRIPT_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &&
    pwd
)"

# 현재 스크립트 자신의 전체 경로다.
# 하위 프로세스 종료 후 스크립트를 처음부터 재실행할 때 사용한다.
SCRIPT_PATH="$SCRIPT_DIR/$(basename -- "${BASH_SOURCE[0]}")"

# GO에서 사용할 통합 브로드캐스트 송신 프로그램의 경로다.
# DENM 송신, Heartbeat 감시, BTP 수신을 한 프로세스가 담당한다.
SENDER_BIN="$SCRIPT_DIR/broadcast_sender"

# 백그라운드로 실행한 통합 Sender 프로세스의 PID를 저장한다.
SENDER_PID=""

# PIN 등록 뒤 새 Client를 기다린다. Ping에 걸린 시간도 60초 예산에 포함한다.
# 호출 성공 시 CURRENT_COUNT에 관측한 연결 수를 반환한다.
wait_for_new_client() {
    local previous_count=$1 deadline=$((SECONDS + 60)) client_ip current_count

    while (( SECONDS < deadline )); do
        current_count=0
        for client_ip in "${CLIENT_IPS[@]}"; do
            (( SECONDS < deadline )) || return 1
            if ping -I "$GO_IF" -c 1 -W 1 -w 1 "$client_ip" \
                >/dev/null 2>&1; then
                current_count=$((current_count + 1))
            fi
        done

        if (( current_count > previous_count )); then
            CURRENT_COUNT=$current_count
            return 0
        fi

        (( SECONDS < deadline )) || break
        sleep 1
    done
    return 1
}

# 이전 실행이 비정상 종료되어 남아 있을 수 있는 프로세스를 강제로 정리한다.
# 새 실행과 소켓·포트가 충돌하는 상황을 방지하기 위한 초기 정리 함수다.
cleanup_stale_processes() {

    local binary_path proc_exe process_path stale_pid
    binary_path="$(readlink -f -- "$SENDER_BIN")" || return 1

    # comm 이름은 15자로 잘리므로 /proc의 실제 실행 파일 경로를 대조한다.
    for proc_exe in /proc/[0-9]*/exe; do
        process_path="$(readlink -- "$proc_exe" 2>/dev/null)" || continue
        process_path="${process_path% (deleted)}"
        [ "$process_path" = "$binary_path" ] || continue
        stale_pid="${proc_exe#/proc/}"
        stale_pid="${stale_pid%/exe}"
        echo "[초기 정리] 기존 $SENDER_BIN 종료 (PID=$stale_pid)"
        kill -9 "$stale_pid" 2>/dev/null || true
    done

}


# 이번 스크립트가 직접 실행한 Sender를 정상 종료한다.
# PID가 실제로 살아 있는 경우에만 종료 신호를 보내고 wait로 수거한다.
cleanup_processes() {
    if [ -n "${SENDER_PID:-}" ]; then
        if kill -0 "$SENDER_PID" 2>/dev/null; then
            echo "[정리] Sender 프로세스 종료"
            kill "$SENDER_PID" 2>/dev/null || true
            wait "$SENDER_PID" 2>/dev/null || true
        fi
    fi

    SENDER_PID=""
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
echo "      Wi-Fi Direct GO 시작"
echo "========================================"

# 네트워크 인터페이스와 wpa_supplicant를 제어하려면 root 권한이 필요하다.
if [ "$EUID" -ne 0 ]; then
    echo "sudo로 실행하세요."
    echo "예: sudo ./p2p_go.sh"
    exit 1
fi


#################################################
# 이전 실행에서 남은 프로세스 정리
#################################################

cleanup_stale_processes


#################################################
# Client IP 목록 확인
#################################################

# 감시할 Client IP가 하나도 없으면 연결 완료 조건을 만들 수 없으므로 종료한다.
if [ "${#CLIENT_IPS[@]}" -eq 0 ]; then
    echo "CLIENT_IPS에 최소 1개의 Client IP를 입력하세요."
    exit 1
fi

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
# 3. 기존 GO 그룹 및 P2P 상태 정리
#################################################

# iw dev에서 type이 P2P-GO인 기존 그룹 인터페이스를 찾아 하나씩 제거한다.
while read -r OLD_IF; do
    [ -z "$OLD_IF" ] && continue

    echo "기존 GO 그룹 제거: $OLD_IF"

    wpa_cli -i "$MAIN_IF" \
        p2p_group_remove "$OLD_IF" \
        >/dev/null 2>&1

done < <(
    iw dev | awk '
        $1=="Interface" {
            iface=$2
        }

        $1=="type" && $2=="P2P-GO" {
            print iface
        }
    '
)

# 진행 중인 P2P 연결 요청과 탐색 상태도 함께 취소한다.
wpa_cli -i "$MAIN_IF" p2p_cancel \
    >/dev/null 2>&1 || true

wpa_cli -i "$MAIN_IF" p2p_stop_find \
    >/dev/null 2>&1 || true

sleep 1


#################################################
# 4. GO 그룹 생성
#################################################

# wlan0를 제어하는 wpa_supplicant에 자율 GO 그룹 생성을 요청한다.
RESULT=$(
    wpa_cli -i "$MAIN_IF" p2p_group_add 2>/dev/null
)

if ! echo "$RESULT" | grep -q "OK"; then
    echo "그룹 생성 실패"
    exit 1
fi


#################################################
# 5. GO 인터페이스 검색
#################################################

# 그룹 생성 후 생기는 P2P-GO 인터페이스 이름을 저장한다.
GO_IF=""

# 인터페이스 생성이 비동기로 진행되므로 최대 10초 동안 반복해서 찾는다.
for i in $(seq 1 10); do
    GO_IF=$(
        iw dev | awk '
            $1=="Interface" {
                iface=$2
            }

            $1=="type" && $2=="P2P-GO" {
                print iface
                exit
            }
        '
    )

    if [ -n "$GO_IF" ]; then
        break
    fi

    sleep 1
done

if [ -z "$GO_IF" ]; then
    echo "GO 인터페이스 생성 실패"
    exit 1
fi

#################################################
# 6. GO IP 설정
#################################################

# 생성된 GO 인터페이스를 활성화한 뒤 기존 IPv4 주소를 비우고 고정 IP를 설정한다.
if ! ip link set "$GO_IF" up; then
    echo "GO 인터페이스 활성화 실패"
    exit 1
fi

ip -4 addr flush dev "$GO_IF"

if ! ip addr add "$GO_IP" dev "$GO_IF"; then
    echo "GO IP 설정 실패"
    exit 1
fi

echo
echo "P2P 그룹 생성 완료"

#################################################
# 7. Client 순차 수용
#################################################

echo
echo "========================================"
echo " Client 연결 대기"
echo "========================================"

# 등록된 모든 Client IP가 Ping에 응답할 때까지 연결 수용 절차를 반복한다.
while true; do
    CONNECTED_COUNT=0

    # 현재 Ping에 응답하는 Client 수를 계산한다.
    for CLIENT_IP in "${CLIENT_IPS[@]}"; do
        if ping -c 1 -W 1 "$CLIENT_IP" \
            >/dev/null 2>&1; then

            CONNECTED_COUNT=$((CONNECTED_COUNT + 1))
        fi
    done

    if [ "$CONNECTED_COUNT" -eq "${#CLIENT_IPS[@]}" ]; then
        break
    fi

    # 아직 연결되지 않은 Client가 있으면 GO 인터페이스에 고정 PIN을 등록한다.
    # any를 사용하므로 등록된 PIN으로 접속하는 Client 한 대를 수용할 수 있다.
    RESULT=$(
        wpa_cli -i "$GO_IF" \
            wps_pin any "$PIN" \
            2>/dev/null
    )

    if echo "$RESULT" | grep -q "FAIL"; then
        echo "PIN 등록 실패"
        sleep 2
        continue
    fi

    if [ "$RESULT" != "$PIN" ]; then
        echo "예상하지 못한 PIN 등록 결과: $RESULT"
        sleep 2
        continue
    fi

    # PIN 등록 전 연결 수를 기준으로 새 Client가 추가됐는지 판단한다.
    PREVIOUS_COUNT=$CONNECTED_COUNT
    NEW_CLIENT_CONNECTED=0

    # 응답하지 않는 Client 수와 관계없이 60초 후 PIN 재등록으로 진행한다.
    if wait_for_new_client "$PREVIOUS_COUNT"; then
        echo
        echo "새로운 Client 연결"
        echo "현재 연결: $CURRENT_COUNT / ${#CLIENT_IPS[@]}"
        NEW_CLIENT_CONNECTED=1
    fi

    if [ "$NEW_CLIENT_CONNECTED" -ne 1 ]; then
        echo
        echo "연결 대기 시간 초과"
        echo "PIN을 다시 등록하고 계속 기다립니다."
    fi

    sleep 1
done


#################################################
# 8. 전체 Client 최종 확인
#################################################

echo
echo "등록된 Client 모두 연결 완료"

# 모든 Client가 연결된 뒤 각 Client와 4회 Ping으로 최종 통신 상태를 확인한다.
for CLIENT_IP in "${CLIENT_IPS[@]}"; do
    if ! ping -c 4 "$CLIENT_IP" >/dev/null 2>&1; then
        echo "Ping 테스트 실패: $CLIENT_IP"
        exit 1
    fi
done

echo "Ping 테스트 완료"


#################################################
# 9. 통합 Sender 시작
#################################################

echo
echo "========================================"
echo " Sender 시작"
echo "========================================"

# 첫 번째 인자로 DENM ACK 예상 개수를 전달하고,
# 이후 인자로 Heartbeat 감시 대상 Client IP 목록을 전달한다.
# broadcast_sender 내부에서 DENM / Heartbeat / BTP 수신을 함께 처리한다.
# 서비스에서는 같은 PID로 실행하여 watchdog 알림을 MainPID에서 보낸다.
if [ "${ACE_P2P_SYSTEMD:-0}" = 1 ]; then
    exec "$SENDER_BIN" "${#CLIENT_IPS[@]}" "${CLIENT_IPS[@]}"
fi
"$SENDER_BIN" \
    "${#CLIENT_IPS[@]}" \
    "${CLIENT_IPS[@]}" &
SENDER_PID=$!

#################################################
# 통합 Sender 상태 감시
#################################################

# 직접 실행에서는 자식을 수거한 뒤 연결 과정부터 다시 시작한다.
wait "$SENDER_PID"
SENDER_STATUS=$?
SENDER_PID=""
echo "Sender 프로세스가 종료되었습니다. (status=$SENDER_STATUS)"

echo
echo "GO 연결 과정을 처음부터 다시 시작합니다."

sleep 2

# 현재 셸 프로세스를 새 스크립트 실행으로 교체하여 처음 단계부터 다시 시작한다.
exec bash "$SCRIPT_PATH"
