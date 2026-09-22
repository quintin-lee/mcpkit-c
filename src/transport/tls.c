/**
 * @file tls.c
 *
 * OpenSSL-backed TLS line transport. The TCP socket is opened the
 * same way as socket.c; the TLS handshake completes inside
 * mcp_tls_transport_create() for the client (SSL_connect) and inside
 * start() for the server (SSL_accept).  send/recv use SSL_write/
 * SSL_read with the same 4 KB chunk + 4 MB cap + deadline logic as
 * socket.c.  stop() calls SSL_shutdown (best-effort), SSL_free,
 * SSL_CTX_free, close(fd), and nulls the backend pointer.
 *
 * Peer certificate verification is DISABLED by design in this
 * adapter: it is suitable for loopback testing and development; a
 * production deployment must install its own CA bundle before use.
 */
#define _DEFAULT_SOURCE
#include "mcpkit/transport/tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "internals.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/transport/transport.h"

#define TLS_LINE_CAP 4096u

typedef struct {
    int     fd;
    SSL    *ssl;
    SSL_CTX *sctx;
    bool    server_mode;
} tls_backend_t;

/* ------------------------------------------------------------------ */
/* Shared helpers (same as socket.c)                                   */
/* ------------------------------------------------------------------ */

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

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

/* ------------------------------------------------------------------ */
/* Socket helpers (same as socket.c)                                    */
/* ------------------------------------------------------------------ */

static int open_tcp_socket(const char *host, uint16_t port, bool server_mode) {
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
        if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(fd, 16) < 0) {
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

/* ------------------------------------------------------------------ */
/* TLS handshake helpers                                               */
/* ------------------------------------------------------------------ */

static int tls_accept(int lfd, SSL_CTX *sctx, SSL **ssl_out) {
    /* Accept one TCP connection, then run SSL_accept on it. */
    struct sockaddr_in client_addr;
    socklen_t slen = sizeof(client_addr);
    int cfd;
    do {
        cfd = accept(lfd, (struct sockaddr *)&client_addr, &slen);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) {
        return -1;
    }
    SSL *ssl = SSL_new(sctx);
    if (ssl == NULL) {
        close(cfd);
        return -1;
    }
    /* BIO_NOCLOSE: SSL_free(ssl) does NOT close the fd; tls_stop()
     * owns the fd and closes it explicitly. */
    BIO *fd_bio = BIO_new_fd(cfd, BIO_NOCLOSE);
    if (fd_bio == NULL) {
        SSL_free(ssl);
        close(cfd);
        return -1;
    }
    SSL_set_bio(ssl, fd_bio, fd_bio);
    SSL_set_accept_state(ssl);

    /* Handshake with a deadline-aware poll loop.  SSL_do_handshake
     * returns 1 on success, 0 on fatal error, <0 on want read/write. */
    for (;;) {
        int r = SSL_do_handshake(ssl);
        if (r == 1) {
            *ssl_out = ssl;
            return cfd;
        }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ) {
            /* Poll for readability on cfd before retrying. */
            struct pollfd pfd = {.fd = cfd, .events = POLLIN, .revents = 0};
            int n = poll(&pfd, 1, -1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                SSL_free(ssl);
                close(cfd);
                return -1;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            struct pollfd pfd = {.fd = cfd, .events = POLLOUT, .revents = 0};
            int n = poll(&pfd, 1, -1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                SSL_free(ssl);
                close(cfd);
                return -1;
            }
        } else {
            SSL_free(ssl);
            close(cfd);
            return -1;
        }
    }
}

static int tls_connect(int cfd, SSL_CTX *sctx, SSL **ssl_out) {
    SSL *ssl = SSL_new(sctx);
    if (ssl == NULL) {
        return -1;
    }
    BIO *bio = BIO_new_fd(cfd, BIO_NOCLOSE);
    if (bio == NULL) {
        SSL_free(ssl);
        return -1;
    }
    SSL_set_bio(ssl, bio, bio);
    SSL_set_connect_state(ssl);
    /* Peer verification is disabled (SSL_set_verify with
     * SSL_VERIFY_NONE is the default; no CA bundle is loaded). */

    for (;;) {
        int r = SSL_do_handshake(ssl);
        if (r == 1) {
            *ssl_out = ssl;
            return 0;
        }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ) {
            struct pollfd pfd = {.fd = cfd, .events = POLLIN, .revents = 0};
            int n = poll(&pfd, 1, -1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                SSL_free(ssl);
                return -1;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            struct pollfd pfd = {.fd = cfd, .events = POLLOUT, .revents = 0};
            int n = poll(&pfd, 1, -1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                SSL_free(ssl);
                return -1;
            }
        } else {
            SSL_free(ssl);
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 4-op vtable                                                         */
/* ------------------------------------------------------------------ */

static mcp_status_t tls_start(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    tls_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->fd < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (b->server_mode && b->sctx != NULL) {
        /* Server mode: accept one TLS connection on the listening fd.
         * After success, replace b->fd with the accepted conn fd and
         * mark server_mode=false so a subsequent start() is a no-op. */
        SSL *ssl = NULL;
        int cfd = tls_accept(b->fd, b->sctx, &ssl);
        if (cfd < 0) {
            return MCP_ERR_IO;
        }
        int one = 1;
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        close(b->fd);
        b->fd = cfd;
        b->ssl = ssl;
        b->server_mode = false;
    }
    /* Client mode: handshake already done in create(); no-op. */
    return MCP_OK;
}

static mcp_status_t tls_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                             size_t len) {
    if (t == NULL || (data == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    tls_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->ssl == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    uint64_t read_ms = 0, write_ms = 0;
    mcp_transport_get_timeout(ctx, t, &read_ms, &write_ms);
    (void)read_ms;
    uint64_t deadline = write_ms == 0 ? 0 : now_ms() + write_ms;

    /* Write the payload in chunks, retrying on WANT_WRITE. */
    size_t off = 0;
    while (off < len) {
        if (deadline != 0) {
            mcp_status_t ws = wait_until(b->fd, POLLOUT, deadline);
            if (ws != MCP_OK) {
                return ws;
            }
        }
        int n = SSL_write(b->ssl, data + off, (int)(len - off));
        if (n < 0) {
            int err = SSL_get_error(b->ssl, n);
            if (err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return MCP_ERR_IO;
        }
        off += (size_t)n;
    }

    /* Append newline. */
    if (deadline != 0) {
        mcp_status_t ws = wait_until(b->fd, POLLOUT, deadline);
        if (ws != MCP_OK) {
            return ws;
        }
    }
    int n = SSL_write(b->ssl, "\n", 1);
    if (n < 0) {
        int err = SSL_get_error(b->ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            /* One more poll+retry cycle. */
            if (deadline != 0) {
                wait_until(b->fd, POLLOUT, deadline);
            }
            n = SSL_write(b->ssl, "\n", 1);
        }
        if (n < 0) {
            return MCP_ERR_IO;
        }
    }
    return MCP_OK;
}

static mcp_status_t tls_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    if (t == NULL || line_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    tls_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->ssl == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    size_t cap = 256;
    size_t len = 0;
    char *buf = a->malloc_fn(cap, a->userdata);
    if (buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    uint64_t read_ms = 0, write_ms = 0;
    mcp_transport_get_timeout(ctx, t, &read_ms, &write_ms);
    (void)write_ms;
    uint64_t deadline = read_ms == 0 ? 0 : now_ms() + read_ms;

    for (;;) {
        if (deadline != 0) {
            mcp_status_t ws = wait_until(b->fd, POLLIN, deadline);
            if (ws != MCP_OK) {
                if (ws == MCP_ERR_TIMEOUT) {
                    mcp_logger_logf(mcp_context_logger(ctx), MCP_LOG_DEBUG,
                                    "event=recv_timeout transport=tls");
                }
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return ws;
            }
        }
        char chunk[TLS_LINE_CAP];
        int n = SSL_read(b->ssl, chunk, sizeof(chunk));
        if (n < 0) {
            int err = SSL_get_error(b->ssl, n);
            if (err == SSL_ERROR_WANT_READ) {
                continue;
            }
            /* SSL_ERROR_SYSERR with ECONNRESET or clean EOF:
             * SSL_read returns 0 on orderly peer close. */
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
            a->free_fn(buf, a->userdata);
            return MCP_ERR_IO;
        }
        if (n == 0) {
            /* Peer closed TLS connection. */
            if (len == 0) {
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return MCP_ERR_IO;
            }
            buf[len] = '\0';
            *line_out = buf;
            return MCP_OK;
        }
        for (int i = 0; i < n; i++) {
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

static mcp_status_t tls_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    tls_backend_t *b = mcp_transport_backend(ctx, t);
    if (b != NULL) {
        /* SSL_shutdown is best-effort: the peer may have already
         * disconnected, and a forced close is acceptable for a
         * line transport.  The BIO owns the fd reference (BIO_NOCLOSE),
         * so we must close b->fd explicitly. */
        if (b->ssl != NULL) {
            SSL_shutdown(b->ssl);
            SSL_free(b->ssl);
        }
        if (b->sctx != NULL) {
            SSL_CTX_free(b->sctx);
        }
        if (b->fd >= 0) {
            close(b->fd);
        }
        a->free_fn(b, a->userdata);
    }
    mcp_transport_set_backend(ctx, t, NULL);
    return MCP_OK;
}

static const mcp_transport_ops_t kTlsOps = {
    tls_start,
    tls_send,
    tls_recv,
    tls_stop,
};

/* ------------------------------------------------------------------ */
/* Public constructor                                                  */
/* ------------------------------------------------------------------ */

mcp_transport_t *mcp_tls_transport_create(mcp_context_t *ctx, const char *host,
                                          uint16_t port, bool server_mode,
                                          const char *cert, const char *key) {
    int fd = open_tcp_socket(host, port, server_mode);
    if (fd < 0) {
        return NULL;
    }

    const mcp_allocator_t *a = alloc_of(ctx);
    tls_backend_t *b = a->malloc_fn(sizeof(*b), a->userdata);
    if (b == NULL) {
        close(fd);
        return NULL;
    }
    b->fd = fd;
    b->ssl = NULL;
    b->sctx = NULL;
    b->server_mode = server_mode;

    SSL_CTX *sctx = SSL_CTX_new(TLS_method());
    if (sctx == NULL) {
        close(fd);
        a->free_fn(b, a->userdata);
        return NULL;
    }
    /* Disable peer certificate verification (development / loopback use).
     * Production code must load its own CA bundle before enabling
     * SSL_set_verify(SSL_CTX, ctx, SSL_VERIFY_PEER). */
    SSL_CTX_set_verify(sctx, SSL_VERIFY_NONE, NULL);

    if (server_mode) {
        /* Load the certificate and private key. */
        if (cert == NULL || key == NULL ||
            SSL_CTX_use_certificate_file(sctx, cert, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(sctx, key, SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(sctx);
            close(fd);
            a->free_fn(b, a->userdata);
            return NULL;
        }
        b->sctx = sctx;
        /* Handshake deferred to start() (SSL_accept on the accepted conn). */
    } else {
        /* Client: handshake immediately. */
        SSL *ssl = NULL;
        if (tls_connect(fd, sctx, &ssl) != 0) {
            SSL_CTX_free(sctx);
            close(fd);
            a->free_fn(b, a->userdata);
            return NULL;
        }
        b->ssl = ssl;
        b->sctx = sctx;
        b->server_mode = false;
    }

    mcp_transport_t *t = mcp_transport_create(ctx, &kTlsOps, b);
    if (t == NULL) {
        if (b->ssl != NULL) SSL_free(b->ssl);
        SSL_CTX_free(sctx);
        close(fd);
        a->free_fn(b, a->userdata);
        return NULL;
    }
    return t;
}
