#ifndef MCPKIT_JSON_ARRAY_H
#define MCPKIT_JSON_ARRAY_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

mcp_status_t mcp_json_array_append(mcp_context_t *ctx, mcp_json_value_t *arr,
                                   mcp_json_value_t *val);
const mcp_json_value_t *mcp_json_array_get(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                           size_t i);
size_t mcp_json_array_size(mcp_context_t *ctx, const mcp_json_value_t *arr);

#endif
