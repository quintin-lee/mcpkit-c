# Production Hardening & Zero-Copy I/O Design Specification

- **Date:** 2026-11-19
- **Status:** Proposed (Under Review)
- **Target Version:** `v0.3.0`
- **Scope:** Zero-Copy Scatter-Gather Vector I/O, Reactor Backpressure Watermarks, and Graceful Draining in MCPKit Core.

---

## 1. Intent & Context

MCPKit (`v0.2.0`) currently achieves over 1.5M QPS for JSON-RPC microbenchmarks and over 370k QPS on Streamable HTTP loopback sockets. However, under high-concurrency production deployments and hostile/slow network conditions, three operational resilience gaps exist:

1. **System Call Multiplication & Extra Buffer Copies**:
   - `mcp_transport_send` sends the payload bytes and then performs a separate `send(..., "\n", 1)` call in `socket.c` and `stdio.c`. This doubles system call overhead and risks packet fragmentation.
2. **Unbounded Outbox Memory Under Slow Clients (Missing Backpressure)**:
   - `mcp_async_server` queues outbound data on `struct async_conn.write_buf` without a ceiling. A stalled or slow client that stops reading while server workers or SSE streams continue pushing responses will cause exponential buffer reallocations, eventually exhausting server heap memory (OOM).
3. **Abrupt Shutdown on Service Reload (Missing Graceful Draining)**:
   - Calling `mcp_async_server_stop` or destroying the server immediately terminates the reactor, dropping in-flight HTTP requests and unwritten outbox frames. Production deployments require a graceful draining phase that stops accepting new connections while giving existing in-flight RPCs and buffers time to flush cleanly.

---

## 2. Goals, Non-Goals, and Anti-Goals

### 2.1 Goals
- **G1: Vectorized I/O (`mcp_transport_sendv`)**: Provide scatter-gather I/O (`writev`) support in `mcp_transport_ops_t`. In `socket.c` and `stdio.c`, combine message payload and `\n` into a single atomic system call.
- **G2: Backward Compatibility**: Existing transports that do not implement `sendv` must automatically fall back to sequential `send` calls without modifying callers.
- **G3: High & Low Watermark Backpressure**: Introduce configurable outbox watermarks (`outbox_high_watermark`, `outbox_low_watermark`, `outbox_max_drop_bytes`) in `mcp_async_server`. Pause `MCP_EVENT_READ` on congested connections to stop ingesting new requests, and forcefully shed connections exceeding the hard limit.
- **G4: Graceful Server Draining (`mcp_async_server_drain`)**: Provide a drain API that stops the listen socket and steps the reactor until active connections reach zero or a timeout expires.
- **G5: Zero Regression & Full Test Coverage**: 100% pass across all 88 existing unit tests, with new dedicated unit tests and benchmark verification under Release and ASan/UBSan.

### 2.2 Non-Goals
- **NG1: Rewriting JSON Parser/Serializer Internal Tree**: This design does not modify `mcp_json_serialize` to produce non-contiguous chunk arrays. The zero-copy boundary is established at the transport and network layer.
- **NG2: Platform-Specific Kernel Splice (`splice(2)`/`sendfile(2)`)**: We focus on standard POSIX `writev` to remain portable across Linux, BSD, and macOS without kernel-specific pipe requirements.

### 2.3 Anti-Goals
- **AG1: Breaking Transport ABI or Signature Changes**: Do not change the existing signature of `mcp_transport_send`. All enhancements must be additive.
- **AG2: Deadlocking During Drain**: The draining loop must never block indefinitely if a client socket hangs; the timeout parameter must guarantee bounded execution.

---

## 3. Approaches Considered & Trade-Offs

### 3.1 Approach A: Incremental & Non-invasive Enhancement (Selected)
- Add optional `sendv` to `mcp_transport_ops_t` with automated fallback to `send` if NULL.
- Optimize `socket.c` and `stdio.c` to use `writev` for atomic payload + newline output.
- Add outbox watermarks in `mcp_async_server_config_t` and implement read-pausing and hard-limit disconnection in `async_server.c`.
- Implement `mcp_async_server_drain` with listen socket detachment and timeout stepping.
- **Trade-off**: Zero breaking changes, high safety, immediate measurable reduction in system calls and memory explosion risk.

### 3.2 Approach B: Full Zero-Copy Pipeline (Rejected)
- Refactor `mcp_message_serialize` to return `mcp_iovec_t[]` structures instead of a `char*` string.
- Pass iovecs through outbox and network.
- **Trade-off**: High complexity, breaks internal string assumptions, risks subtle use-after-free bugs across complex JSON object lifecycles.

### 3.3 Approach C: Pure Runtime Control-Plane (Rejected)
- Only implement backpressure and draining in `async_server.c`; omit `writev` transport improvements.
- **Trade-off**: Leaves the 2x system call overhead in `socket.c` and `stdio.c` unaddressed.

---

## 4. Technical Architecture & Interfaces

### 4.1 Vectorized I/O Architecture

#### 4.1.1 Type Definitions (`include/mcpkit/transport/transport.h`)
```c
/**
 * @brief Scatter/gather I/O vector entry.
 *
 * Compatible with POSIX struct iovec layout.
 */
typedef struct mcp_iovec {
    const void *iov_base; /**< Pointer to contiguous buffer. */
    size_t      iov_len;  /**< Number of bytes in buffer. */
} mcp_iovec_t;
```

#### 4.1.2 Transport Ops Extension
```c
typedef struct mcp_transport_ops {
    mcp_status_t (*start)(mcp_context_t *ctx, mcp_transport_t *t);
    mcp_status_t (*send)(mcp_context_t *ctx, mcp_transport_t *t, const char *data, size_t len);
    mcp_status_t (*recv)(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
    mcp_status_t (*stop)(mcp_context_t *ctx, mcp_transport_t *t);

    /**
     * @brief Optional vector send callback.
     * Line-framed: transmits all vectors in iov followed by a single trailing newline '\n'.
     * If NULL, mcp_transport_sendv will fall back to concatenating vectors and calling send().
     */
    mcp_status_t (*sendv)(mcp_context_t *ctx, mcp_transport_t *t,
                          const mcp_iovec_t *iov, size_t iovcnt);
} mcp_transport_ops_t;

/**
 * @brief Sends multiple contiguous buffers as a single line-delimited message.
 *
 * Transmits the combined payload of iov[0..iovcnt-1] and appends a single trailing
 * newline ('\n') on the wire, matching mcp_transport_send framing.
 *
 * @param ctx    Context; may be NULL.
 * @param t      Target transport.
 * @param iov    Array of buffer vectors.
 * @param iovcnt Number of vectors in iov array.
 * @return MCP_OK on success; MCP_ERR_* on transport failure.
 */
mcp_status_t mcp_transport_sendv(mcp_context_t *ctx, mcp_transport_t *t,
                                 const mcp_iovec_t *iov, size_t iovcnt);
```

#### 4.1.3 Socket and Stdio Implementation (`src/transport/socket.c`, `src/transport/stdio.c`)
In `socket.c`:
- Implement `sock_sendv`:
  - Sets up `struct iovec k_iov[iovcnt + 1]`. Copies `iov[0..iovcnt-1]` and sets `k_iov[iovcnt] = { "\n", 1 }`.
  - Uses `writev(b->fd, k_iov, (int)(iovcnt + 1))` in a loop handling `EINTR`, non-blocking `EAGAIN` with `wait_until(..., POLLOUT, deadline)`, and partial writes across multiple vectors.
- Optimize `sock_send`:
  - Constructs `mcp_iovec_t v[1] = { { data, len } };`
  - Delegates directly to `sock_sendv(ctx, t, v, 1)`.
  - Result: single atomic system call for payload + newline, zero packet fragmentation.
- Fallback in `mcp_transport_sendv`:
  - If `ops->sendv == NULL`, concatenates vectors into a stack buffer (or heap buffer if total length exceeds 1024 bytes) and invokes `ops->send(ctx, t, buf, total_len)` exactly once.

---

### 4.2 Reactor Outbox Backpressure & Slow-Client Mitigation

#### 4.2.1 Configuration Options (`include/mcpkit/transport/async_server.h`)
```c
typedef struct {
    uint16_t port;
    const char *bind_addr;
    size_t max_connections;
    uint32_t keepalive_timeout_s;
    bool enable_websocket;
    bool enable_sse;

    /* Backpressure Watermarks */
    size_t outbox_high_watermark; /**< Max unwritten bytes before pausing reads (default: 1048576 = 1MB if 0). */
    size_t outbox_low_watermark;  /**< Resumes reads once unwritten bytes drop below this (default: 262144 = 256KB if 0). */
    size_t outbox_max_drop_bytes; /**< Max unwritten bytes before forcibly dropping connection (default: 4194304 = 4MB if 0). */
} mcp_async_server_config_t;
```

#### 4.2.2 State Machine in `src/transport/async_server.c`
In `struct async_conn`:
```c
    bool read_paused;           /**< True if MCP_EVENT_READ is currently disabled due to backpressure. */
    size_t queued_write_bytes;  /**< Total bytes currently pending write in outbox queue. */
    uint64_t last_write_time_ms;/**< Timestamp of last successful socket write for stall detection. */
```
In `session_entry_t`:
```c
    atomic_bool backpressured;  /**< True if session SSE stream is currently congested. */
```

**Backpressure Rules:**
1. **High Watermark Exceeded (`queued_write_bytes >= outbox_high_watermark`)**:
   - If `!conn->read_paused`:
     - Update reactor interest mask via `mcp_reactor_modify(s->reactor, conn->fd, MCP_EVENT_WRITE)`.
     - Set `conn->read_paused = true`.
     - Cease reading incoming requests on this socket.
   - For Streamable HTTP sessions, if the `GET /sse` connection hits high watermark, set `session->backpressured = true`. Incoming `POST` requests for this session immediately return HTTP `503 Service Unavailable` with `Retry-After: 1`, preventing memory buildup across multi-connection sessions.
2. **Low Watermark Cleared (`queued_write_bytes <= outbox_low_watermark`)**:
   - If `conn->read_paused`:
     - Update reactor interest mask via `mcp_reactor_modify(s->reactor, conn->fd, MCP_EVENT_READ | (has_writes ? MCP_EVENT_WRITE : 0))`.
     - Set `conn->read_paused = false`.
   - If this is an SSE connection, set `session->backpressured = false`, allowing POST requests to resume.
3. **Hard Ceiling Exceeded (`queued_write_bytes >= outbox_max_drop_bytes`)**:
   - To distinguish legitimate large responses (up to 4MB) from slow-loris stalled clients:
     Eviction occurs if `queued_write_bytes >= outbox_max_drop_bytes` AND the socket has been unable to advance writes for > 5000ms, OR if total queued bytes exceed 8MB (`2 * MCP_ASYNC_MAX_BODY`).
   - On eviction: closes socket immediately, frees unwritten buffers, and marks `conn->closing = true`.

---

### 4.3 Graceful Server Draining (`mcp_async_server_drain`)

#### 4.3.1 Interface (`include/mcpkit/transport/async_server.h`)
```c
/**
 * @brief Gracefully drains active server connections before shutting down.
 *
 * Thread-Safe Control:
 * - May be invoked either from an external control thread while mcp_async_server_run()
 *   is active, or synchronously in single-threaded step mode.
 * - Stops accepting new connections on listen_fd.
 * - Actively signals existing connections to finish: closes idle HTTP keep-alive sockets,
 *   sets Connection: close on in-flight HTTP responses, initiates RFC 6455 1001 Close
 *   frames on WebSockets, and terminates SSE streams.
 * - Waits up to timeout_ms milliseconds for all in-flight worker tasks and outbox buffers
 *   to drain cleanly.
 *
 * @param ctx        Context; may be NULL.
 * @param s          Async server handle; must not be NULL.
 * @param timeout_ms Maximum time to wait in ms (0 = immediate close, -1 = block indefinitely).
 * @return MCP_OK on clean zero-connection drain; MCP_ERR_TIMEOUT on forced expiration.
 */
mcp_status_t mcp_async_server_drain(mcp_context_t *ctx, mcp_async_server_t *s, int timeout_ms);
```

#### 4.3.2 Implementation Flow in `src/transport/async_server.c`
1. Close `s->listen_fd` and unregister from `s->reactor`. Set `s->listen_fd = -1`.
2. Set `atomic_store(&s->draining, true);`
3. Proactively signal existing connections:
   - For idle HTTP connections (`!conn->http_in_flight` and empty outbox): close immediately.
   - For active HTTP connections: ensure response headers include `Connection: close` and set `close_after = true`.
   - For WebSocket connections: enqueue RFC 6455 1001 Close frame with `close_after = true`.
   - For SSE streams: send stream end comment and close.
4. If `s->running` is true (multi-threaded mode):
   - Wake reactor with `mcp_reactor_wakeup(s->reactor)`.
   - Wait on `s->drain_cv` (using `pthread_cond_timedwait` for `timeout_ms`).
   - Worker thread signals `s->drain_cv` as soon as `conns_count == 0`.
5. If `s->running` is false (single-threaded step mode):
   - Drive loop stepping `mcp_async_server_step(ctx, s, 50)` until `conns_count == 0` or deadline passes.
6. If timeout expires with connections remaining:
   - Forcibly close all remaining fds and return `MCP_ERR_TIMEOUT`.
7. If all connections reach 0: return `MCP_OK`.

---

## 5. Verification & Testing Plan

1. **Unit Tests (`tests/unit/test_transport_sendv.c`)**:
   - `test_sendv_socket`: Send 3 vectors over socket transport, verify peer receives combined contiguous payload with single newline.
   - `test_sendv_fallback`: Create mock transport with `ops.sendv = NULL`, verify `mcp_transport_sendv` accurately loops through `ops.send`.
2. **Unit Tests (`tests/unit/test_async_backpressure.c`)**:
   - `test_backpressure_pause_and_resume`: Connect slow consumer client, push data until high watermark hit, assert read events paused. Drain client, assert read events resume.
   - `test_backpressure_hard_drop`: Connect unreading client, flood until `outbox_max_drop_bytes` exceeded, assert connection is closed with server RSS remaining bounded.
3. **Unit Tests (`tests/unit/test_async_drain.c`)**:
   - `test_graceful_drain_success`: Connect 2 clients with in-flight requests, invoke `mcp_async_server_drain(ctx, s, 2000)`, clients complete requests and close, drain returns `MCP_OK`.
   - `test_graceful_drain_timeout`: Connect client that holds socket open without closing, verify drain returns `MCP_ERR_TIMEOUT` within specified window and frees server cleanly.
4. **Benchmark Verification**:
   - Re-run `bench_http_throughput` and `bench_async_server` to verify zero regression in throughput and confirm lower CPU system-call utilization under `writev`.
5. **Double-Gate Memory Verification**:
   - Release: all 91+ tests pass with zero warnings.
   - ASan + UBSan: all tests pass with zero leaks and zero memory corruption.

---

## 6. What's Left to the Builder
- Precise partial-vector offset tracking inside `sock_sendv` when `writev` returns fewer bytes than the sum of vector lengths.
- Internal default constants for watermarks when `cfg` fields are 0.
