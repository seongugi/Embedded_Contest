#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "========================================"
echo "      Application Deployment"
echo "========================================"

#--------------------------------------------------
# Executables
#--------------------------------------------------
echo "[1/4] Executable"

read -rp "타겟으로 보낼 파일명 (여러 개면 공백으로 구분): " FILE_INPUT

if [ -z "$FILE_INPUT" ]; then
    echo "ERROR: 파일명이 필요합니다."
    exit 1
fi

read -r -a FILES <<< "$FILE_INPUT"

# 로컬 파일 존재 확인
for FILE in "${FILES[@]}"; do
    FILE_PATH="$SCRIPT_DIR/$FILE"

    if [ ! -f "$FILE_PATH" ]; then
        echo "ERROR: 파일을 찾을 수 없습니다."
        echo "확인 경로: $FILE_PATH"
        exit 1
    fi
done

echo
echo "전송할 파일:"
for FILE in "${FILES[@]}"; do
    echo "  - $FILE"
done


#--------------------------------------------------
# Target
#--------------------------------------------------
echo
echo "[2/4] Deployment Target"

read -rp "라즈베리파이 접속 주소 (사용자명@IP): " TARGET

if [ -z "$TARGET" ]; then
    echo "ERROR: 라즈베리파이 접속 주소가 필요합니다."
    exit 1
fi

if [[ "$TARGET" != *@* ]]; then
    echo "ERROR: 형식은 사용자명@IP 형식이어야 합니다."
    exit 1
fi

TARGET_USER="${TARGET%@*}"

read -rp "홈 디렉터리 아래 배포할 경로 (예: filter, p2p, sensor) [/home/$TARGET_USER]: " TARGET_DIR

# Enter만 누르면 사용자 홈
if [ -z "$TARGET_DIR" ]; then
    TARGET_DIR="/home/$TARGET_USER"
# 절대경로가 아니면 /home/<user>/ 아래로 처리
elif [[ "$TARGET_DIR" != /* ]]; then
    TARGET_DIR="/home/$TARGET_USER/$TARGET_DIR"
fi

echo
echo "배포 대상:"
echo "  사용자@호스트 : $TARGET"
echo "  배포 경로     : $TARGET_DIR"

echo
read -rsp "라즈베리파이 비밀번호: " PASSWORD
echo


#--------------------------------------------------
# Upload
#--------------------------------------------------
echo
echo "[3/4] Upload"

# 원격 디렉터리 생성
sshpass -p "$PASSWORD" ssh "$TARGET" \
    "mkdir -p '$TARGET_DIR'"

UPLOAD_FILES=()

for FILE in "${FILES[@]}"; do
    UPLOAD_FILES+=("$SCRIPT_DIR/$FILE")
done

# 여러 파일 한 번에 업로드
sshpass -p "$PASSWORD" scp \
    "${UPLOAD_FILES[@]}" \
    "$TARGET:$TARGET_DIR/"

echo
echo "업로드 완료"


#--------------------------------------------------
# Execute
#--------------------------------------------------
echo
echo "[4/4] Execute"

read -rp "타겟에서 실행할 파일명 (실행하지 않으려면 Enter): " RUN_FILE

if [ -z "$RUN_FILE" ]; then
    RUN_FILE="__NO_RUN__"
fi

sshpass -p "$PASSWORD" ssh "$TARGET" \
    bash -s -- "$TARGET_DIR" "$RUN_FILE" "${FILES[@]}" <<'EOF'

set -e

TARGET_DIR="$1"
RUN_FILE="$2"
shift 2

cd "$TARGET_DIR"

echo
echo "타겟 파일 확인:"

for FILE in "$@"; do
    chmod +x "$FILE"

    printf "  %-30s " "$FILE"
    file "$FILE" | sed 's/^.*: //'
done

if [ -n "$RUN_FILE" ] && [ "$RUN_FILE" != "__NO_RUN__" ]; then

    FOUND=0

    for FILE in "$@"; do
        if [ "$FILE" = "$RUN_FILE" ]; then
            FOUND=1
            break
        fi
    done

    if [ "$FOUND" -ne 1 ]; then
        echo
        echo "ERROR: 실행 파일 '$RUN_FILE'이 전송 목록에 없습니다."
        exit 1
    fi

    echo
    echo "========================================"
    echo "      Execute: $RUN_FILE"
    echo "========================================"

    "./$RUN_FILE"

    EXIT_CODE=$?

    echo
    echo "Exit code: $EXIT_CODE"

else

    echo
    echo "실행할 파일을 선택하지 않았습니다."

fi

EOF


echo
echo "========================================"
echo "Deployment completed successfully."
echo "========================================"
