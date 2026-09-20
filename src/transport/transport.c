/**
 * @file transport.c
 *
 * Generic transport wrapper over a user-supplied ops table.
 * mcp_transport_destroy is a shell-only teardown: it frees the wrapper
 * struct only; the ops table's stop/free slots (if provided) are
 * responsible for tearing down the backend. NULL-ctx-safe via
 * alloc_of.
 */
#include "mcpkit/transport/transport.h"

#include "internals.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"

struct mcp_transport {
    mcp_transport_ops_t ops;
    void *backend;
    uint64_t read_ms;
    uint64_t write_ms;
};

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

mcp_transport_t *mcp_transport_create(mcp_context_t *ctx, const mcp_transport_ops_t *ops,
                                      void *backend) {
    if (ops == NULL) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    mcp_transport_t *t = a->malloc_fn(sizeof(*t), a->userdata);
    if (t == NULL) {
        return NULL;
    }
    t->ops = *ops;
    t->backend = backend;
    t->read_ms = 0;
    t->write_ms = 0;
    return t;
}

mcp_status_t mcp_transport_set_timeout(mcp_context_t *ctx, mcp_transport_t *t,
                                       uint64_t read_ms, uint64_t write_ms) {
    (void)ctx;
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    t->read_ms = read_ms;
    t->write_ms = write_ms;
    return MCP_OK;
}

mcp_status_t mcp_transport_get_timeout(mcp_context_t *ctx, const mcp_transport_t *t,
                                       uint64_t *read_ms_out, uint64_t *write_ms_out) {
    (void)ctx;
    if (t == NULL || read_ms_out == NULL || write_ms_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *read_ms_out = t->read_ms;
    *write_ms_out = t->write_ms;
    return MCP_OK;
}

void mcp_transport_destroy(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(t, a->userdata);
}

void *mcp_transport_backend(mcp_context_t *ctx, const mcp_transport_t *t) {
    (void)ctx;
    return t != NULL ? t->backend : NULL;
}

void mcp_transport_set_backend(mcp_context_t *ctx, mcp_transport_t *t, void *backend) {
    (void)ctx;
    if (t != NULL) {
        t->backend = backend;
    }
}

mcp_status_t mcp_transport_start(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (t->ops.start == NULL) {
        return MCP_ERR_UNSUPPORTED;
    }
    return t->ops.start(ctx, t);
}

mcp_status_t mcp_transport_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                                size_t len) {
    if (t == NULL || (data == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (t->ops.send == NULL) {
        return MCP_ERR_UNSUPPORTED;
    }
    return t->ops.send(ctx, t, data, len);
}

mcp_status_t mcp_transport_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    if (t == NULL || line_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (t->ops.recv == NULL) {
        return MCP_ERR_UNSUPPORTED;
    }
    return t->ops.recv(ctx, t, line_out);
}

mcp_status_t mcp_transport_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (t->ops.stop == NULL) {
        return MCP_ERR_UNSUPPORTED;
    }
    return t->ops.stop(ctx, t);
}
