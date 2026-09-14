#include "mcpkit/core/context.h"

#include <stdbool.h>

struct mcp_context {
    mcp_allocator_t alloc;
    mcp_logger_t *logger;
    bool owns_logger;
    const mcp_json_backend_ops_t *json_backend;
};

static void resolve_allocator(const mcp_allocator_t *in, mcp_allocator_t *out) {
    const mcp_allocator_t *d = mcp_default_allocator();
    const mcp_allocator_t *src = in != NULL ? in : d;
    out->malloc_fn = src->malloc_fn != NULL ? src->malloc_fn : d->malloc_fn;
    out->free_fn = src->free_fn != NULL ? src->free_fn : d->free_fn;
    out->calloc_fn = src->calloc_fn != NULL ? src->calloc_fn : d->calloc_fn;
    out->realloc_fn = src->realloc_fn != NULL ? src->realloc_fn : d->realloc_fn;
    out->userdata = src->userdata;
}

mcp_context_t *mcp_context_create(const mcp_context_config_t *config) {
    mcp_allocator_t alloc;
    resolve_allocator(config != NULL ? config->allocator : NULL, &alloc);
    mcp_context_t *ctx = alloc.malloc_fn(sizeof(*ctx), alloc.userdata);
    if (ctx == NULL) {
        return NULL;
    }
    ctx->alloc = alloc;
    ctx->owns_logger = false;
    ctx->json_backend = NULL;
    if (config != NULL && config->logger != NULL) {
        ctx->logger = config->logger;
    } else {
        ctx->logger = mcp_logger_default_stderr(&ctx->alloc);
        if (ctx->logger == NULL) {
            alloc.free_fn(ctx, alloc.userdata);
            return NULL;
        }
        ctx->owns_logger = true;
    }
    if (config != NULL) {
        ctx->json_backend = config->json_backend;
    }
    return ctx;
}

void mcp_context_destroy(mcp_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->owns_logger) {
        mcp_logger_destroy(ctx->logger);
    }
    ctx->alloc.free_fn(ctx, ctx->alloc.userdata);
}

const mcp_allocator_t *mcp_context_allocator(mcp_context_t *ctx) {
    if (ctx == NULL) {
        return mcp_default_allocator();
    }
    return &ctx->alloc;
}

mcp_logger_t *mcp_context_logger(mcp_context_t *ctx) {
    if (ctx == NULL) {
        return NULL;
    }
    return ctx->logger;
}

const mcp_json_backend_ops_t *mcp_context_json_backend(mcp_context_t *ctx) {
    if (ctx == NULL) {
        return NULL;
    }
    return ctx->json_backend;
}

void mcp_context_set_json_backend(mcp_context_t *ctx, const void *ops) {
    if (ctx != NULL) {
        ctx->json_backend = ops;
    }
}
