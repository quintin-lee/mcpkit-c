#ifndef MCPKIT_CORE_CONTEXT_H
#define MCPKIT_CORE_CONTEXT_H

#include "mcpkit/core/types.h"
#include "mcpkit/logging/logger.h"

typedef struct mcp_json_backend_ops mcp_json_backend_ops_t;

typedef struct {
    const mcp_allocator_t *allocator;
    mcp_logger_t *logger;
    const mcp_json_backend_ops_t *json_backend;
} mcp_context_config_t;

mcp_context_t *mcp_context_create(const mcp_context_config_t *config);
void mcp_context_destroy(mcp_context_t *ctx);
const mcp_allocator_t *mcp_context_allocator(mcp_context_t *ctx);
mcp_logger_t *mcp_context_logger(mcp_context_t *ctx);
const mcp_json_backend_ops_t *mcp_context_json_backend(mcp_context_t *ctx);

// INTERNAL: json layer only. Use mcp_json_set_backend() instead.
void mcp_context_set_json_backend(mcp_context_t *ctx, const void *ops);

#endif
