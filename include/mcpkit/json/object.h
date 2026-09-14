#ifndef MCPKIT_JSON_OBJECT_H
#define MCPKIT_JSON_OBJECT_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

mcp_status_t mcp_json_object_set(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                                 mcp_json_value_t *val);
const mcp_json_value_t *mcp_json_object_get(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                            const char *key);
bool mcp_json_object_has(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);
size_t mcp_json_object_size(mcp_context_t *ctx, const mcp_json_value_t *obj);
const char *mcp_json_object_key_at(mcp_context_t *ctx, const mcp_json_value_t *obj, size_t i);

#endif
