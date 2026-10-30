#!/bin/sh
# Async acceptance: unified HTTP/SSE and WebSocket server, ws-client, and concurrency test.
# argv: <async-server> <ws-client> <test-async-concurrency>
set -e

SRV="$1"
WS_CLIENT="$2"
CONCURRENCY="$3"

if [ -z "$SRV" ] || [ -z "$WS_CLIENT" ] || [ -z "$CONCURRENCY" ]; then
  echo "usage: $0 <async-server> <ws-client> <test-async-concurrency>"
  exit 2
fi

SRV_PID=""
cleanup() {
  if [ -n "$SRV_PID" ]; then
    kill "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Retry on ports
PORT=""
for P in 19876 29876 39876 49876; do
  "$SRV" "$P" >/dev/null 2>&1 &
  SRV_PID=$!
  sleep 0.3
  if kill -0 "$SRV_PID" 2>/dev/null; then
    PORT="$P"
    break
  fi
  wait "$SRV_PID" 2>/dev/null || true
  SRV_PID=""
done

if [ -z "$PORT" ]; then
  echo "FAIL: async-server would not start on any test port"
  exit 1
fi

echo "Server running on port $PORT (PID $SRV_PID)"

# 1. Test WebSocket CLI client
echo "Testing ws-client..."
out="$("$WS_CLIENT" "ws://127.0.0.1:$PORT/mcp" call echo "acceptance ws test")"
case "$out" in
  *acceptance\ ws\ test*) ;;
  *)
    echo "FAIL: ws-client echo output mismatch: $out"
    exit 1
    ;;
esac

# 2. Test high concurrency (HTTP, SSE, and WS)
echo "Testing high concurrency..."
"$CONCURRENCY" 127.0.0.1 "$PORT"

# 3. Clean server stop
kill "$SRV_PID" 2>/dev/null || true
wait "$SRV_PID" 2>/dev/null || true
SRV_PID=""

echo "async acceptance OK"
exit 0
