/**
 * @file stdio.c
 *
 * FILE*-backed line transport. recv() reads one line (CRLF stripped)
 * per call and returns an owned heap string; an oversized line (>4MB)
 * is discarded to the newline and yields MCP_ERR_PROTOCOL, and an EOF
 * with no data yields MCP_ERR_IO. recv() reads with read() on the
 * input fd directly — never fgetc() — so a poll() readiness wait
 * cannot miss bytes already sitting in a userspace stdio buffer;
 * bytes read past the newline are kept in a backend stash for the
 * next call. When a read timeout is set via mcp_transport_set_timeout,
 * recv() polls the input fd against a monotonic deadline and returns
 * MCP_ERR_TIMEOUT on expiry. The FILE* handles are borrowed — this
 * transport never fclose()s them — but the caller must not read
 * b->in through stdio while the transport owns it. mcp_stdio_serve
 * manages the transport's start/stop/destroy internally; the caller
 * must not.
 */
#define _DEFAULT_SOURCE
#include "mcpkit/transport/stdio.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "internals.h"
#include "mcpkit/client/client.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/shutdown.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/transport.h"

typedef struct {
    FILE *in;
    FILE *out;
    char *stash;
    size_t stash_len;
} stdio_backend_t;

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Polls fd for readability until the absolute monotonic deadline
 * (deadline==0 waits forever). EINTR restarts against the same
 * deadline. Only POLLIN readiness is waited on. */
static mcp_status_t wait_readable(int fd, uint64_t deadline) {
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
        pfd.events = POLLIN;
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

static mcp_status_t stdio_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    return t != NULL ? MCP_OK : MCP_ERR_INVALID_ARGUMENT;
}

static mcp_status_t stdio_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                               size_t len) {
    if (t == NULL || (data == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    stdio_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (len > 0 && fwrite(data, 1, len, b->out) != len) {
        return MCP_ERR_IO;
    }
    if (fputc('\n', b->out) == EOF || fflush(b->out) == EOF) {
        return MCP_ERR_IO;
    }
    return MCP_OK;
}

static mcp_status_t stash_store(mcp_context_t *ctx, stdio_backend_t *b, const char *data,
                                size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(b->stash, a->userdata);
    b->stash = NULL;
    b->stash_len = 0;
    if (n == 0) {
        return MCP_OK;
    }
    b->stash = a->malloc_fn(n, a->userdata);
    if (b->stash == NULL) {
        return MCP_ERR_NOMEM;
    }
    memcpy(b->stash, data, n);
    b->stash_len = n;
    return MCP_OK;
}

static mcp_status_t stdio_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    if (t == NULL || line_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    stdio_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->in == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    int fd = fileno(b->in);
    if (fd < 0) {
        return MCP_ERR_IO;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    size_t cap = 256;
    size_t len = 0;
    char *buf = a->malloc_fn(cap, a->userdata);
    if (buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (b->stash_len > 0) {
        if (b->stash_len + 1 > cap) {
            cap = b->stash_len + 1;
            char *nbuf = a->realloc_fn(buf, cap, a->userdata);
            if (nbuf == NULL) {
                a->free_fn(buf, a->userdata);
                return MCP_ERR_NOMEM;
            }
            buf = nbuf;
        }
        memcpy(buf, b->stash, b->stash_len);
        len = b->stash_len;
        a->free_fn(b->stash, a->userdata);
        b->stash = NULL;
        b->stash_len = 0;
    }
    uint64_t read_ms = 0, write_ms = 0;
    mcp_transport_get_timeout(ctx, t, &read_ms, &write_ms);
    (void)write_ms;
    uint64_t deadline = read_ms == 0 ? 0 : now_ms() + read_ms;
    char tmp[4096];
    bool huge = false;
    for (;;) {
        const char *nl = memchr(buf, '\n', len);
        if (nl != NULL) {
            size_t llen = (size_t)(nl - buf);
            if (stash_store(ctx, b, nl + 1, len - llen - 1) != MCP_OK) {
                a->free_fn(buf, a->userdata);
                return MCP_ERR_NOMEM;
            }
            if (llen > 0 && buf[llen - 1] == '\r') {
                llen--;
            }
            buf[llen] = '\0';
            *line_out = buf;
            return MCP_OK;
        }
        if (deadline != 0) {
            mcp_status_t ws = wait_readable(fd, deadline);
            if (ws != MCP_OK) {
                if (ws == MCP_ERR_TIMEOUT) {
                    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_DEBUG,
                                    "event=recv_timeout transport=stdio");
                }
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return ws;
            }
        }
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (len == 0) {
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return MCP_ERR_IO;
            }
            break;
        }
        if (n == 0) {
            if (len == 0) {
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return MCP_ERR_IO;
            }
            break;
        }
        if (len + (size_t)n + 1 > MCP_PROTOCOL_MAX_MESSAGE_BYTES) {
            huge = true;
            break;
        }
        if (len + (size_t)n + 1 > cap) {
            size_t ncap = cap * 2;
            if (ncap < len + (size_t)n + 1) {
                ncap = len + (size_t)n + 1;
            }
            char *nbuf = a->realloc_fn(buf, ncap, a->userdata);
            if (nbuf == NULL) {
                a->free_fn(buf, a->userdata);
                return MCP_ERR_NOMEM;
            }
            buf = nbuf;
            cap = ncap;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
    }
    if (huge) {
        a->free_fn(buf, a->userdata);
        buf = NULL;
        for (;;) {
            if (deadline != 0) {
                mcp_status_t ws = wait_readable(fd, deadline);
                if (ws != MCP_OK) {
                    *line_out = NULL;
                    return ws;
                }
            }
            ssize_t n = read(fd, tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }
            const char *nl = memchr(tmp, '\n', (size_t)n);
            if (nl != NULL) {
                size_t rest = (size_t)n - ((size_t)(nl - tmp) + 1);
                if (stash_store(ctx, b, nl + 1, rest) != MCP_OK) {
                    *line_out = NULL;
                    return MCP_ERR_NOMEM;
                }
                break;
            }
        }
        *line_out = NULL;
        return MCP_ERR_PROTOCOL;
    }
    if (len > 0 && buf[len - 1] == '\r') {
        len--;
    }
    buf[len] = '\0';
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t stdio_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    stdio_backend_t *b = mcp_transport_backend(ctx, t);
    if (b != NULL) {
        a->free_fn(b->stash, a->userdata);
        a->free_fn(b, a->userdata);
    }
    mcp_transport_set_backend(ctx, t, NULL);
    return MCP_OK;
}

static const mcp_transport_ops_t kStdioOps = {
    stdio_start,
    stdio_send,
    stdio_recv,
    stdio_stop,
};

mcp_transport_t *mcp_stdio_transport_create(mcp_context_t *ctx, FILE *in, FILE *out) {
    const mcp_allocator_t *a = alloc_of(ctx);
    stdio_backend_t *b = a->malloc_fn(sizeof(*b), a->userdata);
    if (b == NULL) {
        return NULL;
    }
    b->in = in != NULL ? in : stdin;
    b->out = out != NULL ? out : stdout;
    b->stash = NULL;
    b->stash_len = 0;
    mcp_transport_t *t = mcp_transport_create(ctx, &kStdioOps, b);
    if (t == NULL) {
        a->free_fn(b, a->userdata);
        return NULL;
    }
    return t;
}

static mcp_status_t send_notification(mcp_context_t *ctx, mcp_transport_t *t,
                                     mcp_message_t *msg) {
    char *out = mcp_message_serialize(ctx, msg);
    mcp_message_destroy(ctx, msg);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

static mcp_status_t send_error(mcp_context_t *ctx, mcp_transport_t *t, int code,
                               const char *text) {
    mcp_message_t *err = mcp_response_err_new(ctx, NULL, code, text, NULL);
    if (err == NULL) {
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, err);
    mcp_message_destroy(ctx, err);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

static mcp_status_t stdio_serve_impl(mcp_context_t *ctx, mcp_server_t *server,
                                     mcp_client_t *client, mcp_transport_t *t) {
    if (server == NULL || t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_session_t *sess = mcp_server_create_session(ctx, server);
    if (sess == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=serve_start transport=stdio");
    mcp_status_t status = MCP_OK;
    for (;;) {
        if (mcp_shutdown_requested()) {
            mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_INFO, "event=shutdown");
            status = MCP_ERR_CANCELLED;
            break;
        }
        mcp_status_t st = MCP_OK;
        for (;;) {
            mcp_message_t *notify = NULL;
            if (mcp_server_outbox_pop(ctx, server, &notify) != MCP_OK) {
                break;
            }
            st = send_notification(ctx, t, notify);
            if (st != MCP_OK) {
                status = st;
                goto done;
            }
        }
        char *line = NULL;
        st = mcp_transport_recv(ctx, t, &line);
        if (st == MCP_ERR_IO) {
            break;
        }
        if (st == MCP_ERR_TIMEOUT) {
            status = mcp_shutdown_requested() ? MCP_ERR_CANCELLED : st;
            break;
        }
        if (st != MCP_OK) {
            status = st;
            break;
        }
        mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
        mcp_json_free_string(ctx, line);
        if (msg == NULL) {
            mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_WARN, "event=parse_error");
            if (send_error(ctx, t, MCP_RPC_PARSE_ERROR, "Parse error") != MCP_OK) {
                status = MCP_ERR_NOMEM;
                break;
            }
            continue;
        }
        mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);
        if (kind == MCP_MSG_NOTIFICATION) {
            mcp_server_notify(ctx, server, sess, msg);
            mcp_message_destroy(ctx, msg);
            continue;
        }
        if (kind != MCP_MSG_REQUEST) {
            mcp_message_destroy(ctx, msg);
            continue;
        }
        /* Route server-to-client requests to the client when available. */
        const char *method = mcp_message_method(ctx, msg);
        bool is_server_to_client = client != NULL && method != NULL &&
            (strcmp(method, "roots/list") == 0 ||
             strcmp(method, "sampling/createMessage") == 0 ||
             strcmp(method, "elicitation/create") == 0);
        mcp_message_t *resp = NULL;
        if (is_server_to_client) {
            st = mcp_client_handle_server_request(ctx, client, msg, &resp);
        } else {
            st = mcp_server_dispatch(ctx, server, sess, msg, &resp);
        }
        mcp_message_destroy(ctx, msg);
        if (st != MCP_OK) {
            status = st;
            break;
        }
        if (resp != NULL) {
            char *out = mcp_message_serialize(ctx, resp);
            mcp_message_destroy(ctx, resp);
            if (out == NULL) {
                status = MCP_ERR_NOMEM;
                break;
            }
            st = mcp_transport_send(ctx, t, out, strlen(out));
            mcp_json_free_string(ctx, out);
            if (st != MCP_OK) {
                status = st;
                break;
            }
        }
    }
done:
    mcp_server_destroy_session(ctx, server, sess);
    return status;
}

mcp_status_t mcp_stdio_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t) {
    return stdio_serve_impl(ctx, server, NULL, t);
}

mcp_status_t mcp_stdio_serve_with_client(mcp_context_t *ctx, mcp_server_t *server,
                                         mcp_client_t *client, mcp_transport_t *t) {
    return stdio_serve_impl(ctx, server, client, t);
}
