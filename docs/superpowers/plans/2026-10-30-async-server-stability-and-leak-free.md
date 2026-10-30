# Async Server Stability and Leak-Free Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement robust connection lifecycle management (zero leak), streaming HTTP parser with two-phase check and sliding window, WebSocket continuation frame reassembly, and deadlock-free ref-counted session reaper in `async_server.c`, along with build warning hardening and comprehensive unit tests.

**Architecture:** 
- **Task 1: Build Hardening & Dead Options Cleanup** — Enable `-Wall -Wextra` in root CMakeLists.txt and prune dead MCPKIT_BUILD_APPS/CLIENT/SERVER options.
- **Task 2: Connection Lifecycle & Zero-Leak (Async Conn Doubly-Linked List)** — Introduce `prev`/`next` doubly-linked list, `detached` idempotency flag, `conn_detach_and_close`, `mcp_async_server_active_connections` probe, and cleanup logic.
- **Task 3: Streaming HTTP State Machine & Sliding Window** — Implement two-phase check (Headers + Content-Length), max body cap (4MB), read buffer capping, strict in-flight serialization (`http_in_flight`), and sliding window consumption (`memmove`).
- **Task 4: WebSocket Continuation Frame Reassembly** — Implement RFC 6455 §5.4 chunk reassembly state machine in `async_server.c` with 4MB DoS payload capping.
- **Task 5: Ref-Counted Session Reaper & Deadlock Immunity** — Introduce `ref_count` on `session_entry_t`, find_session inner ref holding, task lifetime binding, batch reaper with lock-free disconnection queue, and comprehensive unit tests.

**Tech Stack:** C23, POSIX sockets, epoll, pthreads (`threads.h`), CMake / CTest, AddressSanitizer / UBSanitizer.

---

## Chunk 1: Build Hardening, Dead Options Cleanup, and Connection Lifecycle (Zero-Leak)

### Task 1: Build Hardening & CMake Options Cleanup

**Files:**
- Modify: `CMakeLists.txt:17-25`
- Modify: `cmake/MCPKitOptions.cmake:3-5`
- Modify: `README.md:65-67`

- [ ] **Step 1: Edit root CMakeLists.txt to add `-Wall -Wextra`**

```cmake
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()
```

- [ ] **Step 2: Remove dead options from `cmake/MCPKitOptions.cmake`**

Modify `cmake/MCPKitOptions.cmake` to remove `MCPKIT_BUILD_APPS`, `MCPKIT_BUILD_CLIENT`, and `MCPKIT_BUILD_SERVER`.

- [ ] **Step 3: Update README.md build options table**

Remove the three dead options rows from `README.md`.

- [ ] **Step 4: Run build and verify zero warnings**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
Expected: Build succeeds with zero warnings.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt cmake/MCPKitOptions.cmake README.md
git commit -m "build: enforce -Wall -Wextra and remove dead MCPKIT_BUILD_* options"
```

---

### Task 2: Connection Lifecycle & Zero-Leak (Doubly-Linked List & Probe API)

**Files:**
- Modify: `include/mcpkit/transport/async_server.h:60-70`
- Modify: `src/transport/async_server.c:40-110, 136-160, 690-715, 870-922`

- [ ] **Step 1: Add active connections probe declaration to public header**

Modify `include/mcpkit/transport/async_server.h`:
```c
/**
 * @brief Returns the current number of active connections tracked by the server.
 * @param s Async server handle; may be NULL.
 * @return Active connection count, or 0 if s is NULL.
 */
size_t mcp_async_server_active_connections(const mcp_async_server_t *s);
```

- [ ] **Step 2: Update `async_conn_t` and `mcp_async_server_t` structs in `src/transport/async_server.c`**

Add `prev`, `next`, `detached`, `http_in_flight` to `async_conn_t`, and `conns_count` to `mcp_async_server_t`.

- [ ] **Step 3: Implement `conn_detach_and_close` and update `conn_unref` & `on_accept`**

Implement deterministic detachment:
```c
static void conn_detach_and_close(mcp_async_server_t *s, async_conn_t *conn) {
    if (conn == NULL) return;
    if (atomic_exchange(&conn->detached, true)) {
        return;
    }
    atomic_store(&conn->closing, true);
    if (conn->fd >= 0) {
        mcp_reactor_remove(s->ctx, s->reactor, conn->fd);
        close(conn->fd);
        conn->fd = -1;
    }
    if (conn->type == CONN_SSE && conn->session_id[0] != '\0') {
        mtx_lock(&s->sessions_mtx);
        for (session_entry_t *cur = s->sessions; cur != NULL; cur = cur->next) {
            if (cur->sse_conn == conn) {
                cur->sse_conn = NULL;
                break;
            }
        }
        mtx_unlock(&s->sessions_mtx);
    }
    mtx_lock(&s->conns_mtx);
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        s->conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    if (s->conns_count > 0) {
        s->conns_count--;
    }
    mtx_unlock(&s->conns_mtx);
    conn_unref(conn);
}
```

- [ ] **Step 4: Implement `mcp_async_server_active_connections`**

```c
size_t mcp_async_server_active_connections(const mcp_async_server_t *s) {
    if (s == NULL) return 0;
    // conns_count is protected by conns_mtx or atomic; using mutex for safety:
    // Actually, let's make conns_count an atomic_size_t or protect with mutex.
    // Let's use atomic_size_t conns_count for lock-free read.
}
```

- [ ] **Step 5: Update `mcp_async_server_destroy` to use `conn_detach_and_close`**

- [ ] **Step 6: Run test suite to verify no regressions**

Run: `ctest --test-dir build --output-on-failure`
Expected: All existing tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/mcpkit/transport/async_server.h src/transport/async_server.c
git commit -m "refactor(transport): implement doubly-linked connection list and deterministic detach"
```

---

## Chunk 2: Streaming HTTP State Machine & WebSocket Continuation Reassembly

### Task 3: Streaming HTTP Two-Phase Parsing & Sliding Window

**Files:**
- Modify: `src/transport/async_server.c:380-545, 575-615`

- [ ] **Step 1: Implement robust HTTP header/body streaming check and hard limits**

Add `#define MCP_ASYNC_MAX_BODY (4u << 20)` and `#define MCP_ASYNC_MAX_READ_CAP (MCP_ASYNC_MAX_BODY + 65536u)`.
Implement `parse_http_content_length` helper and two-phase check in `handle_client_read`.

- [ ] **Step 2: Implement strict in-flight serialization (`http_in_flight`) and sliding window consumption**

Ensure `conn->http_in_flight` guards dispatch, and is cleared only in `handle_client_read` write completion branch (`conn->write_off >= conn->write_len`), followed by immediate re-check of `conn->read_buf`.

- [ ] **Step 3: Run existing HTTP and async server tests**

Run: `ctest --test-dir build -R "async|http" --output-on-failure`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/transport/async_server.c
git commit -m "feat(transport): implement streaming HTTP two-phase parsing, buffer clamping, and pipelining order"
```

---

### Task 4: WebSocket Continuation Frame Reassembly

**Files:**
- Modify: `src/transport/async_server.c:615-668`

- [ ] **Step 1: Implement WebSocket continuation frame state machine**

In `handle_client_read` (`CONN_WS` branch), add buffer reassembly logic for `ws_frag_buf`, handling initial fragmented text/binary frames, continuation frames (with 4MB DoS cap and 0-length final frame support), and interleaved control frames.

- [ ] **Step 2: Build and run ws unit tests**

Run: `cmake --build build -j && ctest --test-dir build -R "ws|async" --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add src/transport/async_server.c
git commit -m "feat(transport): implement RFC 6455 WebSocket continuation frame reassembly"
```

---

## Chunk 3: Ref-Counted Session Reaper, Unit Tests, and Final Verification

### Task 5: Ref-Counted Session Reaper & Deadlock Immunity

**Files:**
- Modify: `src/transport/async_server.c:70-225, 795-922`
- Modify: `tests/unit/test_async_server.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Implement `session_entry_t` ref counting and TOCTOU-safe `find_session`**

Add `ref_count` and `dead` to `session_entry_t`. Ensure `find_session` increments ref inside lock, and `worker_dispatch_fn` decrements ref upon task completion.

- [ ] **Step 2: Implement lock-free batch session reaper (`reap_expired_sessions`)**

Implement 5-second interval timer check in `async_server_step`, collecting up to 32 expired sessions in a local stack array, unlocking `sessions_mtx`, and safely detaching SSE connections outside the lock to prevent deadlocks.

- [ ] **Step 3: Add unit tests for zero-leak, HTTP fragmentation, WS continuation, and session expiration**

Modify `tests/unit/test_async_server.c` and register in `tests/CMakeLists.txt`.

- [ ] **Step 4: Run full test suite under ASan / UBSan**

Run:
```cmake
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```
Expected: 100% tests passed, zero memory leaks, zero sanitizer complaints.

- [ ] **Step 5: Commit**

```bash
git add src/transport/async_server.c tests/unit/test_async_server.c tests/CMakeLists.txt
git commit -m "feat(transport): implement ref-counted session reaper, deadlock immunity, and async unit tests"
```
