# Resilience Phase 4: Protocol Honesty + Push Path

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the L2 method table and the dispatcher route table share one source of truth, and add a server->client notification (push) API wired into the stdio/loop serve loops.

**Architecture:** A new `src/method_table.h` (single source of truth) lists the 11 spec methods this build accepts (10 routed + notifications/initialized, which is notification-only). L2 validation loops that table plus a smaller "spec-known but unrouted" list, so L2 passes exactly the methods the server claims to understand and nothing it silently ignores. Push path: `mcp_server_notify_client(ctx, srv, session, method, params)` builds a caller-owned notification via the existing `mcp_notification_new` builder; serve loops (stdio + loop) gain a drain-queue step.

**Tech Stack:** C23, no new deps.

---

## Task 1: Method table as single source of truth

**Files:**
- Create: `src/method_table.h`
- Modify: `src/protocol/validate.c` (L2 loops server methods + l2-only methods)
- Modify: `src/server/dispatcher.c` (route_request uses server method table indices)

[ ] Step 1: create method_table.h (k_mcp_server_methods[11] + count static_assert + k_l2_only_methods[15])
[ ] Step 2: validate.c mcp_method_known loops both tables (remove k_known_methods literal)
[ ] Step 3: dispatcher.c route_request index-based dispatch over k_mcp_server_methods (10 routed = idx 0..9; idx 10 notifications/initialized is NOT routed)
[ ] Step 4: build clean, 41/41 ctest
[ ] Step 5: commit `refactor(protocol): share one method table between L2 and route`

## Task 2: mcp_server_notify_client + serve-loop wiring

**Files:**
- Modify: `include/mcpkit/server/server.h` (declare + Doxygen)
- Modify: `src/server/server.c` (build + send; caller retains params on failure)
- Modify: `src/transport/stdio.c` + `src/runtime/loop.c` (drain outbox each loop iteration)
- Modify: `src/server/internals.h` (outbox per session or per server)

Design: outbox is a growable per-server array of (session*, owned mcp_message_t*); `mcp_server_notify_client` pushes. Serve loops pop outbox before recv; on send failure the message is destroyed and the loop logs WARN. Outbox drained at serve-loop level only (stdio/loop), not dispatcher.

[ ] Step 1: declare API in server.h + internals fields
[ ] Step 2: implement in server.c (builder ownership: params taken on OK, retained on ERR)
[ ] Step 3: wire stdio.c + loop.c drain step
[ ] Step 4: build clean
[ ] Step 5: commit `feat(server): add mcp_server_notify_client push path with serve-loop drain`

## Task 3: Migrate examples/socket-server echo_handler to object_set_take

**Files:**
- Modify: `examples/socket-server/main.c`

[ ] Step 1: replace chained-|| object_set blocks with per-step object_set_take (matches 5d23b51 contract)
[ ] Step 2: build + run example via pipe
[ ] Step 3: commit `fix(examples): migrate socket-server handlers to object_set_take`

## Task 4: test_notify_client + negative-matrix assertion

**Files:**
- Create: `tests/unit/test_notify_client.c`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/test_validate.c` (or test_dispatcher.c) — assert REQUEST for notifications/initialized and for l2-only methods (e.g. sampling/createMessage) gets -32601, and that unknown_method WARN log is NOT emitted for l2-only methods (they are L2-known; only truly-unknown names hit the WARN)

[ ] Step 1: write test_notify_client (push visible to fake transport; outbox drain; send-fail destroys message)
[ ] Step 2: add negative-matrix cases
[ ] Step 3: 41 -> 42 ctest
[ ] Step 4: commit `test(server): prove notify_client push and honest L2 matrix`

## Task 5: Gates + cleanup

[ ] Step 1: gcc 42/42, clang 42/42, ASan+UBSan 42/42 (HTTP+SOCKET=ON)
[ ] Step 2: rm build dirs, tree clean
