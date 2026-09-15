#include "mcpkit/transport/stdio.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/transport.h"

typedef struct {
    FILE *in;
    FILE *out;
} stdio_backend_t;

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
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

static mcp_status_t stdio_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    if (t == NULL || line_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    stdio_backend_t *b = mcp_transport_backend(ctx, t);
    if (b == NULL || b->in == NULL) {
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
        int c = fgetc(b->in);
        if (c == EOF) {
            if (len == 0) {
                a->free_fn(buf, a->userdata);
                *line_out = NULL;
                return MCP_ERR_IO;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (len + 1 >= cap) {
            if (cap >= MCP_PROTOCOL_MAX_MESSAGE_BYTES) {
                int d;
                do {
                    d = fgetc(b->in);
                } while (d != EOF && d != '\n');
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
        buf[len++] = (char)c;
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
    a->free_fn(mcp_transport_backend(ctx, t), a->userdata);
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
    mcp_transport_t *t = mcp_transport_create(ctx, &kStdioOps, b);
    if (t == NULL) {
        a->free_fn(b, a->userdata);
        return NULL;
    }
    return t;
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

mcp_status_t mcp_stdio_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t) {
    if (server == NULL || t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_session_t *sess = mcp_server_create_session(ctx, server);
    if (sess == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t status = MCP_OK;
    for (;;) {
        char *line = NULL;
        mcp_status_t st = mcp_transport_recv(ctx, t, &line);
        if (st == MCP_ERR_IO) {
            break;
        }
        if (st != MCP_OK) {
            status = st;
            break;
        }
        mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
        mcp_json_free_string(ctx, line);
        if (msg == NULL) {
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
        mcp_message_t *resp = NULL;
        st = mcp_server_dispatch(ctx, server, sess, msg, &resp);
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
    mcp_server_destroy_session(ctx, server, sess);
    return status;
}
