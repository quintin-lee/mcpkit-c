/**
 * @file socket.c
 *
 * POSIX TCP line transport. In server mode create() binds and
 * listens but does NOT accept; start() performs the accept() and
 * replaces the listening fd with the connection. send() appends a
 * newline and retries on EINTR. recv() reads SOCK_LINE_CAP (4KB)
 * chunks and grows the line buffer on demand, rejecting a line once
 * it reaches MCP_PROTOCOL_MAX_MESSAGE_BYTES (4MB) with MCP_ERR_PROTOCOL
 * and returning MCP_ERR_IO on a clean EOF. When timeouts are set via
 * mcp_transport_set_timeout, send/recv wait on poll() against a single
 * monotonic deadline per call and return MCP_ERR_TIMEOUT on expiry.
 */
#define _DEFAULT_SOURCE
#include "mcpkit/transport/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "internals.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/shutdown.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/runtime/executor.h"
#include "mcpkit/server/dispatcher.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"
#include "mcpkit/transport/transport.h"

#define SOCK_LINE_CAP 4096u

typedef struct {
    int fd;
    bool server_mode;
    char *carry;
    size_t carry_len;
    size_t carry_cap;
} socket_backend_t;

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Wait until fd is ready for events or the absolute monotonic deadline
 * passes (deadline==0 waits forever). EINTR restarts with the same
 * deadline so a signal cannot extend or shorten the window. */
static mcp_status_t wait_until(int fd, short events, uint64_t deadline) {
    for (;;) {
        int to = -1;
        if (deadline != 0) {
            uint64_t now = now_ms();
            if (now >= deadline) {
                return MCP_ERR_TIMEOUT;
            }
            uint64_t left = deadline - now;
            to = left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        int n = poll(&pfd, 1, to);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MCP_ERR_IO;
        }
        if (n == 0) {
            return MCP_ERR_TIMEOUT;
        }
        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            return MCP_ERR_IO;
        }
        return MCP_OK;
    }
}

static int open_socket(const char *host, uint16_t port, bool server_mode) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (server_mode) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
            close(fd);
            return -1;
        }
    } else {
        if (host != NULL) {
            if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
                close(fd);
                return -1;
            }
        } else {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return fd;
}

static mcp_status_t sock_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    socket_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->fd < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (b->server_mode) {
        struct sockaddr_in client_addr;
        socklen_t slen = sizeof(client_addr);
        int cfd;
        do {
            cfd = accept(b->fd, (struct sockaddr *)&client_addr, &slen);
        } while (cfd < 0 && errno == EINTR);
        if (cfd < 0) {
            return MCP_ERR_IO;
        }
        int one = 1;
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        close(b->fd);
        b->fd = cfd;
        b->server_mode = false;
    }
    return MCP_OK;
}

static mcp_status_t sock_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    if (t == NULL || (data == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    socket_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->fd < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    uint64_t read_ms = 0, write_ms = 0;
    mcp_transport_get_timeout(ctx, t, &read_ms, &write_ms);
    (void)read_ms;
    uint64_t deadline = write_ms == 0 ? 0 : now_ms() + write_ms;
    size_t off = 0;
    while (off < len) {
        if (deadline != 0) {
            mcp_status_t ws = wait_until(b->fd, POLLOUT, deadline);
            if (ws != MCP_OK) {
                return ws;
            }
        }
        ssize_t n = send(b->fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MCP_ERR_IO;
        }
        if (n == 0) {
            return MCP_ERR_IO;
        }
        off += (size_t)n;
    }
    // MSG_NOSIGNAL: a peer that closed the socket yields EPIPE ->
    // MCP_ERR_IO instead of the default SIGPIPE process kill.
    if (deadline != 0) {
        mcp_status_t ws = wait_until(b->fd, POLLOUT, deadline);
        if (ws != MCP_OK) {
            return ws;
        }
    }
    if (send(b->fd, "\n", 1, MSG_NOSIGNAL) < 0) {
        return MCP_ERR_IO;
    }
    return MCP_OK;
}

static mcp_status_t sock_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    if (t == NULL || line_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    socket_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->fd < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    uint64_t read_ms = 0, write_ms = 0;
    mcp_transport_get_timeout(ctx, t, &read_ms, &write_ms);
    (void)write_ms;
    uint64_t deadline = read_ms == 0 ? 0 : now_ms() + read_ms;

    for (;;) {
        /* Check if carry buffer already contains a newline */
        if (b->carry != NULL && b->carry_len > 0) {
            char *nl = memchr(b->carry, '\n', b->carry_len);
            if (nl != NULL) {
                size_t line_len = (size_t)(nl - b->carry);
                size_t copy_len = line_len;
                if (copy_len > 0 && b->carry[copy_len - 1] == '\r') {
                    copy_len--;
                }
                char *line = a->malloc_fn(copy_len + 1, a->userdata);
                if (line == NULL) {
                    *line_out = NULL;
                    return MCP_ERR_NOMEM;
                }
                memcpy(line, b->carry, copy_len);
                line[copy_len] = '\0';

                size_t rem = b->carry_len - (line_len + 1);
                if (rem > 0) {
                    memmove(b->carry, nl + 1, rem);
                }
                b->carry_len = rem;
                *line_out = line;
                return MCP_OK;
            }
        }

        /* Check max message size before reading more */
        if (b->carry_len >= MCP_PROTOCOL_MAX_MESSAGE_BYTES) {
            *line_out = NULL;
            return MCP_ERR_PROTOCOL;
        }

        if (deadline != 0) {
            mcp_status_t ws = wait_until(b->fd, POLLIN, deadline);
            if (ws != MCP_OK) {
                if (ws == MCP_ERR_TIMEOUT) {
                    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_DEBUG,
                                    "event=recv_timeout transport=socket");
                }
                *line_out = NULL;
                return ws;
            }
        }

        char chunk[SOCK_LINE_CAP];
        ssize_t n = read(b->fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *line_out = NULL;
            return MCP_ERR_IO;
        }
        if (n == 0) {
            /* EOF */
            if (b->carry_len == 0) {
                *line_out = NULL;
                return MCP_ERR_IO;
            }
            /* Return remainder as the final line */
            size_t copy_len = b->carry_len;
            if (copy_len > 0 && b->carry[copy_len - 1] == '\r') {
                copy_len--;
            }
            char *line = a->malloc_fn(copy_len + 1, a->userdata);
            if (line == NULL) {
                *line_out = NULL;
                return MCP_ERR_NOMEM;
            }
            memcpy(line, b->carry, copy_len);
            line[copy_len] = '\0';
            b->carry_len = 0;
            *line_out = line;
            return MCP_OK;
        }

        /* Append chunk to carry buffer */
        if (b->carry_len + (size_t)n > b->carry_cap) {
            size_t new_cap = b->carry_cap == 0 ? 512 : b->carry_cap * 2;
            while (new_cap < b->carry_len + (size_t)n) {
                new_cap *= 2;
            }
            char *new_buf = a->realloc_fn(b->carry, new_cap, a->userdata);
            if (new_buf == NULL) {
                *line_out = NULL;
                return MCP_ERR_NOMEM;
            }
            b->carry = new_buf;
            b->carry_cap = new_cap;
        }
        memcpy(b->carry + b->carry_len, chunk, (size_t)n);
        b->carry_len += (size_t)n;
    }
}

static mcp_status_t sock_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    socket_backend_t *b = mcp_transport_backend(ctx, t);
    if (b != NULL) {
        if (b->fd >= 0) {
            close(b->fd);
        }
        if (b->carry != NULL) {
            a->free_fn(b->carry, a->userdata);
            b->carry = NULL;
        }
        a->free_fn(b, a->userdata);
    }
    mcp_transport_set_backend(ctx, t, NULL);
    return MCP_OK;
}

static const mcp_transport_ops_t kSocketOps = {
    sock_start,
    sock_send,
    sock_recv,
    sock_stop,
};

// Wraps an already-connected fd in a client-mode transport. Used by
// mcp_socket_serve to hand each accepted connection to a worker.
static mcp_transport_t *socket_transport_from_fd(mcp_context_t *ctx, int fd) {
    const mcp_allocator_t *a = alloc_of(ctx);
    socket_backend_t *b = a->malloc_fn(sizeof(*b), a->userdata);
    if (b == NULL) {
        return NULL;
    }
    b->fd = fd;
    b->server_mode = false; // already connected; sock_start is a no-op
    b->carry = NULL;
    b->carry_len = 0;
    b->carry_cap = 0;
    mcp_transport_t *t = mcp_transport_create(ctx, &kSocketOps, b);
    if (t == NULL) {
        a->free_fn(b, a->userdata);
        close(fd);
    }
    return t;
}

/* Helpers for the per-connection serve loop (serve_one_conn). */
static mcp_status_t conn_send_error(mcp_context_t *ctx, mcp_transport_t *t, int code,
                                   const char *text) {
    mcp_message_t *err = mcp_response_err_new(ctx, NULL, code, text, NULL);
    if (err == NULL) return MCP_ERR_NOMEM;
    char *out = mcp_message_serialize(ctx, err);
    mcp_message_destroy(ctx, err);
    if (out == NULL) return MCP_ERR_NOMEM;
    mcp_status_t st = mcp_transport_send(ctx, t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

static mcp_status_t conn_send_response(mcp_context_t *ctx, mcp_transport_t *t,
                                       mcp_message_t *resp) {
    char *out = mcp_message_serialize(ctx, resp);
    mcp_message_destroy(ctx, resp);
    if (out == NULL) return MCP_ERR_NOMEM;
    mcp_status_t st = mcp_transport_send(ctx, t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

typedef struct {
    mcp_context_t *ctx;
    mcp_server_t  *server;
    int            cfd;
    const mcp_allocator_t *alloc;
} conn_job_t;

// Runs one connection's serve loop on a pool worker thread.
// Creates a session, drains the outbox, processes requests/notifications
// until clean EOF, then tears down the session and transport.
static void serve_one_conn(mcp_context_t *ctx, void *arg) {
    conn_job_t *job = (conn_job_t *)arg;
    const mcp_allocator_t *a = job->alloc;
    mcp_transport_t *t = socket_transport_from_fd(ctx, job->cfd);
    if (t == NULL) {
        a->free_fn(job, a->userdata);
        return;
    }
    mcp_session_t *sess = mcp_server_create_session(ctx, job->server);
    if (sess == NULL) {
        mcp_transport_destroy(ctx, t);
        a->free_fn(job, a->userdata);
        return;
    }
    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=conn_start transport=socket");
    for (;;) {
        if (mcp_shutdown_requested()) break;
        // Drain outbox before each read.
        mcp_status_t st = MCP_OK;
        for (;;) {
            mcp_message_t *notify = NULL;
            if (mcp_server_outbox_pop(ctx, job->server, &notify) != MCP_OK) break;
            st = conn_send_response(ctx, t, notify);
            if (st != MCP_OK) goto done;
        }
        char *line = NULL;
        st = mcp_transport_recv(ctx, t, &line);
        if (st == MCP_ERR_IO || st == MCP_ERR_TIMEOUT) break;
        if (st != MCP_OK) {
            mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_WARN, "event=conn_recv_err");
            break;
        }
        mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
        mcp_json_free_string(ctx, line);
        if (msg == NULL) {
            mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_WARN, "event=parse_error");
            if (conn_send_error(ctx, t, MCP_RPC_PARSE_ERROR, "Parse error") != MCP_OK) goto done;
            continue;
        }
        mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);
        if (kind == MCP_MSG_NOTIFICATION) {
            mcp_server_notify(ctx, job->server, sess, msg);
            mcp_message_destroy(ctx, msg);
            continue;
        }
        if (kind != MCP_MSG_REQUEST) {
            mcp_message_destroy(ctx, msg);
            continue;
        }
        mcp_message_t *resp = NULL;
        mcp_status_t dst = mcp_server_dispatch(ctx, job->server, sess, msg, &resp);
        mcp_message_destroy(ctx, msg);
        if (dst != MCP_OK) {
            mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_DEBUG, "event=dispatch_err");
            break;
        }
        if (resp != NULL) {
            st = conn_send_response(ctx, t, resp);
            if (st != MCP_OK) goto done;
        }
    }
done:
    mcp_server_destroy_session(ctx, job->server, sess);
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=conn_end transport=socket");
    a->free_fn(job, a->userdata);
}

mcp_status_t mcp_socket_serve(mcp_context_t *ctx, mcp_server_t *server,
                              uint16_t port, mcp_executor_t *pool) {
    if (server == NULL || pool == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    int lfd = open_socket(NULL, port, true);
    if (lfd < 0) {
        return MCP_ERR_NOMEM;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=serve_start transport=socket");
    for (;;) {
        struct pollfd pfd;
        pfd.fd = lfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int n = poll(&pfd, 1, 500);
        if (n < 0 && errno != EINTR) {
            close(lfd);
            return MCP_ERR_IO;
        }
        if (mcp_shutdown_requested()) {
            mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=shutdown transport=socket");
            break;
        }
        if (n == 0) {
            continue;
        }
        struct sockaddr_in client_addr;
        socklen_t slen = sizeof(client_addr);
        int cfd;
        do {
            cfd = accept(lfd, (struct sockaddr *)&client_addr, &slen);
        } while (cfd < 0 && errno == EINTR);
        if (cfd < 0) {
            continue;
        }
        int one = 1;
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        conn_job_t *job = a->malloc_fn(sizeof(*job), a->userdata);
        if (job == NULL) {
            close(cfd);
            continue;
        }
        job->ctx = ctx;
        job->server = server;
        job->cfd = cfd;
        job->alloc = a;
        mcp_status_t st = mcp_executor_submit(ctx, pool, serve_one_conn, job);
        if (st != MCP_OK) {
            a->free_fn(job, a->userdata);
            close(cfd);
        }
    }
    close(lfd);
    mcp_executor_wait(ctx, pool);
    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=serve_end transport=socket");
    return MCP_OK;
}

mcp_transport_t *mcp_socket_transport_create(mcp_context_t *ctx, const char *host,
                                             uint16_t port, bool server_mode) {
    int fd = open_socket(host, port, server_mode);
    if (fd < 0) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    socket_backend_t *b = a->malloc_fn(sizeof(*b), a->userdata);
    if (b == NULL) {
        close(fd);
        return NULL;
    }
    b->fd = fd;
    b->server_mode = server_mode;
    b->carry = NULL;
    b->carry_len = 0;
    b->carry_cap = 0;
    mcp_transport_t *t = mcp_transport_create(ctx, &kSocketOps, b);
    if (t == NULL) {
        close(fd);
        a->free_fn(b, a->userdata);
        return NULL;
    }
    return t;
}
