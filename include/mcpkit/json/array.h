#ifndef MCPKIT_JSON_ARRAY_H
#define MCPKIT_JSON_ARRAY_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file array.h
 * JSON array accessors.
 *
 * Ownership contract: `mcp_json_array_append` transfers ownership of
 * `val` to the array on MCP_OK; on error the caller retains it.
 * `mcp_json_array_get` returns a BORROWED pointer valid while the
 * array is alive.
 */

/**
 * Appends a value to the end of the array. On MCP_OK the array owns
 * `val`. Returns `MCP_ERR_INVALID_ARGUMENT` if `arr` is not an array.
 * NULL `val` appends a JSON null.
 */
mcp_status_t mcp_json_array_append(mcp_context_t *ctx, mcp_json_value_t *arr,
                                   mcp_json_value_t *val);

/** Returns a BORROWED pointer to the i-th element, or NULL out of range. */
const mcp_json_value_t *mcp_json_array_get(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                            size_t i);

/** Returns the number of elements in the array. */
size_t mcp_json_array_size(mcp_context_t *ctx, const mcp_json_value_t *arr);

#endif
