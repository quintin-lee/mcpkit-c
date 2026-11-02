# 事件反应器 Reactor 跨平台 POSIX `poll()` 降级实现计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a fully compliant POSIX `poll()` fallback backend for `mcp_reactor_t` in `src/runtime/reactor.c`, enabling seamless cross-platform build and execution on macOS and BSD systems while maintaining 100% semantic and behavioral symmetry with the Linux `epoll` engine.

**Architecture:**
- **Task 1: CMake Configuration & Macros** — Add `MCPKIT_REACTOR_FORCE_POLL` option in `cmake/MCPKitOptions.cmake` and wire `MCP_REACTOR_FORCE_POLL` definition in root `CMakeLists.txt`.
- **Task 2: Reentrant Sibling & Reallocation Unit Tests** — Implement `test_reentrant_remove_sibling` (deterministic fd ordering) and `test_reentrant_add_within_cb` (filling to capacity to trigger reentrant reallocation) in `tests/unit/test_reactor.c` to guarantee snapshot isolation.
- **Task 3: POSIX `poll()` Backend & Cross-Platform Wakeup** — Implement conditional headers (decoupled `<sys/eventfd.h>`), safe non-Linux self-pipe wakeup, complete lifecycle functions (`create`, `add`, `modify`, `remove`, `destroy`), symmetric event mapping, Slot 0 wakeup drain, and dynamic snapshot dispatch in `src/runtime/reactor.c`.
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

In root `CMakeLists.txt` immediately after `include(cmake/MCPKitOptions.cmake)` (line 31):
```cmake
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

Add test function verifying that adding a new descriptor that forces buffer reallocation (`capacity` threshold) from within a callback executes safely without memory corruption or dangling pointer access:
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
    (void)events;
    char b;
    CHECK(read(fd, &b, 1) == 1);
    add_test_ctx_t *tc = (add_test_ctx_t *)user;
    CHECK(mcp_reactor_add(ctx, tc->r, tc->new_fd_to_add, MCP_EVENT_READ, on_dynamic_read, tc) == MCP_OK);
}

static void test_reentrant_add_within_cb(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    /* Fill reactor to initial capacity (16 descriptors) so next add triggers rt_realloc */
    int fill_sv[15][2];
    for (int i = 0; i < 15; i++) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fill_sv[i]) == 0);
        CHECK(mcp_reactor_add(ctx, r, fill_sv[i][0], MCP_EVENT_READ, dummy_cb, NULL) == MCP_OK);
    }

    int sv_trigger[2], sv_overflow[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_trigger) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_overflow) == 0);

    add_test_ctx_t tc = {
        .ctx = ctx,
        .r = r,
        .new_fd_to_add = sv_overflow[0],
        .new_cb_fired = false
    };

    /* 16th descriptor: capacity reaches 16 */
    CHECK(mcp_reactor_add(ctx, r, sv_trigger[0], MCP_EVENT_READ, on_trigger_add, &tc) == MCP_OK);

    char byte = 'a';
    CHECK(write(sv_trigger[1], &byte, 1) == 1);
    CHECK(write(sv_overflow[1], &byte, 1) == 1);

    /* First step: on_trigger_add fires and dynamically adds 17th fd (sv_overflow[0]), triggering realloc */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);

    /* Second step: sv_overflow[0] should now be active and fire */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(tc.new_cb_fired == true);

    for (int i = 0; i < 15; i++) {
        close(fill_sv[i][0]); close(fill_sv[i][1]);
    }
    close(sv_trigger[0]); close(sv_trigger[1]);
    close(sv_overflow[0]); close(sv_overflow[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}
```

- [ ] **Step 3: Register test functions in `main()` of `test_reactor.c` and verify**

Register `test_reentrant_remove_sibling()` and `test_reentrant_add_within_cb()` in `test_reactor.c:main()`:
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
#define _GNU_SOURCE

#include "mcpkit/runtime/reactor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

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

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/shutdown.h"
#include "internals.h"
```

- [ ] **Step 2: Update `struct mcp_reactor` for poll backend**

In `src/runtime/reactor.c`:
```c
typedef struct {
    int fd;
    mcp_event_mask_t events;
    mcp_reactor_cb_t cb;
    void *user;
} mcp_reactor_entry_t;

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

- [ ] **Step 3: Implement cross-platform self-pipe wakeup and `mcp_reactor_create`**

In `src/runtime/reactor.c`:
```c
mcp_reactor_t *mcp_reactor_create(mcp_context_t *ctx) {
#if defined(MCP_REACTOR_BACKEND_EPOLL)
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        return NULL;
    }
#endif

    int w_read = -1;
    int w_write = -1;

#if defined(EFD_CLOEXEC) && defined(EFD_NONBLOCK)
    w_read = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif
    if (w_read < 0) {
        int pipe_fds[2];
#if defined(__linux__) && defined(_GNU_SOURCE)
        if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            w_read = pipe_fds[0];
            w_write = pipe_fds[1];
        }
#endif
        if (w_read < 0) {
            if (pipe(pipe_fds) == 0) {
                (void)fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
                (void)fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
                int flags0 = fcntl(pipe_fds[0], F_GETFL, 0);
                (void)fcntl(pipe_fds[0], F_SETFL, flags0 | O_NONBLOCK);
                int flags1 = fcntl(pipe_fds[1], F_GETFL, 0);
                (void)fcntl(pipe_fds[1], F_SETFL, flags1 | O_NONBLOCK);
                w_read = pipe_fds[0];
                w_write = pipe_fds[1];
            }
        }
    }

    if (w_read < 0) {
#if defined(MCP_REACTOR_BACKEND_EPOLL)
        close(epfd);
#endif
        return NULL;
    }

#if defined(MCP_REACTOR_BACKEND_EPOLL)
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = w_read;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, w_read, &ev) != 0) {
        close(w_read);
        if (w_write >= 0) close(w_write);
        close(epfd);
        return NULL;
    }
#endif

    mcp_reactor_t *r = rt_malloc(ctx, sizeof(*r));
    if (r == NULL) {
        close(w_read);
        if (w_write >= 0) close(w_write);
#if defined(MCP_REACTOR_BACKEND_EPOLL)
        close(epfd);
#endif
        return NULL;
    }

#if defined(MCP_REACTOR_BACKEND_EPOLL)
    r->epoll_fd = epfd;
#else
    r->poll_fds = rt_malloc(ctx, 1 * sizeof(struct pollfd));
    if (r->poll_fds == NULL) {
        close(w_read);
        if (w_write >= 0) close(w_write);
        rt_free(ctx, r);
        return NULL;
    }
    r->poll_fds[0].fd = w_read;
    r->poll_fds[0].events = POLLIN;
    r->poll_fds[0].revents = 0;
    r->snapshots = NULL;
#endif

    r->wakeup_fd = w_read;
    r->wakeup_write_fd = w_write;
    atomic_init(&r->stop_requested, false);
    r->entries = NULL;
    r->count = 0;
    r->capacity = 0;

    return r;
}
```

- [ ] **Step 4: Implement `mcp_reactor_destroy` for poll**

```c
void mcp_reactor_destroy(mcp_context_t *ctx, mcp_reactor_t *r) {
    if (r == NULL) {
        return;
    }
#if defined(MCP_REACTOR_BACKEND_EPOLL)
    if (r->epoll_fd >= 0) {
        close(r->epoll_fd);
    }
#else
    if (r->poll_fds != NULL) {
        rt_free(ctx, r->poll_fds);
    }
    if (r->snapshots != NULL) {
        rt_free(ctx, r->snapshots);
    }
#endif
    if (r->wakeup_fd >= 0) {
        close(r->wakeup_fd);
    }
    if (r->wakeup_write_fd >= 0) {
        close(r->wakeup_write_fd);
    }
    if (r->entries != NULL) {
        rt_free(ctx, r->entries);
    }
    rt_free(ctx, r);
}
```

- [ ] **Step 5: Implement event mapping helpers and transactional `add`, `modify`, `remove` for poll**

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

mcp_status_t mcp_reactor_add(mcp_context_t *ctx, mcp_reactor_t *r, int fd,
                             mcp_event_mask_t events, mcp_reactor_cb_t cb, void *user) {
    if (r == NULL || fd < 0 || cb == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if ((events & ~(MCP_EVENT_READ | MCP_EVENT_WRITE | MCP_EVENT_ERROR)) != 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (fd == r->wakeup_fd || (r->wakeup_write_fd >= 0 && fd == r->wakeup_write_fd)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    size_t ins = 0;
    if (find_entry_idx(r, fd, &ins) >= 0) {
        return MCP_ERR_ALREADY_EXISTS;
    }

    if (r->count == r->capacity) {
        size_t new_cap = r->capacity == 0 ? 16 : r->capacity * 2;
#if defined(MCP_REACTOR_BACKEND_EPOLL)
        mcp_reactor_entry_t *new_entries =
            rt_realloc(ctx, r->entries, new_cap * sizeof(*new_entries));
        if (new_entries == NULL) {
            return MCP_ERR_NOMEM;
        }
        r->entries = new_entries;
        r->capacity = new_cap;
#else
        struct pollfd *new_poll_fds =
            rt_realloc(ctx, r->poll_fds, (new_cap + 1) * sizeof(*new_poll_fds));
        if (new_poll_fds == NULL) {
            return MCP_ERR_NOMEM;
        }
        poll_event_snapshot_t *new_snapshots =
            rt_realloc(ctx, r->snapshots, new_cap * sizeof(*new_snapshots));
        if (new_snapshots == NULL) {
            r->poll_fds = new_poll_fds;
            return MCP_ERR_NOMEM;
        }
        mcp_reactor_entry_t *new_entries =
            rt_realloc(ctx, r->entries, new_cap * sizeof(*new_entries));
        if (new_entries == NULL) {
            r->poll_fds = new_poll_fds;
            r->snapshots = new_snapshots;
            return MCP_ERR_NOMEM;
        }
        r->poll_fds = new_poll_fds;
        r->snapshots = new_snapshots;
        r->entries = new_entries;
        r->capacity = new_cap;
#endif
    }

#if defined(MCP_REACTOR_BACKEND_EPOLL)
    uint32_t epoll_ev = 0;
    if (events & MCP_EVENT_READ) {
        epoll_ev |= (uint32_t)(EPOLLIN | EPOLLRDHUP);
    }
    if (events & MCP_EVENT_WRITE) {
        epoll_ev |= (uint32_t)EPOLLOUT;
    }
    if (events & MCP_EVENT_ERROR) {
        epoll_ev |= (uint32_t)(EPOLLERR | EPOLLHUP);
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = epoll_ev;
    ev.data.fd = fd;
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        return MCP_ERR_IO;
    }
#else
    if (ins < r->count) {
        memmove(&r->poll_fds[ins + 2], &r->poll_fds[ins + 1],
                (r->count - ins) * sizeof(struct pollfd));
    }
    r->poll_fds[ins + 1].fd = fd;
    r->poll_fds[ins + 1].events = map_to_poll_events(events);
    r->poll_fds[ins + 1].revents = 0;
#endif

    if (ins < r->count) {
        memmove(&r->entries[ins + 1], &r->entries[ins],
                (r->count - ins) * sizeof(mcp_reactor_entry_t));
    }
    r->entries[ins].fd = fd;
    r->entries[ins].events = events;
    r->entries[ins].cb = cb;
    r->entries[ins].user = user;
    r->count++;

    return MCP_OK;
}

mcp_status_t mcp_reactor_modify(mcp_context_t *ctx, mcp_reactor_t *r, int fd,
                                mcp_event_mask_t events) {
    (void)ctx;
    if (r == NULL || fd < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if ((events & ~(MCP_EVENT_READ | MCP_EVENT_WRITE | MCP_EVENT_ERROR)) != 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    size_t idx = 0;
    if (find_entry_idx(r, fd, &idx) < 0) {
        return MCP_ERR_NOT_FOUND;
    }

#if defined(MCP_REACTOR_BACKEND_EPOLL)
    uint32_t epoll_ev = 0;
    if (events & MCP_EVENT_READ) {
        epoll_ev |= (uint32_t)(EPOLLIN | EPOLLRDHUP);
    }
    if (events & MCP_EVENT_WRITE) {
        epoll_ev |= (uint32_t)EPOLLOUT;
    }
    if (events & MCP_EVENT_ERROR) {
        epoll_ev |= (uint32_t)(EPOLLERR | EPOLLHUP);
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = epoll_ev;
    ev.data.fd = fd;
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        return MCP_ERR_IO;
    }
#else
    r->poll_fds[idx + 1].events = map_to_poll_events(events);
#endif

    r->entries[idx].events = events;
    return MCP_OK;
}

mcp_status_t mcp_reactor_remove(mcp_context_t *ctx, mcp_reactor_t *r, int fd) {
    (void)ctx;
    if (r == NULL || fd < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    size_t idx = 0;
    if (find_entry_idx(r, fd, &idx) < 0) {
        return MCP_ERR_NOT_FOUND;
    }

#if defined(MCP_REACTOR_BACKEND_EPOLL)
    (void)epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
#else
    if (idx + 1 < r->count) {
        memmove(&r->poll_fds[idx + 1], &r->poll_fds[idx + 2],
                (r->count - idx - 1) * sizeof(struct pollfd));
    }
#endif

    if (idx + 1 < r->count) {
        memmove(&r->entries[idx], &r->entries[idx + 1],
                (r->count - idx - 1) * sizeof(mcp_reactor_entry_t));
    }
    r->count--;

    return MCP_OK;
}
```

- [ ] **Step 6: Implement `mcp_reactor_step` with timeout normalization and dynamic snapshot dispatch**

```c
mcp_status_t mcp_reactor_step(mcp_context_t *ctx, mcp_reactor_t *r, int timeout_ms) {
    if (r == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_shutdown_requested()) {
        return MCP_ERR_CANCELLED;
    }

#if defined(MCP_REACTOR_BACKEND_EPOLL)
    struct epoll_event evs[64];
    int n = epoll_wait(r->epoll_fd, evs, 64, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) {
            return MCP_OK;
        }
        return MCP_ERR_IO;
    }

    for (int i = 0; i < n; i++) {
        if (atomic_load(&r->stop_requested) || mcp_shutdown_requested()) {
            break;
        }

        int fd = evs[i].data.fd;
        if (fd == r->wakeup_fd) {
            drain_wakeup(r);
            continue;
        }

        size_t idx = 0;
        if (find_entry_idx(r, fd, &idx) < 0) {
            /* fd was removed by a prior callback in this batch */
            continue;
        }

        mcp_reactor_entry_t entry = r->entries[idx];
        mcp_event_mask_t revents = 0;

        if (evs[i].events & (uint32_t)(EPOLLIN | EPOLLRDHUP)) {
            if (entry.events & MCP_EVENT_READ) {
                revents |= MCP_EVENT_READ;
            }
        }
        if (evs[i].events & (uint32_t)EPOLLOUT) {
            if (entry.events & MCP_EVENT_WRITE) {
                revents |= MCP_EVENT_WRITE;
            }
        }
        if (evs[i].events & (uint32_t)(EPOLLERR | EPOLLHUP)) {
            if (entry.events & MCP_EVENT_ERROR) {
                revents |= MCP_EVENT_ERROR;
            } else if (entry.events & MCP_EVENT_READ) {
                revents |= MCP_EVENT_READ;
            }
        }

        if (revents != 0) {
            entry.cb(ctx, fd, revents, entry.user);
        }
    }
#else
    int timeout = timeout_ms < 0 ? -1 : timeout_ms;
    int n = poll(r->poll_fds, (nfds_t)(r->count + 1), timeout);
    if (n < 0) {
        if (errno == EINTR) {
            return MCP_OK;
        }
        return MCP_ERR_IO;
    }
    if (n == 0) {
        if (mcp_shutdown_requested()) {
            return MCP_ERR_CANCELLED;
        }
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
            /* Target fd was removed by a prior callback in this batch */
            continue;
        }

        mcp_reactor_entry_t entry = r->entries[cur_idx];
        mcp_event_mask_t mask = map_from_poll_revents(fired_rev, entry.events);
        if (mask != 0) {
            entry.cb(ctx, target_fd, mask, entry.user);
        }
    }
#endif

    if (mcp_shutdown_requested()) {
        return MCP_ERR_CANCELLED;
    }

    return MCP_OK;
}
```

- [ ] **Step 7: Build and test both epoll and poll**

Run:
```bash
cmake -S . -B build -DMCPKIT_REACTOR_FORCE_POLL=OFF && cmake --build build -j && ctest --test-dir build -R "test_reactor" --output-on-failure
cmake -S . -B build-poll -DMCPKIT_REACTOR_FORCE_POLL=ON && cmake --build build-poll -j && ctest --test-dir build-poll -R "test_reactor" --output-on-failure
```
Expected: All `test_reactor` cases pass 100% under both backends without warnings.

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
