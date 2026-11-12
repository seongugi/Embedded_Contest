#!/bin/sh

# 관측차의 저장소 안 실행 파일을 부팅 서비스로 등록한다.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: sudo sh filter/deploy/install-service.sh 로 실행하세요." >&2
    exit 1
fi

case "$(hostname -s)" in
    craft3|craft4|craft5|seongugi|craft6) ;;
    *)
        echo "ERROR: Witness Filter는 관측차(craft3~craft6, seongugi)용입니다." >&2
        exit 1
        ;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
EXEC_ROOT="$PROJECT_ROOT/test"
UNIT_SOURCE="$SCRIPT_DIR/systemd/witness_filter.service"
UNIT_TARGET=/etc/systemd/system/witness_filter.service

case "$EXEC_ROOT" in
    *[!A-Za-z0-9_./-]*)
        echo "ERROR: 서비스 경로에는 영문·숫자·밑줄·점·하이픈을 사용하세요: $EXEC_ROOT" >&2
        exit 2
        ;;
esac
test -x "$EXEC_ROOT/witness_filter" || {
    echo "ERROR: $EXEC_ROOT/witness_filter가 없습니다. make -C filter로 빌드하세요." >&2
    exit 1
}
test -f "$UNIT_SOURCE" || {
    echo "ERROR: $UNIT_SOURCE가 없습니다." >&2
    exit 1
}

RENDERED_UNIT=$(mktemp /tmp/witness_filter.service.XXXXXX)
cleanup()
{
    rm -f "$RENDERED_UNIT"
}
trap cleanup EXIT HUP INT TERM

sed "s|@EXEC_ROOT@|$EXEC_ROOT|g" "$UNIT_SOURCE" > "$RENDERED_UNIT"
if grep -q '@[A-Z_][A-Z_]*@' "$RENDERED_UNIT"; then
    echo "ERROR: systemd 템플릿에 치환되지 않은 값이 있습니다." >&2
    exit 1
fi

install -m 0644 "$RENDERED_UNIT" "$UNIT_TARGET"
systemctl daemon-reload
systemctl enable witness_filter.service
systemctl restart witness_filter.service
if ! systemctl is-active --quiet witness_filter.service; then
    systemctl --no-pager --full status witness_filter.service || true
    echo "ERROR: witness_filter.service가 시작되지 않았습니다." >&2
    exit 1
fi
systemctl --no-pager --full status witness_filter.service
