#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Only load the wait helper; do not touch wireless devices or start a service.
source <(sed -n '/^wait_for_new_client() {$/,/^}$/p' "$root/p2p_go.sh")
declare -F wait_for_new_client >/dev/null

CLIENT_IPS=(192.168.49.2 192.168.49.3 192.168.49.4 192.168.49.5)
GO_IF=p2p-test-0
MODE=none
PING_CALLS=0
CURRENT_COUNT=0

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
sleep() { SECONDS=$((SECONDS + $1)); }
ping() {
    [[ "$*" == "-I $GO_IF -c 1 -W 1 -w 1 "* ]] || fail 'Ping is not interface/deadline bounded'
    PING_CALLS=$((PING_CALLS + 1))
    local address=${!#}
    case "$MODE:$address" in
        one:192.168.49.2|late:192.168.49.2) return 0 ;;
        late:192.168.49.3) (( SECONDS >= 12 )) && return 0 ;;
        all:*) return 0 ;;
    esac
    # Model an unreachable peer consuming the full one-second Ping timeout.
    SECONDS=$((SECONDS + 1))
    return 1
}

MODE=none; SECONDS=0; PING_CALLS=0
if wait_for_new_client 0; then fail 'No peers must time out'; fi
(( SECONDS == 60 )) || fail "No peers waited $SECONDS seconds, expected 60"
(( PING_CALLS == 48 )) || fail "No peers made $PING_CALLS probes, expected 48"

MODE=one; SECONDS=0; PING_CALLS=0
if wait_for_new_client 1; then fail 'One unchanged peer must time out'; fi
(( SECONDS == 60 )) || fail "One peer waited $SECONDS seconds, expected 60"
(( PING_CALLS == 60 )) || fail "One peer made $PING_CALLS probes, expected 60"

MODE=late; SECONDS=0; PING_CALLS=0; CURRENT_COUNT=0
wait_for_new_client 1 || fail 'A new peer must complete the wait'
(( CURRENT_COUNT == 2 && SECONDS < 60 )) || fail 'New peer was not reported promptly'

MODE=all; SECONDS=0; CURRENT_COUNT=0
wait_for_new_client 3 || fail 'All peers must complete immediately'
(( CURRENT_COUNT == 4 && SECONDS == 0 )) || fail 'All peers added an unnecessary wait'

printf 'PASS: GO reconnect wait is bounded by elapsed time (4 cases)\n'
