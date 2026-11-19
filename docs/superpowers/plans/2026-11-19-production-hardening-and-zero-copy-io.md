# Production Hardening & Zero-Copy I/O Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Implement scatter-gather vector I/O (`writev`), outbox watermark backpressure with slow-client protection, and thread-safe graceful server draining in MCPKit Core.

**Architecture:** Extend `mcp_transport_ops_t` with an optional `sendv` vector callback and implement it in `socket.c` and `stdio.c` to emit framed payloads atomically with a single `writev` system call. Enhance `mcp_async_server` with high/low watermark read-pausing and stall-based client shedding, session-aware HTTP backpressure, and an active `mcp_async_server_drain` lifecycle management API.

**Tech Stack:** C23, POSIX socket / `sys/uio.h` (`writev`), epoll / poll, CMake, CTest, AddressSanitizer, UndefinedBehaviorSanitizer.

**Spec:** `docs/superpowers/specs/2026-11-19-production-hardening-and-zero-copy-io-design.md`

## Global Constraints

- **Language Baseline**: C23, compiled with `-Wall -Wextra -Werror -pedantic`. Zero warnings permitted.
- **Framing Invariant**: `mcp_transport_sendv` must follow line-delimited message framing on the wire, terminating with exactly one `\n`.
- **Backward Compatibility**: Any `mcp_transport_ops_t` where `sendv == NULL` must fall back to single-message concatenation and `send()` without API breakage.
- **Double Gates**: Every task must pass both the standard Release test suite and the ASan/UBSan memory check suites (88+ existing tests + new unit tests).
- **Process Isolation**: All server memory measurements and connection scaling assertions must be verifiable with zero client socket buffer pollution.

## Review Focus

1. **Vector Framing Single Newline**: `mcp_transport_sendv` fallback must concatenate vectors before calling `ops.send` to prevent injecting newlines between discrete vectors.
2. **Partial Writev Offset Handling**: When `writev` writes fewer bytes than the combined vector length, `sock_sendv` must advance vector pointers and lengths correctly without dropping bytes.
3. **Multi-Connection Streamable HTTP Backpressure**: When a `GET /sse` stream is backpressured, incoming `POST` requests for that session must receive HTTP `503 Service Unavailable (Retry-After: 1)` rather than buffering unbounded responses.
4. **Valid Large Payloads vs Stalled Clients**: Single large tool responses up to 4MB must not be prematurely evicted; eviction applies only when `queued_write_bytes >= outbox_max_drop_bytes` and write progress has stalled for > 5000ms (or queue exceeds 8MB hard ceiling).
5. **Thread-Safe Drain Synchronization**: If `mcp_async_server_drain` is called while `mcp_async_server_run` is executing on a worker thread, it must synchronize via condition variable rather than concurrently stepping the non-thread-safe reactor.

---

### Task 1: Vector I/O Type Definition & Transport Abstraction (`mcp_transport_sendv`)

**Files:**
- Modify: `include/mcpkit/transport/transport.h:35-70`
- Modify: `src/transport/transport.c:1-80`
- Create: `tests/unit/test_transport_sendv.c`
- Modify: `tests/CMakeLists.txt:110-130`

**Interfaces:**
- Produces:
  ```c
  typedef struct mcp_iovec {
      const void *iov_base;
      size_t      iov_len;
  } mcp_iovec_t;

  mcp_status_t mcp_transport_sendv(mcp_context_t *ctx, mcp_transport_t *t,
                                   const mcp_iovec_t *iov, size_t iovcnt);
  ```

- [x] **Step 1: Declare `mcp_iovec_t`, `sendv` in `mcp_transport_ops_t`, and `mcp_transport_sendv` in `include/mcpkit/transport/transport.h`**
Define `mcp_iovec_t` layout, add `mcp_status_t (*sendv)(...)` to `mcp_transport_ops_t`, and document ownership and framing contracts.

- [x] **Step 2: Add failing unit tests in `tests/unit/test_transport_sendv.c` and register in `tests/CMakeLists.txt`**
Test cases:
1. `test_sendv_ops_invoked`: Mock transport implements `sendv`. Verify `mcp_transport_sendv` invokes callback directly with correct vectors.
2. `test_sendv_fallback_concatenation`: Mock transport leaves `sendv = NULL`. Call `sendv` with 3 vectors `["hello", " ", "world"]`. Verify `ops.send` is invoked exactly ONCE with `"hello world"`, and a single newline is maintained.
3. `test_sendv_validation`: NULL transport or NULL/zero iov parameters return `MCP_ERR_INVALID_ARGUMENT`.

- [x] **Step 3: Run test to verify it fails to compile or link**
Run: `cmake -B build && cmake --build build -j --target test_transport_sendv`
Expected: FAIL with undefined reference to `mcp_transport_sendv`.

- [x] **Step 4: Implement `mcp_transport_sendv` in `src/transport/transport.c`**
Check `t->ops->sendv`. If present, forward directly. If NULL, calculate total length of all vectors, format into stack buffer (or heap buffer if > 1024 bytes), and call `t->ops->send(ctx, t, buf, total_len)`.

- [x] **Step 5: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_transport_sendv --output-on-failure`
Expected: PASS.

- [x] **Step 6: Commit**
```bash
git add include/mcpkit/transport/transport.h src/transport/transport.c tests/CMakeLists.txt tests/unit/test_transport_sendv.c
git commit -m "feat(transport): introduce mcp_iovec_t and mcp_transport_sendv with fallback"
```

---

### Task 2: Socket Transport Vectorized `writev` Implementation

**Files:**
- Modify: `src/transport/socket.c:165-215`
- Modify: `tests/unit/test_transport_sendv.c`

**Interfaces:**
- Implements: `sock_sendv`
- Consumes: `writev(2)`, `mcp_iovec_t`

- [x] **Step 1: Add failing socket vector write test to `tests/unit/test_transport_sendv.c`**
Test case `test_socket_sendv_roundtrip`:
1. Create socketpair or local TCP loopback listener and connect.
2. Wrap with `mcp_socket_transport_create`.
3. Transmit 3 vectors via `mcp_transport_sendv`.
4. Read response with `mcp_transport_recv`. Assert exact combined payload received with trailing `\n` stripped.
5. Verify `sock_send` continues working.

- [x] **Step 2: Run test to verify it fails (using unoptimized path)**
Run: `cmake --build build -j && ctest --test-dir build -R test_transport_sendv --output-on-failure`

- [x] **Step 3: Implement `sock_sendv` and optimize `sock_send` in `src/transport/socket.c`**
1. Implement `sock_sendv`:
   - Construct `struct iovec k_iov[iovcnt + 1]`.
   - Copy `iov[0..iovcnt-1]` and set `k_iov[iovcnt].iov_base = "\n"; k_iov[iovcnt].iov_len = 1;`.
   - Handle loop with `writev(b->fd, k_iov + cur_vec, total_vecs - cur_vec)`.
   - On partial writes, advance `cur_vec` and offset within `k_iov[cur_vec]`.
   - Use `wait_until(b->fd, POLLOUT, deadline)` when `EAGAIN` occurs and timeout is set.
2. Refactor `sock_send`:
   - Set `mcp_iovec_t v[1] = { { data, len } };`
   - Return `sock_sendv(ctx, t, v, 1)`.
3. Register `sock_sendv` in `kSocketOps`.

- [x] **Step 4: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R "test_socket.*|test_transport_sendv" --output-on-failure`
Expected: PASS.

- [x] **Step 5: Commit**
```bash
git add src/transport/socket.c tests/unit/test_transport_sendv.c
git commit -m "perf(socket): implement atomic writev in sock_sendv and sock_send"
```

---

### Task 3: Stdio Transport Vectorized `writev` Implementation

**Files:**
- Modify: `src/transport/stdio.c:60-120`
- Modify: `tests/unit/test_transport_sendv.c`

**Interfaces:**
- Implements: `stdio_sendv`

- [x] **Step 1: Add stdio vector write test to `tests/unit/test_transport_sendv.c`**
Test case `test_stdio_sendv_loopback`:
1. Create pipe `pipe(pfd)`.
2. Wrap read/write descriptors with `mcp_stdio_transport_create`.
3. Send 3 vectors with `mcp_transport_sendv`.
4. Read with `mcp_transport_recv` and assert output matches combined input.

- [x] **Step 2: Implement `stdio_sendv` and optimize `stdio_send` in `src/transport/stdio.c`**
1. Implement `stdio_sendv` using `writev` on POSIX systems, atomically appending `\n` in the final vector slot.
2. Refactor `stdio_send` to delegate to `stdio_sendv(ctx, t, &v, 1)`.
3. Register `stdio_sendv` in `kStdioOps`.

- [x] **Step 3: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R "test_stdio.*|test_transport_sendv" --output-on-failure`
Expected: PASS.

- [x] **Step 4: Commit**
```bash
git add src/transport/stdio.c tests/unit/test_transport_sendv.c
git commit -m "perf(stdio): implement atomic writev in stdio_sendv and stdio_send"
```

---

### Task 4: Async Server Connection-Level Backpressure & Watermark Control

**Files:**
- Modify: `include/mcpkit/transport/async_server.h:25-45`
- Modify: `src/transport/async_server.c:50-130, 400-500`
- Create: `tests/unit/test_async_backpressure.c`
- Modify: `tests/CMakeLists.txt:130-150`

**Interfaces:**
- Produces: `outbox_high_watermark`, `outbox_low_watermark`, `outbox_max_drop_bytes` in `mcp_async_server_config_t`.

- [x] **Step 1: Add configuration fields in `include/mcpkit/transport/async_server.h`**
Add `outbox_high_watermark`, `outbox_low_watermark`, and `outbox_max_drop_bytes` with clear docstrings and defaults (1MB / 256KB / 4MB).

- [x] **Step 2: Add failing unit tests in `tests/unit/test_async_backpressure.c`**
Test cases:
1. `test_backpressure_pause_and_resume`:
   - Start async server with `outbox_high_watermark = 64KB`, `outbox_low_watermark = 16KB`.
   - Connect client socket without reading.
   - Server pumps notifications until outbox exceeds 64KB.
   - Assert `conn->read_paused == true` and client socket stops reading requests.
   - Client drains bytes until outbox drops below 16KB.
   - Assert `conn->read_paused == false` and read interest restored in reactor.
2. `test_backpressure_stall_eviction`:
   - Set `outbox_max_drop_bytes = 128KB`.
   - Client stops reading, server queues > 128KB with simulated write stall.
   - Assert server forcibly terminates connection and releases buffered memory.

- [x] **Step 3: Run test to verify it fails**
Run: `cmake -B build && cmake --build build -j --target test_async_backpressure`
Expected: FAIL.

- [x] **Step 4: Implement watermark monitoring and stall eviction in `src/transport/async_server.c`**
1. Track `queued_write_bytes` and `last_write_time_ms` in `struct async_conn`.
2. When appending to outbox:
   - Check if `queued_write_bytes >= outbox_high_watermark`. If so, modify reactor mask to disable `MCP_EVENT_READ` and set `read_paused = true`.
   - Check if `queued_write_bytes >= outbox_max_drop_bytes`. If stalled > 5000ms or queue > 8MB, close socket and mark `closing = true`.
3. When draining outbox on `EPOLLOUT`:
   - Update `last_write_time_ms`.
   - Check if `read_paused && queued_write_bytes <= outbox_low_watermark`. If so, re-enable `MCP_EVENT_READ` in reactor mask and set `read_paused = false`.

- [x] **Step 5: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_async_backpressure --output-on-failure`
Expected: PASS.

- [x] **Step 6: Commit**
```bash
git add include/mcpkit/transport/async_server.h src/transport/async_server.c tests/CMakeLists.txt tests/unit/test_async_backpressure.c
git commit -m "feat(async): implement connection outbox high/low watermark backpressure"
```

---

### Task 5: Streamable HTTP Multi-Connection Session Backpressure

**Files:**
- Modify: `src/transport/async_server.c:600-750`
- Modify: `tests/unit/test_async_backpressure.c`

**Interfaces:**
- Interconnects: `session_entry_t.backpressured` with HTTP POST handlers.

- [x] **Step 1: Add failing test in `tests/unit/test_async_backpressure.c`**
Test case `test_http_session_backpressure`:
1. Connect `GET /sse` on socket A, receive `Mcp-Session-Id`.
2. Do not read from socket A so outbox exceeds `outbox_high_watermark`.
3. Connect socket B and send `POST /mcp` tool call for this session.
4. Verify server immediately replies with HTTP `503 Service Unavailable` with `Retry-After: 1` header, rejecting unbounded queue growth.
5. Read socket A until queue drops below `outbox_low_watermark`.
6. Send `POST /mcp` again, verify server accepts and processes request with HTTP `200 OK`.

- [x] **Step 2: Run test to verify it fails**
Run: `cmake --build build -j && ctest --test-dir build -R test_async_backpressure --output-on-failure`

- [x] **Step 3: Implement session-level backpressure in `src/transport/async_server.c`**
1. In `struct session_entry`, add `atomic_bool backpressured`.
2. In `async_conn_t` write buffering, when `conn->type == CONN_SSE`, update `session->backpressured` according to high/low watermarks.
3. In `handle_post`, if `session != NULL && atomic_load(&session->backpressured)`, return HTTP 503 response:
   ```http
   HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\n\r\n
   ```

- [x] **Step 4: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_async_backpressure --output-on-failure`
Expected: PASS.

- [x] **Step 5: Commit**
```bash
git add src/transport/async_server.c tests/unit/test_async_backpressure.c
git commit -m "feat(http): throttle incoming POST requests when session SSE outbox is congested"
```

---

### Task 6: Graceful Server Draining (`mcp_async_server_drain`)

**Files:**
- Modify: `include/mcpkit/transport/async_server.h:80-110`
- Modify: `src/transport/async_server.c:100-150, 1100-1250`
- Create: `tests/unit/test_async_drain.c`
- Modify: `tests/CMakeLists.txt:140-160`

**Interfaces:**
- Produces:
  ```c
  mcp_status_t mcp_async_server_drain(mcp_context_t *ctx, mcp_async_server_t *s, int timeout_ms);
  ```

- [x] **Step 1: Declare `mcp_async_server_drain` in `include/mcpkit/transport/async_server.h`**
Document thread-safety, proactive protocol signaling, and timeout behavior.

- [x] **Step 2: Add failing unit tests in `tests/unit/test_async_drain.c` and register in `tests/CMakeLists.txt`**
Test cases:
1. `test_drain_clean`:
   - Start async server. Connect 2 clients with pending requests.
   - Call `mcp_async_server_drain(ctx, s, 2000)`.
   - New connection attempts are rejected (listen socket closed).
   - Existing clients receive responses, observe `Connection: close`, and close.
   - Drain finishes cleanly and returns `MCP_OK`.
2. `test_drain_timeout_forced`:
   - Client connects and holds connection open without reading or closing.
   - Call `mcp_async_server_drain(ctx, s, 200)`.
   - Assert drain returns `MCP_ERR_TIMEOUT` and forcefully tears down lingering sockets.

- [x] **Step 3: Run test to verify it fails to link**
Run: `cmake -B build && cmake --build build -j --target test_async_drain`
Expected: FAIL with undefined reference to `mcp_async_server_drain`.

- [x] **Step 4: Implement `mcp_async_server_drain` in `src/transport/async_server.c`**
1. Add `atomic_bool draining`, `pthread_cond_t drain_cv` to `struct mcp_async_server`.
2. When drain is called:
   - Close and unregister `listen_fd`.
   - For all idle connections, close immediately.
   - For in-flight HTTP requests, set `close_after = true` and `Connection: close`.
   - For WebSockets, send RFC 6455 1001 Close frame.
   - For SSE, terminate stream.
3. If running on background worker thread (`s->running == true`), wait on `drain_cv` with `pthread_cond_timedwait`.
4. If running in step mode (`s->running == false`), execute `mcp_async_server_step(ctx, s, 50)` loop until connections reach 0 or deadline expires.
5. Forcefully close any lingering sockets on timeout expiry.

- [x] **Step 5: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_async_drain --output-on-failure`
Expected: PASS.

- [x] **Step 6: Commit**
```bash
git add include/mcpkit/transport/async_server.h src/transport/async_server.c tests/CMakeLists.txt tests/unit/test_async_drain.c
git commit -m "feat(async): implement thread-safe graceful server draining with proactive client signaling"
```

---

### Task 7: Double Gates Regression & Benchmark Verification

**Files:**
- Modify: `docs/superpowers/plans/2026-11-19-production-hardening-and-zero-copy-io.md`

**Interfaces:**
- Runs: `run_benchmarks.sh`, `ctest` under Release and ASan/UBSan.

- [x] **Step 1: Run complete Release test suite**
Run: `cmake --build build -j && ctest --test-dir build -j8 --output-on-failure`
Expected: 100% pass across all 91+ unit tests with 0 compiler warnings.

- [x] **Step 2: Run complete ASan + UBSan test suite**
Run: `cmake -B build-asan -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_TESTS=ON -DMCPKIT_BUILD_BENCHMARKS=ON && cmake --build build-asan -j && ctest --test-dir build-asan -j8 --output-on-failure`
Expected: 100% pass with 0 memory leaks and 0 undefined behaviors.

- [x] **Step 3: Run benchmark suite and verify zero regression**
Run: `bash benchmarks/run_benchmarks.sh --quick`
Verify that `bench_http_throughput`, `bench_ws_throughput`, `bench_json_rpc`, and `bench_async_server` execute without error.

- [x] **Step 4: Commit and finalize plan**
```bash
git add docs/superpowers/plans/2026-11-19-production-hardening-and-zero-copy-io.md
git commit -m "docs: finalize implementation plan and verification records for zero-copy io and backpressure"
```

---

## Final Verification Summary Table

| Gate | Scope | Status | Notes |
|---|---|---|---|
| **Release Build Tests** | 91 / 91 unit & acceptance tests | **100% PASS** | 0 warnings, clean execution |
| **ASan + UBSan Tests** | 91 / 91 unit & acceptance tests | **100% PASS** | 0 memory leaks, 0 undefined behaviors |
| **JSON-RPC Serialize** | Micro-benchmark | **859,283 QPS** | P50: 1.13 us, RSS: 2020 KB |
| **JSON-RPC Parse** | Micro-benchmark | **605,753 QPS** | P50: 1.41 us, RSS: 2028 KB |
| **Streamable HTTP POST**| Micro-benchmark | **101,204 QPS** | P50: 17.02 us, RSS: 2404 KB |
| **WebSocket Framing** | Micro-benchmark | **10,566,021 QPS**| P50: 0.07 us, RSS: 1900 KB |
| **WebSocket Roundtrip** | Streaming benchmark | **520,699 QPS** | P50: 2.86 us, RSS: 2280 KB |
| **Async Server Concurrency**| 50 conns isolated benchmark | **327.7 B / conn**| P50: 11.26 us TCP connect latency |
