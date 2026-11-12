#!/usr/bin/env bash

set -euo pipefail

ORT_ROOT=/opt/onnxruntime
ORT_VERSION=1.29.0
# Microsoft 공식 release asset을 내려받아 계산한 SHA-256.
ORT_SHA256=e1799098ebc054b370f6176a450f158720f297818c613e5dc99b92e2ec82346f
setup_tmp=""
ort_stage=""
apt_updated=0

cleanup_setup() {
    if [[ "$setup_tmp" == /tmp/ace-target-setup.* ]]; then
        rm -rf -- "$setup_tmp"
    fi
    if [[ "$ort_stage" == "${ORT_ROOT%/*}"/.ace-onnxruntime.* ]]; then
        rm -rf -- "$ort_stage"
    fi
}

update_package_index() {
    if [ "$apt_updated" -eq 0 ]; then
        apt-get update
        apt_updated=1
    fi
}

install_missing_packages() {
    local package
    local missing=()
    for package in "$@"; do
        if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null |
            grep -q '^install ok installed$'; then
            missing+=("$package")
        fi
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        update_package_index
        apt-get install -y "${missing[@]}"
    fi
}

remove_legacy_uploader_profile() {
    local legacy=/etc/profile.d/uploader.sh
    local digest
    if [ ! -e "$legacy" ] && [ ! -L "$legacy" ]; then
        return
    fi
    # 이전 setup_target.sh가 쓴 root:600 일반 파일과 정확한 내용만 정리한다.
    if [ -L "$legacy" ] || [ ! -f "$legacy" ] ||
        [ "$(readlink -f -- "$legacy")" != "$legacy" ] ||
        [ "$(stat -c '%u:%a' -- "$legacy")" != '0:600' ]; then
        echo "기존 $legacy 보존: 이 설치 스크립트의 생성 파일로 확인되지 않음."
        return
    fi
    digest="$(sha256sum -- "$legacy")"
    digest="${digest%% *}"
    case "$digest" in
        71b04c86907fd1e1f51d5dfd6f156fb39ab16a8faee5402ab85c3f821a9ab314|\
        272d4aea7204952f2f6d63254aca4ee01d72e547486d11d36af883cab3fad604|\
        d3ab4d546eb1195bdbc6fecc80cf978d5970da5663d213a43e8d18903c21ad99|\
        68f890e7c6872dad488cb2199386cff5db6a2abc7f0bdcfbfffda8bcb1c44178|\
        0d854fadca5aa21374d4aafe19dd68cf8ace56e6f701e1ff21b34f7663ffc67a)
            rm -- "$legacy"
            echo "이전 설치에서 만든 Uploader profile 정리 완료."
            ;;
        *) echo "기존 $legacy 보존: 사용자 수정 또는 다른 내용이 있음." ;;
    esac
}

pair_and_trust_pc() {
    local pc=88:F4:DA:74:9E:14
    local device=/org/bluez/hci0/dev_88_F4_DA_74_9E_14
    local paired trusted
    paired="$(busctl --system --timeout=5 get-property org.bluez "$device" \
        org.bluez.Device1 Paired 2>/dev/null || true)"
    if [ "$paired" != 'b true' ]; then
        # bluetoothctl의 기본 컨트롤러가 다른 어댑터를 가리키지 않도록 확인한다.
        local controllers=(/sys/class/bluetooth/hci[0-9]*)
        if [ "${#controllers[@]}" -ne 1 ] ||
            [ "${controllers[0]}" != /sys/class/bluetooth/hci0 ] ||
            [ ! -d /sys/class/bluetooth/hci0 ]; then
            echo "PC 자동 페어링 보류: hci0 단일 Bluetooth 컨트롤러가 필요합니다." >&2
            echo "컨트롤러 설정을 확인한 뒤 setup_target.sh를 다시 실행하세요." >&2
            return 1
        fi
        if ! busctl --system --timeout=5 set-property org.bluez /org/bluez/hci0 \
            org.bluez.Adapter1 Powered b true; then
            echo "PC 자동 페어링 실패: hci0 전원을 켜지 못했습니다. 설치 후 다시 실행하세요." >&2
            return 1
        fi
        echo "Windows의 Bluetooth 설정 화면을 열어 두세요. PC $pc를 15초간 검색합니다."
        # 발견 세션은 이 CLI 종료 시 해제된다. Pairable/Discoverable은 변경하지 않는다.
        timeout --kill-after=5s 20s bluetoothctl --timeout=15 scan on || true
        paired="$(busctl --system --timeout=5 get-property org.bluez "$device" \
            org.bluez.Device1 Paired 2>/dev/null || true)"
        if [ -z "$paired" ]; then
            echo "PC $pc를 찾지 못했습니다. PC Bluetooth 설정을 연 뒤 설치를 다시 실행하세요." >&2
            return 1
        fi
        if [ "$paired" != 'b true' ]; then
            echo "PC $pc 최초 페어링을 최대 60초간 시도합니다. Windows 승인창이 뜨면 확인하세요."
            timeout --kill-after=5s 65s bluetoothctl --timeout=60 \
                --agent=NoInputNoOutput pair "$pc" || true
            paired="$(busctl --system --timeout=5 get-property org.bluez "$device" \
                org.bluez.Device1 Paired 2>/dev/null || true)"
        fi
        if [ "$paired" != 'b true' ]; then
            echo "PC 페어링 미완료: Windows 승인을 확인한 뒤 setup_target.sh를 다시 실행하세요." >&2
            return 1
        fi
    fi
    trusted="$(busctl --system --timeout=5 get-property org.bluez "$device" \
        org.bluez.Device1 Trusted 2>/dev/null || true)"
    if [ "$trusted" = 'b true' ]; then
        echo "페어링된 PC $pc의 trust 설정 유지."
        return 0
    fi
    if ! busctl --system --timeout=5 set-property org.bluez "$device" \
        org.bluez.Device1 Trusted b true; then
        echo "PC trust 설정 실패: Bluetooth 준비 후 setup_target.sh를 다시 실행하세요." >&2
        return 1
    fi
    trusted="$(busctl --system --timeout=5 get-property org.bluez "$device" \
        org.bluez.Device1 Trusted 2>/dev/null || true)"
    if [ "$trusted" != 'b true' ]; then
        echo "PC trust 확인 실패: Bluetooth 준비 후 setup_target.sh를 다시 실행하세요." >&2
        return 1
    fi
    echo "페어링된 PC $pc trust 설정 완료."
    return 0
}

install_sensor_service() {
    local role opposite host script_dir project_root
    local sensor_dir template rendered opposite_state
    host="$(hostname -s)"
    case "$host" in
        craft2) role=accident; opposite=witness ;;
        craft3|craft4|seongugi|craft5|craft6) role=witness; opposite=accident ;;
        *)
            echo "hostname $host의 센서 역할이 미정입니다. 센서 자동 시작 등록은 생략합니다."
            return
            ;;
    esac
    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
    project_root="$(cd -- "$script_dir/../.." && pwd)"
    sensor_dir="$project_root/test"
    case "$sensor_dir" in
        /*) ;;
        *) echo "센서 실행 경로가 절대 경로가 아닙니다." >&2; return 1 ;;
    esac
    case "$sensor_dir" in
        *[!A-Za-z0-9_./-]*)
            echo "센서 서비스 경로에는 영문·숫자·밑줄·점·하이픈을 사용하세요: $sensor_dir" >&2
            return 1
            ;;
    esac
    template="$project_root/sensor/sensor@.service"
    rendered="$setup_tmp/sensor@.service"
    if [ ! -f "$template" ]; then
        echo "센서 서비스 템플릿을 찾을 수 없습니다: $template" >&2
        return 1
    fi
    sed "s|@SENSOR_DIR@|$sensor_dir|g" "$template" > "$rendered"
    install -m 0644 "$rendered" /etc/systemd/system/sensor@.service
    systemctl daemon-reload
    # 이전 역할은 다음 부팅 대상에서만 제외하며 현재 프로세스는 유지한다.
    if systemctl is-enabled --quiet "sensor@$opposite.service"; then
        systemctl disable "sensor@$opposite.service"
    fi
    systemctl enable "sensor@$role.service"
    if ! opposite_state="$(systemctl show --property=ActiveState --value \
        "sensor@$opposite.service" 2>/dev/null)"; then
        echo "기존 센서 상태를 확인하지 못해 새 역할 시작을 보류합니다. 부팅 등록은 완료됐습니다." >&2
        return
    fi
    case "$opposite_state" in
        inactive|failed) ;;
        *)
            echo "기존 sensor@$opposite.service가 $opposite_state 상태여서 새 역할 시작을 보류합니다."
            echo "현재 사고 처리가 끝난 뒤 역할을 전환하세요. 다음 부팅 역할은 $role입니다."
            return
            ;;
    esac
    if [ -x "$sensor_dir/${role}_aruco" ]; then
        # 이미 active인 센서에는 start가 영향을 주지 않는다.
        systemctl start "sensor@$role.service"
        echo "sensor@$role.service 부팅 등록·시작 완료: $sensor_dir/${role}_aruco"
    else
        echo "sensor@$role.service 부팅 등록 완료. 실행 파일 배치 위치: $sensor_dir/${role}_aruco"
        echo "Pi에서 make -C sensor aruco 또는 VM에서 ARM64 빌드한 파일을 위 경로에 복사하세요."
        echo "배치 후 sudo systemctl start sensor@$role.service 로 시작할 수 있습니다."
    fi
}

check_ai_system_libraries() {
    local candidate
    install_missing_packages libcamera-dev libavcodec-dev libavformat-dev libavutil-dev
    if ! pkg-config --atleast-version=2.0 libgpiod; then
        update_package_index
        candidate="$(LC_ALL=C apt-cache policy libgpiod-dev | awk '/Candidate:/ {print $2; exit}')"
        if [ -z "$candidate" ] || [ "$candidate" = '(none)' ] ||
            ! dpkg --compare-versions "$candidate" ge 2.0; then
            echo "AI 준비 실패: libgpiod v2 개발 패키지가 필요합니다." >&2
            echo "현재 APT 후보: ${candidate:-없음}. Trixie의 v2 또는 기존 v2 설치를 사용하세요." >&2
            echo "Bookworm 기본 v1은 사용할 수 없습니다. 저장소/OS는 자동 변경하지 않습니다." >&2
            return 1
        fi
        apt-get install -y libgpiod-dev
    fi
    pkg-config --atleast-version=2.0 libgpiod

    # 하드웨어를 열지 않고 현재 코드가 쓰는 API와 H.264 encoder를 확인한다.
    cat > "$setup_tmp/ai-libraries.cpp" <<'CPP'
#include <libcamera/control_ids.h>
#include <gpiod.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
int main() {
    auto wall_clock_id = libcamera::controls::FrameWallClock.id();
    auto *settings = gpiod_line_settings_new();
    if (!settings) return 1;
    gpiod_line_settings_free(settings);
    return wall_clock_id && avformat_version() && avutil_version() &&
           avcodec_find_encoder(AV_CODEC_ID_H264) ? 0 : 1;
}
CPP
    local flags=()
    read -r -a flags <<< "$(pkg-config --cflags --libs libcamera libgpiod libavcodec libavformat libavutil)"
    if ! /usr/bin/g++ -std=c++17 "$setup_tmp/ai-libraries.cpp" "${flags[@]}" \
        -o "$setup_tmp/ai-libraries" || ! "$setup_tmp/ai-libraries"; then
        echo "AI 준비 실패: libcamera FrameWallClock, libgpiod v2, H.264 encoder 구성을 확인하세요." >&2
        return 1
    fi
}

check_onnxruntime() {
    local root="$1"
    [ -r "$root/include/onnxruntime_cxx_api.h" ] &&
        [ -r "$root/include/onnxruntime_c_api.h" ] &&
        [ -r "$root/lib/libonnxruntime.so" ] || return 1
    LC_ALL=C readelf -h "$root/lib/libonnxruntime.so" |
        grep -q 'Machine:.*AArch64' || return 1
    cat > "$setup_tmp/ort-check.cpp" <<'CPP'
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <cstdio>
#include <cstring>
int main(int argc, char **argv) {
    const auto *base = OrtGetApiBase();
    const char *version = base ? base->GetVersionString() : nullptr;
    if (argc != 2 || !version || std::strcmp(version, argv[1]) != 0) {
        std::fprintf(stderr, "ONNX Runtime version: expected %s, found %s\n",
                     argc == 2 ? argv[1] : "argument", version ? version : "unavailable");
        return 1;
    }
    const auto *api = base->GetApi(ORT_API_VERSION);
    if (!api) return 1;
    Ort::InitApi(api);
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ace-setup");
        std::printf("ONNX Runtime %s\n", version);
        return 0;
    } catch (...) { return 1; }
}
CPP
    /usr/bin/g++ -std=c++17 -I"$root/include" "$setup_tmp/ort-check.cpp" \
        -L"$root/lib" -Wl,-rpath,"$root/lib" -lonnxruntime \
        -o "$setup_tmp/ort-check" || return 1
    LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$setup_tmp/ort-check" "$ORT_VERSION"
}

activate_onnxruntime() (
    local version_root="$1"
    if [ -L "$ORT_ROOT" ] &&
        [ "$(readlink -f -- "$ORT_ROOT")" = "$(readlink -f -- "$version_root")" ]; then
        check_onnxruntime "$ORT_ROOT" || return 1
        echo "기존 ONNX Runtime $ORT_VERSION 재사용: $ORT_ROOT"
        return
    fi

    local link_stage backup="" original=absent pending=0 committed=0
    link_stage="$(mktemp -d "${ORT_ROOT%/*}/.ace-onnxruntime-link.XXXXXX")" || return 1
    finish_onnxruntime_activation() {
        local status=$?
        if [ "$pending" -eq 1 ] && [ "$committed" -eq 0 ]; then
            if [ "$original" = directory ] && [ -d "$backup/runtime" ]; then
                if [ -L "$ORT_ROOT" ] && [ "$(readlink -- "$ORT_ROOT")" = "$version_root" ]; then
                    rm -- "$ORT_ROOT" || true
                fi
                if [ ! -e "$ORT_ROOT" ] && [ ! -L "$ORT_ROOT" ]; then
                    mv -Tn -- "$backup/runtime" "$ORT_ROOT" || true
                fi
                if [ -d "$backup/runtime" ]; then
                    echo "AI 준비 실패: 기존 런타임 복원 경로는 $backup/runtime 입니다." >&2
                fi
            elif [ "$original" = symlink ]; then
                if [ -L "$ORT_ROOT" ] || [ ! -e "$ORT_ROOT" ]; then
                    mv -Tf -- "$link_stage/rollback" "$ORT_ROOT" ||
                        echo "AI 준비 실패: 기존 링크 복원에 실패했습니다: $ORT_ROOT" >&2
                fi
            elif [ "$original" = absent ] && [ -L "$ORT_ROOT" ] &&
                [ "$(readlink -- "$ORT_ROOT")" = "$version_root" ]; then
                rm -- "$ORT_ROOT" || true
            fi
        fi
        [ -z "$backup" ] || rmdir -- "$backup" 2>/dev/null || true
        rm -f -- "$link_stage/current" "$link_stage/rollback" || true
        rmdir -- "$link_stage" 2>/dev/null || true
        return "$status"
    }
    trap finish_onnxruntime_activation EXIT
    trap 'exit 1' HUP INT TERM

    ln -s -- "$version_root" "$link_stage/current" || return 1
    if [ -L "$ORT_ROOT" ]; then
        original=symlink
        ln -s -- "$(readlink -- "$ORT_ROOT")" "$link_stage/rollback" || return 1
    elif [ -d "$ORT_ROOT" ]; then
        original=directory
        backup="$(mktemp -d "${ORT_ROOT}.backup.XXXXXX")" || return 1
    elif [ -e "$ORT_ROOT" ]; then
        echo "AI 준비 실패: $ORT_ROOT가 디렉터리나 링크가 아닙니다. 기존 파일을 보존합니다." >&2
        return 1
    fi
    pending=1
    if [ "$original" = directory ]; then
        mv -T -- "$ORT_ROOT" "$backup/runtime" || return 1
    fi
    if [ "$original" = symlink ]; then
        mv -Tf -- "$link_stage/current" "$ORT_ROOT" || return 1
    else
        mv -Tn -- "$link_stage/current" "$ORT_ROOT" || return 1
        if [ -L "$link_stage/current" ]; then
            echo "AI 준비 실패: $ORT_ROOT가 설치 중 생성되어 덮어쓰지 않았습니다." >&2
            return 1
        fi
    fi
    if ! check_onnxruntime "$ORT_ROOT"; then
        echo "AI 준비 실패: 전환 후 런타임 검사에 실패하여 기존 경로를 복원합니다." >&2
        return 1
    fi
    committed=1
    echo "ONNX Runtime $ORT_VERSION 준비: $ORT_ROOT -> $version_root"
    if [ "$original" = directory ]; then
        echo "기존 런타임 보존: $backup/runtime"
    fi
)

install_onnxruntime() {
    local version_root="${ORT_ROOT}-linux-aarch64-$ORT_VERSION"
    if [ -e "$ORT_ROOT" ] && [ ! -d "$ORT_ROOT" ] && [ ! -L "$ORT_ROOT" ]; then
        echo "AI 준비 실패: $ORT_ROOT가 디렉터리나 링크가 아닙니다. 기존 파일을 보존합니다." >&2
        return 1
    fi
    if [ -e "$version_root" ] || [ -L "$version_root" ]; then
        if ! check_onnxruntime "$version_root"; then
            echo "AI 준비 실패: 기존 $version_root의 헤더·ARM64 런타임·버전 검사가 실패했습니다. 기존 파일을 보존합니다." >&2
            return 1
        fi
    else
        ort_stage="$(mktemp -d "${ORT_ROOT%/*}/.ace-onnxruntime.XXXXXX")" || return 1
        local extracted="$ort_stage/onnxruntime-linux-aarch64-$ORT_VERSION"
        if { [ -e "$ORT_ROOT" ] || [ -L "$ORT_ROOT" ]; } && check_onnxruntime "$ORT_ROOT"; then
            mkdir -- "$extracted" || return 1
            cp -a -- "$ORT_ROOT/." "$extracted/" || return 1
        else
            local archive="onnxruntime-linux-aarch64-$ORT_VERSION.tgz"
            curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
                "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/$archive" \
                -o "$setup_tmp/$archive" || return 1
            printf '%s  %s\n' "$ORT_SHA256" "$setup_tmp/$archive" | sha256sum --check - || return 1
            tar -xzf "$setup_tmp/$archive" -C "$ort_stage" --no-same-owner || return 1
        fi
        if ! check_onnxruntime "$extracted"; then
            echo "AI 준비 실패: ARM64 런타임 $ORT_VERSION을 현재 OS에서 로드할 수 없습니다." >&2
            return 1
        fi
        # 버전 경로가 설치 중 생성되면 기존 내용을 보존한다.
        mv -Tn -- "$extracted" "$version_root" || return 1
        if [ -d "$extracted" ]; then
            echo "AI 준비 실패: $version_root가 설치 중 생성되어 덮어쓰지 않았습니다." >&2
            return 1
        fi
        check_onnxruntime "$version_root" || return 1
    fi
    activate_onnxruntime "$version_root"
}

main() {
    local ble_registration_ready=1
    if [ "$EUID" -ne 0 ]; then
        echo "sudo bash scripts/setting/setup_target.sh 로 실행하세요." >&2
        return 1
    fi
    if [ "$(dpkg --print-architecture)" != arm64 ] || [ "$(uname -m)" != aarch64 ]; then
        echo "이 설치는 Raspberry Pi OS 64-bit (arm64/aarch64)용입니다." >&2
        return 1
    fi
    # Host SDK 설정이 상속되어도 Pi의 native 패키지로 검사한다.
    unset PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR
    setup_tmp="$(mktemp -d /tmp/ace-target-setup.XXXXXX)"
    trap cleanup_setup EXIT

    echo "[1/3] 공통 빌드·BLE 패키지 설치"
    install_missing_packages build-essential cmake git gdb pkg-config rsync vim nano \
        openssh-server chrony bluez systemd openssl libglib2.0-dev libopencv-dev libsqlite3-dev libssl-dev \
        libsqlite3-0 sqlite3 tzdata ca-certificates curl
    pkg-config --exists gio-2.0 sqlite3
    # 기존 NTP 서버 설정은 그대로 사용한다. 실행 중인 서비스는 재시작하지 않는다.
    systemctl enable --now ssh.service chrony.service bluetooth.service
    remove_legacy_uploader_profile
    if ! pair_and_trust_pc; then
        ble_registration_ready=0
    fi
    install_sensor_service

    echo "[2/3] AI 개발 라이브러리·ARM64 ONNX Runtime 준비"
    check_ai_system_libraries
    install_onnxruntime

    if [ "$ble_registration_ready" -eq 0 ]; then
        echo "[3/3] BLE 등록 미완료. 패키지 설치와 센서 부팅 설정은 진행됐습니다." >&2
        echo "Windows Bluetooth 설정을 확인한 뒤 setup_target.sh를 다시 실행하세요." >&2
        return 1
    fi
    echo "[3/3] Target 설치 완료"
    echo "차량 ID·인증 정보·서버 주소는 uploader.env에 설정합니다."
    echo "Time Sync·Uploader·AI·P2P와 관측차 Filter는 각 모듈 deploy/install-service.sh로 등록합니다."
    echo "Pi 저장소 루트에서 make -C ai 후 ai/vehicle_detector.onnx를 배치하세요."
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
