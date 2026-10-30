/**
 * @file reactor.c
 * @brief Non-blocking I/O event reactor implementation.
 */

#define _GNU_SOURCE

#include "mcpkit/runtime/reactor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/shutdown.h"
#include "internals.h"

typedef struct {
    int fd;
    mcp_event_mask_t events;
    mcp_reactor_cb_t cb;
    void *user;
} mcp_reactor_entry_t;

struct mcp_reactor {
    int epoll_fd;
    int wakeup_fd;
    int wakeup_write_fd; /* -1 when eventfd is used; pipe write end otherwise */
    atomic_bool stop_requested;
    mcp_reactor_entry_t *entries;
    size_t count;
    size_t capacity;
};

static ssize_t find_entry_idx(const mcp_reactor_t *r, int fd, size_t *insert_idx) {
    size_t low = 0;
    size_t high = r->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (r->entries[mid].fd == fd) {
            if (insert_idx != NULL) {
                *insert_idx = mid;
            }
            return (ssize_t)mid;
        }
        if (r->entries[mid].fd < fd) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (insert_idx != NULL) {
        *insert_idx = low;
    }
    return -1;
}

static void drain_wakeup(mcp_reactor_t *r) {
    if (r->wakeup_write_fd >= 0) {
        char buf[64];
        while (read(r->wakeup_fd, buf, sizeof(buf)) > 0) {
        }
    } else {
        uint64_t val = 0;
        while (read(r->wakeup_fd, &val, sizeof(val)) > 0) {
        }
    }
}

mcp_reactor_t *mcp_reactor_create(mcp_context_t *ctx) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        return NULL;
    }

    int w_read = -1;
    int w_write = -1;

#if defined(EFD_CLOEXEC) && defined(EFD_NONBLOCK)
    w_read = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif
    if (w_read < 0) {
        int pipe_fds[2];
#if defined(_GNU_SOURCE)
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
        close(epfd);
        return NULL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = w_read;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, w_read, &ev) != 0) {
        close(w_read);
        if (w_write >= 0) {
            close(w_write);
        }
        close(epfd);
        return NULL;
    }

    mcp_reactor_t *r = rt_malloc(ctx, sizeof(*r));
    if (r == NULL) {
        close(w_read);
        if (w_write >= 0) {
            close(w_write);
        }
        close(epfd);
        return NULL;
    }

    r->epoll_fd = epfd;
    r->wakeup_fd = w_read;
    r->wakeup_write_fd = w_write;
    atomic_init(&r->stop_requested, false);
    r->entries = NULL;
    r->count = 0;
    r->capacity = 0;

    return r;
}

void mcp_reactor_destroy(mcp_context_t *ctx, mcp_reactor_t *r) {
    if (r == NULL) {
        return;
    }
    if (r->epoll_fd >= 0) {
        close(r->epoll_fd);
    }
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
        mcp_reactor_entry_t *new_entries =
            rt_realloc(ctx, r->entries, new_cap * sizeof(*new_entries));
        if (new_entries == NULL) {
            return MCP_ERR_NOMEM;
        }
        r->entries = new_entries;
        r->capacity = new_cap;
    }

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

    (void)epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);

    if (idx + 1 < r->count) {
        memmove(&r->entries[idx], &r->entries[idx + 1],
                (r->count - idx - 1) * sizeof(mcp_reactor_entry_t));
    }
    r->count--;

    return MCP_OK;
}

mcp_status_t mcp_reactor_step(mcp_context_t *ctx, mcp_reactor_t *r, int timeout_ms) {
    if (r == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_shutdown_requested()) {
        return MCP_ERR_CANCELLED;
    }

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

    if (mcp_shutdown_requested()) {
        return MCP_ERR_CANCELLED;
    }

    return MCP_OK;
}

mcp_status_t mcp_reactor_run(mcp_context_t *ctx, mcp_reactor_t *r) {
    if (r == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    atomic_store(&r->stop_requested, false);
    while (!atomic_load(&r->stop_requested)) {
        if (mcp_shutdown_requested()) {
            return MCP_ERR_CANCELLED;
        }
        mcp_status_t st = mcp_reactor_step(ctx, r, -1);
        if (st != MCP_OK) {
            if (st == MCP_ERR_CANCELLED) {
                return MCP_ERR_CANCELLED;
            }
            return st;
        }
    }

    return MCP_OK;
}

void mcp_reactor_stop(mcp_context_t *ctx, mcp_reactor_t *r) {
    if (r == NULL) {
        return;
    }
    atomic_store(&r->stop_requested, true);
    (void)mcp_reactor_wakeup(ctx, r);
}

mcp_status_t mcp_reactor_wakeup(mcp_context_t *ctx, mcp_reactor_t *r) {
    (void)ctx;
    if (r == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (r->wakeup_write_fd >= 0) {
        char b = 1;
        ssize_t n = write(r->wakeup_write_fd, &b, 1);
        if (n == 1 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            return MCP_OK;
        }
        return MCP_ERR_IO;
    }

    uint64_t val = 1;
    ssize_t n = write(r->wakeup_fd, &val, sizeof(val));
    if (n == (ssize_t)sizeof(val) || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return MCP_OK;
    }
    return MCP_ERR_IO;
}
