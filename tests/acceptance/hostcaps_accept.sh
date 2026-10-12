#!/bin/sh
# Host-capabilities acceptance: server fires sampling/createMessage + roots/list
# at a live peer over a cross-connected stdio pair; the peer answers canned.
# argv: <host-caps example> <probe>
set -e

SRV="$1"
PROBE="$2"

S2P="$(mktemp -u /tmp/hc_s2p.XXXXXX)"
P2S="$(mktemp -u /tmp/hc_p2s.XXXXXX)"
mkfifo "$S2P" "$P2S"
cleanup() { rm -f "$S2P" "$P2S"; }
trap cleanup EXIT INT TERM

LOG_SRV="$(mktemp /tmp/hc_srv.XXXXXX)"
LOG_PROBE="$(mktemp /tmp/hc_probe.XXXXXX)"
CALL_JSON="$(mktemp /tmp/hc_call.XXXXXX)"
trap 'rm -f "$S2P" "$P2S" "$LOG_SRV" "$LOG_PROBE" "$CALL_JSON"' EXIT INT TERM

"$SRV" < "$P2S" > "$S2P" 2>"$LOG_SRV" &
SRV_PID=$!
"$PROBE" < "$S2P" 2>"$LOG_PROBE" | tee "$P2S" > "$CALL_JSON" &
PROBE_PID=$!

TIMEOUT=15
elapsed=0
while kill -0 "$PROBE_PID" 2>/dev/null; do
  sleep 1
  elapsed=$((elapsed + 1))
  if [ "$elapsed" -ge "$TIMEOUT" ]; then
    echo "FAIL: probe timed out after ${TIMEOUT}s"
    kill "$SRV_PID" "$PROBE_PID" 2>/dev/null || true
    exit 1
  fi
done
wait "$PROBE_PID"
PROBE_EXIT=$?
kill "$SRV_PID" 2>/dev/null || true
wait "$SRV_PID" 2>/dev/null || true

[ "$PROBE_EXIT" -eq 0 ] || { echo "FAIL: probe exit=$PROBE_EXIT"; exit 1; }
grep -q "queued sampling/createMessage + roots/list" "$CALL_JSON" \
  || { echo "FAIL: missing queued receipt"; exit 1; }
grep -q "PEER_ROOTS_OK" "$LOG_PROBE" \
  || { echo "FAIL: peer never answered roots/list"; exit 1; }
grep -q "PEER_SAMPLING_OK" "$LOG_PROBE" \
  || { echo "FAIL: peer never answered sampling/createMessage"; exit 1; }

echo "hostcaps acceptance OK"
