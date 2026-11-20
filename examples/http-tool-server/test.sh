#!/usr/bin/env bash
#
# test.sh - Automated test suite for MCP HTTP Tool Server
# Verifies complete MCP Streamable HTTP lifecycle and tool execution.
#

set -euo pipefail

# ANSI color codes
GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[1;33m"
CYAN="\033[0;36m"
BOLD="\033[1m"
RESET="\033[0m"

pass() { echo -e "  [${GREEN}PASS${RESET}] $1"; }
fail() { echo -e "  [${RED}FAIL${RESET}] $1"; exit 1; }
info() { echo -e "${CYAN}==>${RESET} ${BOLD}$1${RESET}"; }

PORT="${1:-18080}"
BASE_URL="http://127.0.0.1:${PORT}/mcp"
SERVER_BIN="./build/examples/http-tool-server"

SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo -e "\n${YELLOW}Shutting down test server (PID $SERVER_PID)...${RESET}"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# 0. Ensure server binary exists
if [ ! -x "$SERVER_BIN" ]; then
    info "Building http-tool-server target..."
    cmake --build build --target http-tool-server
fi

# 1. Start Server
info "Starting http-tool-server on port ${PORT}..."
"$SERVER_BIN" "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.3

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "Server failed to start on port $PORT"
fi
pass "Server is running (PID: $SERVER_PID) on $BASE_URL"

echo ""

# -----------------------------------------------------------------------------
# Test 1: Initialize Handshake
# -----------------------------------------------------------------------------
info "Step 1: Protocol Handshake (initialize)"
INIT_PAYLOAD='{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": {"name": "test-runner", "version": "1.0.0"}
  }
}'

RESP_HEADER=$(mktemp)
RESP_BODY=$(curl -s -i -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -d "$INIT_PAYLOAD" \
    --dump-header "$RESP_HEADER")

HTTP_STATUS=$(grep -m1 "HTTP/" "$RESP_HEADER" | awk '{print $2}')
SESSION_ID=$(grep -i "Mcp-Session-Id:" "$RESP_HEADER" | tr -d '\r' | awk '{print $2}')
rm -f "$RESP_HEADER"

if [ "$HTTP_STATUS" != "200" ]; then
    fail "Expected HTTP 200 on initialize, got $HTTP_STATUS"
fi
if [ -z "$SESSION_ID" ]; then
    fail "Failed to extract Mcp-Session-Id from response header"
fi
if ! echo "$RESP_BODY" | grep -q '"name":"mcpkit-basic-http-server"'; then
    fail "Server info mismatch in initialize response: $RESP_BODY"
fi
pass "Handshake successful (Mcp-Session-Id: ${SESSION_ID})"

# -----------------------------------------------------------------------------
# Test 2: Negative Test - Access before notifications/initialized
# -----------------------------------------------------------------------------
info "Step 2: Negative test - Calling tools/list before notifications/initialized"
UNINIT_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{"jsonrpc": "2.0", "id": 99, "method": "tools/list"}')

if echo "$UNINIT_RESP" | grep -q "session not initialized"; then
    pass "Correctly rejected uninitialized session with: 'session not initialized'"
else
    fail "Expected session rejection, but got: $UNINIT_RESP"
fi

# -----------------------------------------------------------------------------
# Test 3: Send notifications/initialized
# -----------------------------------------------------------------------------
info "Step 3: Protocol Readiness (notifications/initialized)"
INITED_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{"jsonrpc": "2.0", "method": "notifications/initialized"}')

if [ "$INITED_STATUS" != "202" ]; then
    fail "Expected HTTP 202 for initialized notification, got $INITED_STATUS"
fi
pass "Session state transitioned to INITIALIZED (HTTP 202 Accepted)"

# -----------------------------------------------------------------------------
# Test 4: List Tools & ETag Discovery
# -----------------------------------------------------------------------------
info "Step 4: Tool Discovery (tools/list)"
RESP_HEADER=$(mktemp)
TOOLS_RESP=$(curl -s -i -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{"jsonrpc": "2.0", "id": 2, "method": "tools/list"}' \
    --dump-header "$RESP_HEADER")

ETAG=$(grep -i "^ETag:" "$RESP_HEADER" | tr -d '\r' | awk '{print $2}')
rm -f "$RESP_HEADER"

if ! echo "$TOOLS_RESP" | grep -q '"name":"echo"'; then
    fail "Tool 'echo' missing from tools/list"
fi
if ! echo "$TOOLS_RESP" | grep -q '"name":"calculate"'; then
    fail "Tool 'calculate' missing from tools/list"
fi
if ! echo "$TOOLS_RESP" | grep -q '"name":"get_system_time"'; then
    fail "Tool 'get_system_time' missing from tools/list"
fi
pass "Discovered tools: echo, calculate, get_system_time (ETag: ${ETAG:-none})"

# -----------------------------------------------------------------------------
# Test 5: Conditional Cache Verification (304 Not Modified)
# -----------------------------------------------------------------------------
if [ -n "$ETAG" ]; then
    info "Step 5: Conditional Caching (If-None-Match ETag)"
    CACHE_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE_URL" \
        -H "Content-Type: application/json" \
        -H "Mcp-Session-Id: $SESSION_ID" \
        -H "If-None-Match: $ETAG" \
        -d '{"jsonrpc": "2.0", "id": 3, "method": "tools/list"}')

    if [ "$CACHE_STATUS" == "304" ]; then
        pass "Conditional request returned HTTP 304 Not Modified"
    else
        fail "Expected HTTP 304, got $CACHE_STATUS"
    fi
fi

# -----------------------------------------------------------------------------
# Test 6: Call Tool - echo
# -----------------------------------------------------------------------------
info "Step 6: Invoke Tool 'echo'"
ECHO_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 4,
      "method": "tools/call",
      "params": {
        "name": "echo",
        "arguments": {"message": "Verification automation test"}
      }
    }')

if echo "$ECHO_RESP" | grep -q '\[Echo\] Verification automation test'; then
    pass "Tool 'echo' succeeded with formatted output"
else
    fail "Tool 'echo' output mismatch: $ECHO_RESP"
fi

# -----------------------------------------------------------------------------
# Test 7: Call Tool - calculate (Addition)
# -----------------------------------------------------------------------------
info "Step 7: Invoke Tool 'calculate' (Arithmetic Addition)"
CALC_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 5,
      "method": "tools/call",
      "params": {
        "name": "calculate",
        "arguments": {"operation": "add", "a": 55, "b": 45}
      }
    }')

if echo "$CALC_RESP" | grep -q 'Calculation result: 55.000000 add 45.000000 = 100.000000'; then
    pass "Tool 'calculate' (add) returned 100.000000"
else
    fail "Tool 'calculate' output mismatch: $CALC_RESP"
fi

# -----------------------------------------------------------------------------
# Test 8: Call Tool - calculate (Division by Zero Protection)
# -----------------------------------------------------------------------------
info "Step 8: Invoke Tool 'calculate' (Zero-Division Edge Case)"
CALC_DIV0=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 6,
      "method": "tools/call",
      "params": {
        "name": "calculate",
        "arguments": {"operation": "divide", "a": 10, "b": 0}
      }
    }')

if echo "$CALC_DIV0" | grep -q 'Error: Division by zero'; then
    pass "Tool 'calculate' safely handled division by zero"
else
    fail "Tool 'calculate' division by zero failed: $CALC_DIV0"
fi

# -----------------------------------------------------------------------------
# Test 9: Call Tool - get_system_time
# -----------------------------------------------------------------------------
info "Step 9: Invoke Tool 'get_system_time'"
TIME_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 7,
      "method": "tools/call",
      "params": {
        "name": "get_system_time",
        "arguments": {}
      }
    }')

if echo "$TIME_RESP" | grep -q 'Current Server Time (UTC):'; then
    pass "Tool 'get_system_time' returned UTC timestamp"
else
    fail "Tool 'get_system_time' output mismatch: $TIME_RESP"
fi

# -----------------------------------------------------------------------------
# Test 10: Read Resource - test://info
# -----------------------------------------------------------------------------
info "Step 10: Read Resource 'test://info'"
RES_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 8,
      "method": "resources/read",
      "params": {"uri": "test://info"}
    }')

if echo "$RES_RESP" | grep -q '"uri":"test://info"' && echo "$RES_RESP" | grep -q 'mcpkit-c'; then
    pass "Resource 'test://info' read successfully"
else
    fail "Resource read mismatch: $RES_RESP"
fi

# -----------------------------------------------------------------------------
# Test 11: List Resource Templates - resources/templates/list
# -----------------------------------------------------------------------------
info "Step 11: List Resource Templates (resources/templates/list)"
TMPL_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 9,
      "method": "resources/templates/list"
    }')

if echo "$TMPL_RESP" | grep -q '"uriTemplate":"test://files/{+path}"'; then
    pass "Resource template 'test://files/{+path}' discovered successfully"
else
    fail "Resource template listing mismatch: $TMPL_RESP"
fi

# -----------------------------------------------------------------------------
# Test 12: Read Parameterized Resource via Template Match
# -----------------------------------------------------------------------------
info "Step 12: Read Dynamic Resource via Template Match"
DYN_RESP=$(curl -s -X POST "$BASE_URL" \
    -H "Content-Type: application/json" \
    -H "Mcp-Session-Id: $SESSION_ID" \
    -d '{
      "jsonrpc": "2.0",
      "id": 10,
      "method": "resources/read",
      "params": {"uri": "test://files/reports/2026-audit.pdf"}
    }')

if echo "$DYN_RESP" | grep -q 'Dynamic content for path: reports/2026-audit.pdf'; then
    pass "Dynamic resource successfully resolved template pattern and extracted path variable"
else
    fail "Dynamic resource resolution mismatch: $DYN_RESP"
fi

echo ""
echo -e "${GREEN}${BOLD}================================================================${RESET}"
echo -e "${GREEN}${BOLD} 🎉 All 12 test stages executed sequentially and passed successfully!${RESET}"
echo -e "${GREEN}${BOLD}================================================================${RESET}"
