# P0-1 Multi-Connection Socket Serve Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `mcp_socket_serve` — a library accept-loop + threadpool-per-connection multi-connection TCP serve function.

**Architecture:** The accept loop runs on the calling thread. Each accepted connection fd is wrapped in a client-mode transport and submitted to a caller-supplied executor. Worker tasks run a single-connection serve loop (create session → recv/parse/dispatch/send → destroy session → stop/destroy transport). The accept loop blocks on `poll` with a 500 ms timeout, checks `mcp_shutdown_requested()` on each wakeup, and breaks when set. `mcp_executor_wait` drains in-flight connections before the function returns.

**Tech Stack:** C23, POSIX sockets, C11 atomics, pthread mutex, mcpkit_core static lib.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/server/internals.h` | Add `pthread_mutex_t subscribed_lock` to `struct mcp_server`; change `log_floor` to `atomic_int` |
| `src/server/server.c` | Init/destroy the new mutex; init `log_floor` atomic |
| `src/server/dispatcher.c` | Use `atomic_store/atomic_load` for `log_floor`; lock `subscribed_uris` access in `route_advanced` |
| `src/transport/socket.c` | Add `socket_transport_from_fd` helper; add `mcp_socket_serve` impl |
| `include/mcpkit/transport/socket.h` | Declare `mcp_socket_serve` + Doxygen |
| `tests/unit/test_socket_serve.c` | New: two concurrent clients + shutdown drain proof |
| `tests/CMakeLists.txt` | Register `test_socket_serve` under `MCPKIT_BUILD_SOCKET` |
| `docs/module-reference.md` | Add `mcp_socket_serve` to socket.h section |
| `CHANGELOG.md` | Add bullet to [Unreleased] Added |

---

## Task 1: Concurrency state fix (log_floor atomic + subscribed_lock)

**Files:**
- Modify: `src/server/internals.h` (add pthread include + mutex field + atomic log_floor)
- Modify: `src/server/server.c` (init/destroy mutex + atomic init)
- Modify: `src/server/dispatcher.c` (atomic ops + lock in route_advanced)

- [ ] **Step 1: Update internals.h**

Add `#include <pthread.h>` and `#include <stdatomic.h>` (already present). Change:
```c
mcp_log_level_t log_floor;  →  atomic_int log_floor;
```
Add to `struct mcp_server`:
```c
pthread_mutex_t subscribed_lock;
```

- [ ] **Step 2: Update server.c create/destroy**

In `mcp_server_create`: after `atomic_init` block, add:
```c
atomic_init(&srv->log_floor, MCP_LOG_DEBUG);
pthread_mutex_init(&srv->subscribed_lock, NULL);
```
In `mcp_server_destroy` (before `srv_free(ctx, srv)`):
```c
pthread_mutex_destroy(&srv->subscribed_lock);
```

- [ ] **Step 3: Update dispatcher.c dlogf_srv + route_advanced**

`dlogf_srv`: change `srv->log_floor` read to `atomic_load(&srv->log_floor)`:
```c
if (level < (mcp_log_level_t)atomic_load(&srv->log_floor)) return;
```

`route_advanced` logging/setLevel write:
```c
atomic_store(&srv->log_floor, (int)mapped);
```

`route_advanced` resources/subscribe/unsubscribe: wrap the entire body (both branches) in:
```c
pthread_mutex_lock(&srv->subscribed_lock);
... existing code ...
pthread_mutex_unlock(&srv->subscribed_lock);
```
Note: the function returns early on NOMEM paths — the `goto` or early-return must unlock first. Use a `cleanup:` label or wrap the mutable operations in a helper.

- [ ] **Step 4: Build + verify 45/45 + commit**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j4 && ctest --test-dir build`
Expected: 45/45 green.
Commit: `git add src/server/internals.h src/server/server.c src/server/dispatcher.c && git commit -m "fix(server): atomic log_floor + subscribed_lock for concurrent dispatch"`

---

## Task 2: mcp_socket_serve implementation

**Files:**
- Modify: `include/mcpkit/transport/socket.h` (declare mcp_socket_serve)
- Modify: `src/transport/socket.c` (add socket_transport_from_fd + mcp_socket_serve)

- [ ] **Step 1: Add socket_transport_from_fd helper to socket.c**

Static helper that wraps an already-connected fd in a client-mode transport (no connect, no start needed):
```c
static mcp_transport_t *socket_transport_from_fd(mcp_context_t *ctx, int fd) {
    const mcp_allocator_t *a = alloc_of(ctx);
    socket_backend_t *b = a->malloc_fn(sizeof(*b), a->userdata);
    if (b == NULL) return NULL;
    b->fd = fd;
    b->server_mode = false;  // already-connected; sock_start is a no-op
    mcp_transport_t *t = mcp_transport_create(ctx, &kSocketOps, b);
    if (t == NULL) { a->free_fn(b, a->userdata); close(fd); }
    return t;
}
```

- [ ] **Step 2: Add serve_one_conn task + mcp_socket_serve**

```c
typedef struct {
    mcp_context_t *ctx;
    mcp_server_t  *server;
    int            cfd;
} conn_job_t;

static void serve_one_conn(mcp_context_t *ctx, void *arg) {
    conn_job_t *job = (conn_job_t *)arg;
    mcp_transport_t *t = socket_transport_from_fd(ctx, job->cfd);
    if (t == NULL) return;
    mcp_session_t *sess = mcp_server_create_session(ctx, job->server);
    if (sess == NULL) { mcp_transport_destroy(ctx, t); return; }
    // Run serve loop (same shape as loop_run_impl but no timer/executor)
    ... recv/parse/dispatch/send until MCP_ERR_IO ...
    mcp_server_destroy_session(ctx, job->server, sess);
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
}
```

```c
mcp_status_t mcp_socket_serve(mcp_context_t *ctx, mcp_server_t *server,
                              uint16_t port, mcp_executor_t *pool) {
    // Validate args
    int lfd = open_socket(NULL, port, true);
    if (lfd < 0) return MCP_ERR_NOMEM;
    // Accept loop
    for (;;) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN, .revents = 0 };
        int n = poll(&pfd, 1, 500);
        if (n < 0 && errno != EINTR) { close(lfd); return MCP_ERR_IO; }
        if (mcp_shutdown_requested()) break;
        if (n == 0) continue;
        struct sockaddr_in client_addr; socklen_t slen = sizeof(client_addr);
        int cfd;
        do { cfd = accept(lfd, (struct sockaddr *)&client_addr, &slen); }
        while (cfd < 0 && errno == EINTR);
        if (cfd < 0) continue;
        conn_job_t *job = /* alloc */;
        job->ctx = ctx; job->server = server; job->cfd = cfd;
        mcp_status_t st = mcp_executor_submit(ctx, pool, serve_one_conn, job);
        if (st != MCP_OK) { close(cfd); free(job); }
    }
    close(lfd);
    mcp_executor_wait(ctx, pool);
    return MCP_OK;
}
```

- [ ] **Step 3: Declare in socket.h**

```c
mcp_status_t mcp_socket_serve(mcp_context_t *ctx, mcp_server_t *server,
                              uint16_t port, mcp_executor_t *pool);
```
With full Doxygen @brief/@param/@return (P3 public API contract).

- [ ] **Step 4: Build + verify 45/45 + commit**

Expected: 45/45 green (no new test yet, just new API).
Commit: `git add include/mcpkit/transport/socket.h src/transport/socket.c && git commit -m "feat(transport): add mcp_socket_serve multi-connection accept-loop serve"`

---

## Task 3: test_socket_serve.c

**Files:**
- Create: `tests/unit/test_socket_serve.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test**

Port 45680 (fixed). Test structure:
1. Create server with echo tool
2. Create threadpool (2 workers)
3. Launch `mcp_socket_serve` on a background thread
4. Connect two clients concurrently via `mcp_socket_transport_create(ctx, "127.0.0.1", 45680, false)`
5. Each client: init + tools/call echo → verify response
6. Each client: resources/subscribe with different URIs concurrently → both succeed
7. Call `mcp_request_shutdown()`
8. `mcp_socket_serve` returns MCP_OK
9. `mcp_executor_wait` already called inside; verify pool drained
10. Clean up: destroy pool, server, clients, transports

- [ ] **Step 2: Register in CMakeLists.txt**

Under `MCPKIT_BUILD_SOCKET`:
```cmake
add_executable(test_socket_serve unit/test_socket_serve.c)
target_link_libraries(test_socket_serve PRIVATE mcpkit_core)
add_test(NAME test_socket_serve COMMAND test_socket_serve)
```

- [ ] **Step 3: Build + verify 46/46 + commit**

Expected: 46/46 green.
Commit: `git add tests/unit/test_socket_serve.c tests/CMakeLists.txt && git commit -m "test(socket): prove mcp_socket_serve multi-connection dispatch and shutdown drain"`

---

## Task 4: Docs + CHANGELOG + triple gates

**Files:**
- Modify: `docs/module-reference.md` (socket.h section)
- Modify: `CHANGELOG.md` ([Unreleased] Added)

- [ ] **Step 1: Update module-reference.md**

Add to socket.h section:
```
mcp_status_t mcp_socket_serve(ctx, mcp_server_t *, uint16_t port, mcp_executor_t *pool);
/* Accept loop on calling thread; each connection submitted to pool.
 * Blocks until mcp_shutdown_requested(); drains pool before returning. */
```

- [ ] **Step 2: Update CHANGELOG.md**

Add bullet:
```
- `mcp_socket_serve`: multi-connection TCP accept-loop; each accepted
  connection is served on a threadpool worker. Blocks until
  `mcp_shutdown_requested()`; drains in-flight connections before
  returning.
```

- [ ] **Step 3: Doxygen verify**

Run: `doxygen Doxyfile 2>&1 | grep -v '^Parsing\|^Preprocessing\|^Generating' | grep -iE 'warning|error'`
Expected: 0 real warnings.

- [ ] **Step 4: Triple gates from clean configures**

```
rm -rf build build-clang build-asan
cmake -S . -B build      -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j4 && ctest --test-dir build   # 46/46
cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build-clang -j4 && ctest --test-dir build-clang  # 46/46
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build-asan -j4 && ctest --test-dir build-asan  # 46/46
ldd build-asan/tests/test_socket_serve | grep -E 'asan|ubsan'
rm -rf build build-clang build-asan
git status --porcelain  # expect clean
```

- [ ] **Step 5: Commit docs**

`git add docs/module-reference.md CHANGELOG.md && git commit -m "docs: cover mcp_socket_serve in CHANGELOG and module reference"`

---

## Comment-justifications (P3)

| Comment | Justification |
|---|---|
| socket.c `socket_transport_from_fd` @file note | Documents why `server_mode=false` means "already-connected, skip connect" — non-obvious reuse of the client-mode flag |
| socket.c `mcp_socket_serve` poll timeout 500 ms comment | Explains why poll is used instead of blocking accept (shutdown responsiveness) |
| internals.h `subscribed_lock` field doc | Documents the invariant: lock held during all subscribed_uris mutations |
| dispatcher.c `route_advanced` lock comment | Explains why the lock is per-operation, not held across the whole route_advanced call |
