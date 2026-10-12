#!/bin/sh
# Transport acceptance: socket echo over TCP, or HTTP POST handshake over pipes.
# argv: <socket|http> <server-binary> [probe-binary]
set -e

MODE="$1"
SRV="$2"
PROBE="$3"

if [ "$MODE" = "socket" ]; then
  # socket-server serves one connection then exits; retry bind ports.
  for PORT in 18765 28765 38765; do
    "$SRV" "$PORT" 2>/dev/null & SRV_PID=$!
    sleep 0.5
    if kill -0 $SRV_PID 2>/dev/null; then
      break
    fi
    wait $SRV_PID 2>/dev/null || true
  done
  if ! kill -0 $SRV_PID 2>/dev/null; then
    echo "FAIL: socket-server would not stay up on any port"
    exit 1
  fi
  out="$(timeout 10 "$PROBE" 127.0.0.1 "$PORT" 2>/dev/null)" || {
    echo "FAIL: probe failed"
    kill $SRV_PID 2>/dev/null || true
    wait $SRV_PID 2>/dev/null || true
    exit 1
  }
  wait $SRV_PID 2>/dev/null || true
  case "$out" in *sock-probe*) ;; *) echo "FAIL: echo text missing"; exit 1 ;; esac
  echo "transport socket acceptance OK"
  exit 0
fi

if [ "$MODE" = "http" ]; then
  post() {
    body="$1"; sid="$2"
    if [ -n "$sid" ]; then
      printf 'POST /mcp HTTP/1.1\r\nContent-Length: %s\r\nContent-Type: application/json\r\nMcp-Session-Id: %s\r\n\r\n%s' \
        "$(printf '%s' "$body" | wc -c)" "$sid" "$body"
    else
      printf 'POST /mcp HTTP/1.1\r\nContent-Length: %s\r\nContent-Type: application/json\r\n\r\n%s' \
        "$(printf '%s' "$body" | wc -c)" "$body"
    fi
  }
  out="$({ post '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"h","version":"0.1.0"}}}'
    post '{"jsonrpc":"2.0","method":"notifications/initialized"}' 'sess-1'
    post '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi http"}}}' 'sess-1'
  } | timeout 10 "$SRV" 2>/dev/null)" || { echo "FAIL: http-server run failed"; exit 1; }
  case "$out" in *Mcp-Session-Id:\ sess-1*) ;; *) echo "FAIL: missing session id"; exit 1 ;; esac
  case "$out" in *2025-06-18*) ;; *) echo "FAIL: bad handshake"; exit 1 ;; esac
  case "$out" in *hi\ http*) ;; *) echo "FAIL: echo text missing"; exit 1 ;; esac
  echo "transport http acceptance OK"
  exit 0
fi

echo "usage: $0 <socket|http> <server> [probe]"
exit 2
