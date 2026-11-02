# 事件反应器 Reactor 跨平台 POSIX `poll()` 降级实现计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a fully compliant POSIX `poll()` fallback backend for `mcp_reactor_t` in `src/runtime/reactor.c`, enabling seamless cross-platform build and execution on macOS and BSD systems while maintaining 100% semantic and behavioral symmetry with the Linux `epoll` engine.

**Architecture:**
- **Task 1: CMake Configuration & Macros** — Add `MCPKIT_REACTOR_FORCE_POLL` option in `cmake/MCPKitOptions.cmake` and wire `MCP_REACTOR_FORCE_POLL` definition in root `CMakeLists.txt`.
- **Task 2: Reentrant Sibling & Reallocation Unit Tests** — Implement `test_reentrant_remove_sibling` (deterministic fd ordering) and `test_reentrant_add_within_cb` in `tests/unit/test_reactor.c` to guarantee snapshot isolation.
- **Task 3: POSIX `poll()` Backend & Cross-Platform Wakeup** — Implement conditional headers (decoupled `<sys/eventfd.h>`), safe non-Linux self-pipe wakeup, transactional memory reallocation, symmetric event mapping, Slot 0 wakeup drain, and dynamic snapshot dispatch in `src/runtime/reactor.c`.
- **Task 4: Dual-Engine Matrix Regression & Quality Gates** — Run full CTest suite under default epoll, under forced poll (`-DMCPKIT_REACTOR_FORCE_POLL=ON`), and with ASan / UBSan under `-Wall -Wextra` zero-warning gate.

**Tech Stack:** C23 (`-std=c2x`), POSIX `poll()`, `pipe()`, `fcntl()`, CMake / CTest.

---

## Chunk 1: CMake Configuration, Test Fixtures, and Core Poll Engine Implementation

### Task 1: CMake Configuration & Macros

**Files:**
- Modify: `cmake/MCPKitOptions.cmake:10-15`
- Modify: `CMakeLists.txt:27-32`

- [ ] **Step 1: Add `MCPKIT_REACTOR_FORCE_POLL` option to `cmake/MCPKitOptions.cmake`**

In `cmake/MCPKitOptions.cmake`, add:
```cmake
option(MCPKIT_REACTOR_FORCE_POLL "Force POSIX poll backend in reactor for testing" OFF)
```

- [ ] **Step 2: Add compile definition in `CMakeLists.txt`**

In root `CMakeLists.txt` immediately after `include(cmake/MCPKitOptions.cmake)`:
```cmake
include(cmake/MCPKitOptions.cmake)
if(MCPKIT_REACTOR_FORCE_POLL)
  add_compile_definitions(MCP_REACTOR_FORCE_POLL)
endif()
```

- [ ] **Step 3: Verify CMake configuration passes**

Run: `cmake -S . -B build -DMCPKIT_REACTOR_FORCE_POLL=ON && cmake -S . -B build -DMCPKIT_REACTOR_FORCE_POLL=OFF`
Expected: Both configurations succeed.

- [ ] **Step 4: Commit**

```bash
git add cmake/MCPKitOptions.cmake CMakeLists.txt
git commit -m "build: add MCPKIT_REACTOR_FORCE_POLL option for dual-engine verification"
```

---

### Task 2: Reentrant Sibling & Reallocation Unit Tests

**Files:**
- Modify: `tests/unit/test_reactor.c`

- [ ] **Step 1: Add `test_reentrant_remove_sibling` to `tests/unit/test_reactor.c`**

Add test function verifying that removing a sibling descriptor within a callback prevents that sibling's callback from firing in the same iteration, using deterministic fd ordering:
```c
typedef struct {
    mcp_context_t *ctx;
    mcp_reactor_t *r;
    int target_to_remove;
    bool cb1_fired;
    bool cb2_fired;
} sibling_test_ctx_t;

static void on_first_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    sibling_test_ctx_t *tc = (sibling_test_ctx_t *)user;
    tc->cb1_fired = true;
    mcp_reactor_remove(tc->ctx, tc->r, tc->target_to_remove);
}

static void on_second_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    sibling_test_ctx_t *tc = (sibling_test_ctx_t *)user;
    tc->cb2_fired = true;
}

static void test_reentrant_remove_sibling(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    int sv1[2], sv2[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv1) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);

    /* Determine lower and higher fd numbers to guarantee dispatch order */
    int first_fd = (sv1[0] < sv2[0]) ? sv1[0] : sv2[0];
    int first_peer = (sv1[0] < sv2[0]) ? sv1[1] : sv2[1];
    int second_fd = (sv1[0] < sv2[0]) ? sv2[0] : sv1[0];
    int second_peer = (sv1[0] < sv2[0]) ? sv2[1] : sv1[1];

    sibling_test_ctx_t tc = {
        .ctx = ctx,
        .r = r,
        .target_to_remove = second_fd,
        .cb1_fired = false,
        .cb2_fired = false
    };

    CHECK(mcp_reactor_add(ctx, r, first_fd, MCP_EVENT_READ, on_first_read, &tc) == MCP_OK);
    CHECK(mcp_reactor_add(ctx, r, second_fd, MCP_EVENT_READ, on_second_read, &tc) == MCP_OK);

    /* Write to first_peer then second_peer so both are ready */
    char byte = 'x';
    CHECK(write(first_peer, &byte, 1) == 1);
    CHECK(write(second_peer, &byte, 1) == 1);

    /* Step reactor: first_fd fires and removes second_fd; second_fd must NOT fire */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(tc.cb1_fired == true);
    CHECK(tc.cb2_fired == false);

    close(sv1[0]); close(sv1[1]);
    close(sv2[0]); close(sv2[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}
```

- [ ] **Step 2: Add `test_reentrant_add_within_cb` to `tests/unit/test_reactor.c`**

Add test function verifying that adding a new descriptor (triggering dynamic snapshot buffer expansion) from within a callback executes safely without memory corruption or dangling pointer access:
```c
typedef struct {
    mcp_context_t *ctx;
    mcp_reactor_t *r;
    int new_fd_to_add;
    bool new_cb_fired;
} add_test_ctx_t;

static void on_dynamic_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    add_test_ctx_t *tc = (add_test_ctx_t *)user;
    tc->new_cb_fired = true;
}

static void on_trigger_add(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    add_test_ctx_t *tc = (add_test_ctx_t *)user;
    mcp_reactor_add(tc->ctx, tc->r, tc->new_fd_to_add, MCP_EVENT_READ, on_dynamic_read, tc);
}

static void test_reentrant_add_within_cb(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    int sv_base[2], sv_new[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_base) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_new) == 0);

    add_test_ctx_t tc = {
        .ctx = ctx,
        .r = r,
        .new_fd_to_add = sv_new[0],
        .new_cb_fired = false
    };

    CHECK(mcp_reactor_add(ctx, r, sv_base[0], MCP_EVENT_READ, on_trigger_add, &tc) == MCP_OK);

    char byte = 'a';
    CHECK(write(sv_base[1], &byte, 1) == 1);
    CHECK(write(sv_new[1], &byte, 1) == 1);

    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);

    /* Second step: new_fd_to_add should now be monitored and fire */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(tc.new_cb_fired == true);

    close(sv_base[0]); close(sv_base[1]);
    close(sv_new[0]); close(sv_new[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}
```

- [ ] **Step 3: Register test functions in `main()` of `test_reactor.c` and verify**

Register and run:
`cmake --build build -j && ctest --test-dir build -R "test_reactor" --output-on-failure`
Expected: Existing tests plus new tests pass under epoll.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_reactor.c
git commit -m "test(reactor): add reentrant remove sibling and dynamic add test cases"
```

---

### Task 3: POSIX `poll()` Backend & Cross-Platform Wakeup

**Files:**
- Modify: `src/runtime/reactor.c`

- [ ] **Step 1: Implement conditional headers and decouple `<sys/eventfd.h>`**

In `src/runtime/reactor.c`:
```c
#if defined(__linux__) && __has_include(<sys/epoll.h>) && !defined(MCP_REACTOR_FORCE_POLL)
#define MCP_REACTOR_BACKEND_EPOLL 1
#include <sys/epoll.h>
#else
#define MCP_REACTOR_BACKEND_POLL 1
#include <poll.h>
#endif

#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#endif
```

- [ ] **Step 2: Update `struct mcp_reactor` for poll backend**

In `src/runtime/reactor.c`:
```c
typedef struct {
    int fd;
    short revents;
} poll_event_snapshot_t;

struct mcp_reactor {
#if defined(MCP_REACTOR_BACKEND_EPOLL)
    int epoll_fd;
#else
    struct pollfd *poll_fds;              /* Capacity is capacity + 1 (Slot 0 is wakeup_fd) */
    poll_event_snapshot_t *snapshots;     /* Capacity is capacity */
#endif
    int wakeup_fd;
    int wakeup_write_fd; /* -1 when eventfd is used; pipe write end otherwise */
    atomic_bool stop_requested;
    mcp_reactor_entry_t *entries;
    size_t count;
    size_t capacity;
};
```

- [ ] **Step 3: Implement cross-platform self-pipe wakeup**

In `mcp_reactor_create`:
Ensure `pipe2` is only attempted when `defined(__linux__) && defined(_GNU_SOURCE)`, falling back cleanly to `pipe()` + `fcntl(FD_CLOEXEC, O_NONBLOCK)` on macOS and other POSIX systems.

- [ ] **Step 4: Implement event mapping helpers for poll**

```c
#if defined(MCP_REACTOR_BACKEND_POLL)
static short map_to_poll_events(mcp_event_mask_t events) {
    short ev = 0;
    if (events & MCP_EVENT_READ) {
        ev |= (short)POLLIN;
#if defined(POLLRDHUP)
        ev |= (short)POLLRDHUP;
#endif
    }
    if (events & MCP_EVENT_WRITE) {
        ev |= (short)POLLOUT;
    }
    return ev;
}

static mcp_event_mask_t map_from_poll_revents(short revents, mcp_event_mask_t registered) {
    mcp_event_mask_t mask = 0;
    if (revents & (POLLIN
#if defined(POLLRDHUP)
                   | POLLRDHUP
#endif
                  )) {
        if (registered & MCP_EVENT_READ) {
            mask |= MCP_EVENT_READ;
        }
    }
    if (revents & POLLOUT) {
        if (registered & MCP_EVENT_WRITE) {
            mask |= MCP_EVENT_WRITE;
        }
    }
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (registered & MCP_EVENT_ERROR) {
            mask |= MCP_EVENT_ERROR;
        } else if (registered & MCP_EVENT_READ) {
            mask |= MCP_EVENT_READ;
        }
    }
    return mask;
}
#endif
```

- [ ] **Step 5: Implement `mcp_reactor_create`, `add`, `modify`, `remove`, `destroy` for poll**

Implement Slot 0 allocation in `mcp_reactor_create`, transactional realloc in `mcp_reactor_add`, array shift in `mcp_reactor_remove`, and resource cleanup in `mcp_reactor_destroy`.

- [ ] **Step 6: Implement `mcp_reactor_step` for poll with dynamic snapshot dispatch**

Implement poll call with timeout normalization (`timeout_ms < 0 ? -1 : timeout_ms`), shutdown check on `n == 0`, Slot 0 wakeup drain, and safe snapshot dispatch loop:
```c
#if defined(MCP_REACTOR_BACKEND_POLL)
    int timeout = timeout_ms < 0 ? -1 : timeout_ms;
    int n = poll(r->poll_fds, (nfds_t)(r->count + 1), timeout);
    if (n < 0) {
        if (errno == EINTR) return MCP_OK;
        return MCP_ERR_IO;
    }
    if (n == 0) {
        if (mcp_shutdown_requested()) return MCP_ERR_CANCELLED;
        return MCP_OK;
    }

    if (r->poll_fds[0].revents != 0) {
        drain_wakeup(r);
    }

    size_t n_fired = 0;
    for (size_t i = 1; i <= r->count; i++) {
        if (r->poll_fds[i].revents != 0) {
            r->snapshots[n_fired].fd = r->poll_fds[i].fd;
            r->snapshots[n_fired].revents = r->poll_fds[i].revents;
            n_fired++;
        }
    }

    for (size_t i = 0; i < n_fired; i++) {
        if (atomic_load(&r->stop_requested) || mcp_shutdown_requested()) {
            break;
        }
        int target_fd = r->snapshots[i].fd;
        short fired_rev = r->snapshots[i].revents;
        size_t cur_idx = 0;
        if (find_entry_idx(r, target_fd, &cur_idx) < 0) {
            continue;
        }
        mcp_reactor_entry_t entry = r->entries[cur_idx];
        mcp_event_mask_t mask = map_from_poll_revents(fired_rev, entry.events);
        if (mask != 0) {
            entry.cb(ctx, target_fd, mask, entry.user);
        }
    }

    if (mcp_shutdown_requested()) {
        return MCP_ERR_CANCELLED;
    }
    return MCP_OK;
#endif
```

- [ ] **Step 7: Build and test both epoll and poll**

Run:
```bash
cmake -S . -B build -DMCPKIT_REACTOR_FORCE_POLL=OFF && cmake --build build -j && ctest --test-dir build -R "test_reactor" --output-on-failure
cmake -S . -B build-poll -DMCPKIT_REACTOR_FORCE_POLL=ON && cmake --build build-poll -j && ctest --test-dir build-poll -R "test_reactor" --output-on-failure
```
Expected: `test_reactor` passes 100% under both backends without warnings.

- [ ] **Step 8: Commit**

```bash
git add src/runtime/reactor.c
git commit -m "feat(runtime): implement POSIX poll fallback backend in reactor"
```

---

## Chunk 2: Dual-Engine Full Matrix Verification & Cleanup

### Task 4: Dual-Engine Matrix Regression & Quality Gates

**Files:**
- Full repository test validation

- [ ] **Step 1: Full regression test under default epoll**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_HTTP=ON
cmake --build build --clean-first -j
ctest --test-dir build --output-on-failure
```
Expected: All 69 tests pass 100%, zero warnings.

- [ ] **Step 2: Full regression test under forced poll backend**

Run:
```bash
cmake -S . -B build-poll -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_REACTOR_FORCE_POLL=ON
cmake --build build-poll --clean-first -j
ctest --test-dir build-poll --output-on-failure
```
Expected: All 69 tests pass 100% under poll backend, zero warnings.

- [ ] **Step 3: Sanitizer verification under forced poll backend**

Run:
```bash
cmake -S . -B build-poll-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_REACTOR_FORCE_POLL=ON
cmake --build build-poll-asan --clean-first -j
ctest --test-dir build-poll-asan --output-on-failure
```
Expected: All 69 tests pass, zero memory leaks, zero sanitizer errors.

- [ ] **Step 4: Clean up temporary test build folders and verify cleanliness**

Run:
```bash
rm -rf build-poll build-poll-asan
git status
```
Expected: Working tree clean.

- [ ] **Step 5: Commit and finalize**

```bash
git status
```
Expected: Working tree clean.
