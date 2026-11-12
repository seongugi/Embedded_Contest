#!/bin/sh

# hostname에 맞는 Wi-Fi Direct GO/Client 서비스를 설치하고 즉시 시작한다.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
EXEC_ROOT="$PROJECT_ROOT/test"

case "$(hostname -s)" in
    craft2)
        SERVICE_NAME=p2p-go.service
        OTHER_SERVICE=p2p-client.service
        LAUNCHER_NAME=p2p_go.sh
        BINARY_NAME=broadcast_sender
        ;;
    craft3|craft4|craft5|seongugi|craft6)
        SERVICE_NAME=p2p-client.service
        OTHER_SERVICE=p2p-go.service
        LAUNCHER_NAME=p2p_client.sh
        BINARY_NAME=broadcast_receiver
        ;;
    *)
        echo "ERROR: 이 hostname에는 P2P 역할이 정의되지 않았습니다: $(hostname -s)" >&2
        exit 1
        ;;
esac

UNIT_SOURCE="$SCRIPT_DIR/systemd/$SERVICE_NAME"
UNIT_TARGET="/etc/systemd/system/$SERVICE_NAME"
RENDERED_UNIT=$(mktemp "/tmp/$SERVICE_NAME.XXXXXX")

cleanup()
{
    rm -f "$RENDERED_UNIT"
}
trap cleanup EXIT HUP INT TERM

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: sudo ./install-service.sh 로 실행해야 합니다." >&2
    exit 1
fi
test -x "$EXEC_ROOT/$LAUNCHER_NAME" || {
    echo "ERROR: $EXEC_ROOT/$LAUNCHER_NAME가 없습니다." >&2
    exit 1
}
test -x "$EXEC_ROOT/$BINARY_NAME" || {
    echo "ERROR: $EXEC_ROOT/$BINARY_NAME가 없습니다." >&2
    exit 1
}
test -f "$UNIT_SOURCE" || {
    echo "ERROR: $UNIT_SOURCE가 없습니다." >&2
    exit 1
}

sed -e "s|@EXEC_ROOT@|$EXEC_ROOT|g" \
    "$UNIT_SOURCE" > "$RENDERED_UNIT"
if grep -q '@[A-Z_][A-Z_]*@' "$RENDERED_UNIT"; then
    echo "ERROR: systemd 템플릿에 치환되지 않은 값이 있습니다." >&2
    exit 1
fi

install -m 0644 "$RENDERED_UNIT" "$UNIT_TARGET"
systemctl daemon-reload
if [ "$(systemctl show -p LoadState --value "$OTHER_SERVICE")" != not-found ]; then
    systemctl disable --now "$OTHER_SERVICE"
fi
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

if ! systemctl is-active --quiet "$SERVICE_NAME"; then
    systemctl --no-pager --full status "$SERVICE_NAME" || true
    echo "ERROR: $SERVICE_NAME 서비스가 시작되지 않았습니다." >&2
    exit 1
fi

systemctl --no-pager --full status "$SERVICE_NAME"
