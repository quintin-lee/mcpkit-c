/**
 * @file context.c
 *
 * Context create/destroy and allocator resolution. A context owns
 * exactly one logger: if the config supplies one the context does
 * NOT own it; otherwise a stderr logger is created and owned.
 * The allocator snapshot captured at create time is the one used by
 * destroy, so counting allocators observe the full lifetime.
 */
#include "mcpkit/core/context.h"

#include <stdbool.h>

/**
 * Concrete context layout. Opaque to API consumers; only the core
 * modules may include this definition (via internals headers or
 * direct struct access in this translation unit).
 */
struct mcp_context {
    mcp_allocator_t alloc;
    mcp_logger_t *logger;
    bool owns_logger;          /**< true when the context created a stderr logger */
    const mcp_json_backend_ops_t *json_backend;
};

/**
 * Resolves a possibly-partial user allocator into a complete one:
 * each NULL function pointer is filled from the default (libc)
 * allocator, but `userdata` always comes from the user-supplied
 * struct so mixed allocators stay self-consistent.
 */
static void resolve_allocator(const mcp_allocator_t *in, mcp_allocator_t *out) {
    const mcp_allocator_t *d = mcp_default_allocator();
    const mcp_allocator_t *src = in != NULL ? in : d;
    out->malloc_fn = src->malloc_fn != NULL ? src->malloc_fn : d->malloc_fn;
    out->free_fn = src->free_fn != NULL ? src->free_fn : d->free_fn;
    out->calloc_fn = src->calloc_fn != NULL ? src->calloc_fn : d->calloc_fn;
    out->realloc_fn = src->realloc_fn != NULL ? src->realloc_fn : d->realloc_fn;
    out->userdata = src->userdata;
}

/**
 * Creates a context. Ownership rules:
 * - On success the caller owns the context and must call
 *   `mcp_context_destroy` to release it.
 * - If `config->logger` is non-NULL the context does NOT own it;
 *   the caller must keep the logger alive at least as long as ctx.
 * - A NULL logger (or NULL config) causes a stderr logger to be
 *   created and owned by the context.
 */
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

/**
 * Destroys a context. Frees the logger only when the context created
 * it itself (the stderr default); user-supplied loggers are left
 * untouched. NULL-safe.
 */
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
