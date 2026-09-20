/**
 * @file socket.c
 *
 * POSIX TCP line transport. In server mode create() binds and
 * listens but does NOT accept; start() performs the accept() and
 * replaces the listening fd with the connection. send() appends a
 * newline and retries on EINTR. recv() reads SOCK_LINE_CAP (4KB)
 * chunks and grows the line buffer on demand, rejecting a line once
 * it reaches MCP_PROTOCOL_MAX_MESSAGE_BYTES (4MB) with MCP_ERR_PROTOCOL
 * and returning MCP_ERR_IO on a clean EOF.
 */
#define _DEFAULT_SOURCE
#include "mcpkit/transport/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internals.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/transport/transport.h"

#define SOCK_LINE_CAP 4096u

typedef struct {
    int fd;
    bool server_mode;
} socket_backend_t;

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
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
        int cfd = accept(b->fd, (struct sockaddr *)&client_addr, &slen);
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
    size_t off = 0;
    while (off < len) {
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
    size_t cap = 256;
    size_t len = 0;
    char *buf = a->malloc_fn(cap, a->userdata);
    if (buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    for (;;) {
        char chunk[SOCK_LINE_CAP];
        ssize_t n = read(b->fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            a->free_fn(buf, a->userdata);
            return MCP_ERR_IO;
        }
        if (n == 0) {
            if (len == 0) {
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return MCP_ERR_IO;
            }
            buf[len] = '\0';
            *line_out = buf;
            return MCP_OK;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                if (len > 0 && buf[len - 1] == '\r') {
                    len--;
                }
                buf[len] = '\0';
                *line_out = buf;
                return MCP_OK;
            }
            if (c == '\r') {
                continue;
            }
            if (len + 1 >= cap) {
                if (cap >= MCP_PROTOCOL_MAX_MESSAGE_BYTES) {
                    a->free_fn(buf, a->userdata);
                    *line_out = NULL;
                    return MCP_ERR_PROTOCOL;
                }
                size_t ncap = cap * 2;
                char *nbuf = a->realloc_fn(buf, ncap, a->userdata);
                if (nbuf == NULL) {
                    a->free_fn(buf, a->userdata);
                    return MCP_ERR_NOMEM;
                }
                buf = nbuf;
                cap = ncap;
            }
            buf[len++] = c;
        }
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
    mcp_transport_t *t = mcp_transport_create(ctx, &kSocketOps, b);
    if (t == NULL) {
        close(fd);
        a->free_fn(b, a->userdata);
        return NULL;
    }
    return t;
}
