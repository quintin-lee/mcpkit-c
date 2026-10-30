/**
 * @file ws.c
 * @brief RFC 6455 WebSocket protocol frame codec and transport.
 */

#define _DEFAULT_SOURCE

#include "mcpkit/transport/ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"
#include "mcpkit/transport/transport.h"
#include "../core/sha1.h"

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static void *ws_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void ws_free(mcp_context_t *ctx, void *p) {
    if (p == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

mcp_status_t mcp_ws_parse_frame(mcp_context_t *ctx, const uint8_t *buf, size_t len,
                                mcp_ws_frame_t *frame_out, size_t *consumed_out) {
    if (buf == NULL || frame_out == NULL || consumed_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *consumed_out = 0;
    memset(frame_out, 0, sizeof(*frame_out));

    if (len < 2) {
        return MCP_OK; /* Need more data */
    }

    uint8_t b0 = buf[0];
    uint8_t b1 = buf[1];

    bool fin = (b0 & 0x80) != 0;
    uint8_t rsv = (b0 & 0x70);
    if (rsv != 0) {
        return MCP_ERR_PROTOCOL;
    }

    uint8_t opcode_raw = (b0 & 0x0F);
    mcp_ws_opcode_t opcode;
    switch (opcode_raw) {
        case MCP_WS_OP_CONTINUATION:
        case MCP_WS_OP_TEXT:
        case MCP_WS_OP_BINARY:
        case MCP_WS_OP_CLOSE:
        case MCP_WS_OP_PING:
        case MCP_WS_OP_PONG:
            opcode = (mcp_ws_opcode_t)opcode_raw;
            break;
        default:
            return MCP_ERR_PROTOCOL;
    }

    bool is_control = (opcode_raw & 0x08) != 0;
    if (is_control && !fin) {
        /* Control frames cannot be fragmented */
        return MCP_ERR_PROTOCOL;
    }

    bool masked = (b1 & 0x80) != 0;
    uint64_t payload_len = (b1 & 0x7F);

    if (is_control && payload_len > 125) {
        /* Control frames cannot have payload > 125 */
        return MCP_ERR_PROTOCOL;
    }

    size_t header_len = 2;
    if (payload_len == 126) {
        header_len += 2;
        if (len < header_len) {
            return MCP_OK;
        }
        payload_len = ((uint64_t)buf[2] << 8) | buf[3];
    } else if (payload_len == 127) {
        header_len += 8;
        if (len < header_len) {
            return MCP_OK;
        }
        if ((buf[2] & 0x80) != 0) {
            /* RFC 6455 5.2: Most significant bit must be zero */
            return MCP_ERR_PROTOCOL;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | buf[2 + i];
        }
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (len < header_len + 4) {
            return MCP_OK;
        }
        memcpy(mask_key, buf + header_len, 4);
        header_len += 4;
    }

    if (len < header_len + payload_len) {
        return MCP_OK; /* Payload incomplete */
    }

    char *payload_copy = NULL;
    if (payload_len > 0) {
        payload_copy = ws_malloc(ctx, (size_t)payload_len + 1);
        if (payload_copy == NULL) {
            return MCP_ERR_NOMEM;
        }
        const uint8_t *src = buf + header_len;
        if (masked) {
            for (size_t i = 0; i < (size_t)payload_len; i++) {
                payload_copy[i] = (char)(src[i] ^ mask_key[i % 4]);
            }
        } else {
            memcpy(payload_copy, src, (size_t)payload_len);
        }
        payload_copy[payload_len] = '\0';
    }

    frame_out->fin = fin;
    frame_out->opcode = opcode;
    frame_out->masked = masked;
    memcpy(frame_out->mask_key, mask_key, 4);
    frame_out->payload_len = payload_len;
    frame_out->payload = payload_copy;

    *consumed_out = header_len + (size_t)payload_len;
    return MCP_OK;
}

mcp_status_t mcp_ws_encode_frame(mcp_context_t *ctx, const mcp_ws_frame_t *frame,
                                 uint8_t **bytes_out, size_t *len_out) {
    if (frame == NULL || bytes_out == NULL || len_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *bytes_out = NULL;
    *len_out = 0;

    size_t hdr_len = 2;
    if (frame->payload_len <= 125) {
        hdr_len += 0;
    } else if (frame->payload_len <= 0xFFFF) {
        hdr_len += 2;
    } else {
        hdr_len += 8;
    }

    if (frame->masked) {
        hdr_len += 4;
    }

    size_t total_len = hdr_len + (size_t)frame->payload_len;
    uint8_t *out = ws_malloc(ctx, total_len + 1);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }

    out[0] = (frame->fin ? 0x80 : 0x00) | ((uint8_t)frame->opcode & 0x0F);

    size_t idx = 1;
    uint8_t mask_bit = frame->masked ? 0x80 : 0x00;

    if (frame->payload_len <= 125) {
        out[idx++] = mask_bit | (uint8_t)frame->payload_len;
    } else if (frame->payload_len <= 0xFFFF) {
        out[idx++] = mask_bit | 126;
        out[idx++] = (uint8_t)((frame->payload_len >> 8) & 0xFF);
        out[idx++] = (uint8_t)(frame->payload_len & 0xFF);
    } else {
        out[idx++] = mask_bit | 127;
        for (int i = 7; i >= 0; i--) {
            out[idx++] = (uint8_t)((frame->payload_len >> (i * 8)) & 0xFF);
        }
    }

    if (frame->masked) {
        memcpy(out + idx, frame->mask_key, 4);
        idx += 4;
        if (frame->payload_len > 0 && frame->payload != NULL) {
            for (size_t i = 0; i < (size_t)frame->payload_len; i++) {
                out[idx + i] = (uint8_t)(frame->payload[i] ^ frame->mask_key[i % 4]);
            }
        }
    } else {
        if (frame->payload_len > 0 && frame->payload != NULL) {
            memcpy(out + idx, frame->payload, (size_t)frame->payload_len);
        }
    }
    out[total_len] = 0;

    *bytes_out = out;
    *len_out = total_len;
    return MCP_OK;
}

void mcp_ws_frame_destroy(mcp_context_t *ctx, mcp_ws_frame_t *frame) {
    if (frame == NULL) {
        return;
    }
    if (frame->payload != NULL) {
        ws_free(ctx, frame->payload);
        frame->payload = NULL;
    }
    frame->payload_len = 0;
}

typedef struct {
    int fd;
    bool is_server;
    bool closed;
    uint8_t *carry;
    size_t carry_len;
    size_t carry_cap;
} mcp_ws_backend_t;

static mcp_status_t ws_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t ws_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                            size_t len) {
    if (t == NULL || (data == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_ws_backend_t *b = (mcp_ws_backend_t *)mcp_transport_backend(ctx, t);
    if (b == NULL || b->fd < 0 || b->closed) {
        return MCP_ERR_IO;
    }

    mcp_ws_frame_t frame = {
        .fin = true,
        .opcode = MCP_WS_OP_TEXT,
        .masked = !b->is_server,
        .payload_len = len,
        .payload = (char *)data,
    };

    if (frame.masked) {
        uint32_t r = (uint32_t)rand() ^ (uint32_t)time(NULL);
        memcpy(frame.mask_key, &r, 4);
    }

    uint8_t *bytes = NULL;
    size_t out_len = 0;
    mcp_status_t st = mcp_ws_encode_frame(ctx, &frame, &bytes, &out_len);
    if (st != MCP_OK) {
        return st;
    }

    size_t sent = 0;
    while (sent < out_len) {
        ssize_t n = send(b->fd, bytes + sent, out_len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ws_free(ctx, bytes);
            return MCP_ERR_IO;
        }
        if (n == 0) {
            ws_free(ctx, bytes);
            return MCP_ERR_IO;
        }
        sent += (size_t)n;
    }
    ws_free(ctx, bytes);
    return MCP_OK;
}

static mcp_status_t ws_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    if (t == NULL || line_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_ws_backend_t *b = (mcp_ws_backend_t *)mcp_transport_backend(ctx, t);
    if (b == NULL || b->fd < 0 || b->closed) {
        return MCP_ERR_IO;
    }
    *line_out = NULL;

    for (;;) {
        if (b->carry_len > 0) {
            mcp_ws_frame_t frame = {0};
            size_t consumed = 0;
            mcp_status_t st = mcp_ws_parse_frame(ctx, b->carry, b->carry_len, &frame, &consumed);
            if (st != MCP_OK) {
                return st;
            }
            if (consumed > 0) {
                memmove(b->carry, b->carry + consumed, b->carry_len - consumed);
                b->carry_len -= consumed;

                if (frame.opcode == MCP_WS_OP_TEXT || frame.opcode == MCP_WS_OP_BINARY ||
                    frame.opcode == MCP_WS_OP_CONTINUATION) {
                    *line_out = frame.payload;
                    return MCP_OK;
                }
                if (frame.opcode == MCP_WS_OP_PING) {
                    mcp_ws_frame_t pong = {
                        .fin = true,
                        .opcode = MCP_WS_OP_PONG,
                        .masked = !b->is_server,
                        .payload_len = frame.payload_len,
                        .payload = frame.payload,
                    };
                    if (pong.masked) {
                        uint32_t r = (uint32_t)rand();
                        memcpy(pong.mask_key, &r, 4);
                    }
                    uint8_t *pbytes = NULL;
                    size_t plen = 0;
                    if (mcp_ws_encode_frame(ctx, &pong, &pbytes, &plen) == MCP_OK) {
                        send(b->fd, pbytes, plen, MSG_NOSIGNAL);
                        ws_free(ctx, pbytes);
                    }
                    mcp_ws_frame_destroy(ctx, &frame);
                    continue;
                }
                if (frame.opcode == MCP_WS_OP_CLOSE) {
                    b->closed = true;
                    mcp_ws_frame_destroy(ctx, &frame);
                    return MCP_ERR_IO;
                }
                mcp_ws_frame_destroy(ctx, &frame);
                continue;
            }
        }

        if (b->carry_len + 4096 > b->carry_cap) {
            size_t ncap = b->carry_cap == 0 ? 8192 : b->carry_cap * 2;
            if (ncap > (4u << 20)) {
                return MCP_ERR_PROTOCOL;
            }
            uint8_t *ncarry = ws_malloc(ctx, ncap);
            if (ncarry == NULL) {
                return MCP_ERR_NOMEM;
            }
            if (b->carry != NULL && b->carry_len > 0) {
                memcpy(ncarry, b->carry, b->carry_len);
            }
            ws_free(ctx, b->carry);
            b->carry = ncarry;
            b->carry_cap = ncap;
        }

        ssize_t n = recv(b->fd, b->carry + b->carry_len, b->carry_cap - b->carry_len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MCP_ERR_IO;
        }
        if (n == 0) {
            return MCP_ERR_IO;
        }
        b->carry_len += (size_t)n;
    }
}

static mcp_status_t ws_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    mcp_ws_backend_t *b = (mcp_ws_backend_t *)mcp_transport_backend(ctx, t);
    if (b != NULL) {
        if (b->fd >= 0) {
            close(b->fd);
            b->fd = -1;
        }
        if (b->carry != NULL) {
            ws_free(ctx, b->carry);
            b->carry = NULL;
            b->carry_len = 0;
            b->carry_cap = 0;
        }
        ws_free(ctx, b);
    }
    return MCP_OK;
}

static const mcp_transport_ops_t kWsOps = {
    .start = ws_start,
    .send  = ws_send,
    .recv  = ws_recv,
    .stop  = ws_stop,
};

mcp_transport_t *mcp_ws_transport_create(mcp_context_t *ctx, int fd, bool is_server) {
    if (fd < 0) {
        return NULL;
    }
    mcp_ws_backend_t *b = ws_malloc(ctx, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    b->fd = fd;
    b->is_server = is_server;
    b->closed = false;

    mcp_transport_t *t = mcp_transport_create(ctx, &kWsOps, b);
    if (t == NULL) {
        ws_free(ctx, b);
        return NULL;
    }
    return t;
}

mcp_transport_t *mcp_ws_client_connect(mcp_context_t *ctx, const char *url) {
    if (url == NULL) {
        return NULL;
    }

    /* Parse ws://<host>[:<port>][/<path>] */
    const char *prefix = "ws://";
    if (strncmp(url, prefix, 5) != 0) {
        return NULL;
    }
    const char *host_start = url + 5;
    const char *slash = strchr(host_start, '/');
    const char *colon = strchr(host_start, ':');

    char host[256];
    uint16_t port = 80;
    const char *path = slash != NULL ? slash : "/";

    if (colon != NULL && (slash == NULL || colon < slash)) {
        size_t hlen = (size_t)(colon - host_start);
        if (hlen >= sizeof(host)) return NULL;
        memcpy(host, host_start, hlen);
        host[hlen] = '\0';
        port = (uint16_t)atoi(colon + 1);
    } else {
        size_t hlen = slash != NULL ? (size_t)(slash - host_start) : strlen(host_start);
        if (hlen >= sizeof(host)) return NULL;
        memcpy(host, host_start, hlen);
        host[hlen] = '\0';
    }

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return NULL;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return NULL;
    }

    /* Generate 16-byte random key and base64-encode */
    uint8_t rand_bytes[16];
    for (int i = 0; i < 16; i++) {
        rand_bytes[i] = (uint8_t)(rand() & 0xFF);
    }
    static const char kB64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char client_key[25];
    size_t o = 0;
    for (size_t i = 0; i < 15; i += 3) {
        uint32_t v = ((uint32_t)rand_bytes[i] << 16) | ((uint32_t)rand_bytes[i + 1] << 8) | rand_bytes[i + 2];
        client_key[o++] = kB64Chars[(v >> 18) & 0x3F];
        client_key[o++] = kB64Chars[(v >> 12) & 0x3F];
        client_key[o++] = kB64Chars[(v >> 6) & 0x3F];
        client_key[o++] = kB64Chars[v & 0x3F];
    }
    uint32_t v = ((uint32_t)rand_bytes[15] << 16);
    client_key[o++] = kB64Chars[(v >> 18) & 0x3F];
    client_key[o++] = kB64Chars[(v >> 12) & 0x3F];
    client_key[o++] = '=';
    client_key[o++] = '=';
    client_key[o] = '\0';

    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path, host, port, client_key);
    if (n < 0 || (size_t)n >= sizeof(req) || send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) {
        close(fd);
        return NULL;
    }

    /* Read HTTP 101 response */
    char resp_buf[2048] = {0};
    ssize_t rn = recv(fd, resp_buf, sizeof(resp_buf) - 1, 0);
    if (rn <= 0 || strstr(resp_buf, "101 Switching Protocols") == NULL) {
        close(fd);
        return NULL;
    }

    char expected_accept[32];
    if (mcp_ws_compute_accept_key(client_key, expected_accept) != MCP_OK ||
        strstr(resp_buf, expected_accept) == NULL) {
        close(fd);
        return NULL;
    }

    return mcp_ws_transport_create(ctx, fd, false);
}
