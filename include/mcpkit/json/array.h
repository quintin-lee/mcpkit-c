/**
 * @file array.h
 * JSON array accessors.
 *
 * Ownership contract: `mcp_json_array_append` transfers ownership of
 * `val` to the array on MCP_OK; on error the caller retains it.
 * `mcp_json_array_get` returns a BORROWED pointer valid while the
 * array is alive.
 */

#ifndef MCPKIT_JSON_ARRAY_H
#define MCPKIT_JSON_ARRAY_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


/**
 * @brief Appends a value to the end of a JSON array.
 *
 * On MCP_OK the array owns `val`; on error the caller retains it.
 *
 * @param ctx  Context; NULL uses the default allocator and built-in backend.
 * @param arr  Array to modify; must not be NULL.
 * @param val  Value to append; ownership transfers to `arr` on MCP_OK;
 *             caller retains ownership on error. NULL appends JSON null.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `arr` is not an array.
 */
mcp_status_t mcp_json_array_append(mcp_context_t *ctx, mcp_json_value_t *arr,
                                    mcp_json_value_t *val);

/**
 * @brief Returns a BORROWED pointer to the i-th element.
 *
 * @param ctx  Context.
 * @param arr  Array to query; must not be NULL.
 * @param i    Zero-based element index.
 * @return Borrowed element pointer, or NULL if `i` is out of range.
 */
const mcp_json_value_t *mcp_json_array_get(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                            size_t i);

/**
 * @brief Returns the number of elements in the array.
 *
 * @param ctx  Context.
 * @param arr  Array; NULL returns 0.
 * @return Element count.
 */
size_t mcp_json_array_size(mcp_context_t *ctx, const mcp_json_value_t *arr);

#endif
