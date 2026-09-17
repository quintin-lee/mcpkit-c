#ifndef MCPKIT_JSON_OBJECT_H
#define MCPKIT_JSON_OBJECT_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file object.h
 * JSON object accessors.
 *
 * Ownership contract:
 * - `mcp_json_object_set` transfers ownership of `val` to `obj` on
 *   MCP_OK. On error the caller retains ownership and must free it.
 * - All getters return BORROWED pointers valid while `obj` is alive.
 * - Re-setting an existing key replaces the old value (the old value
 *   is freed by the container; the new value is taken).
 */

/**
 * Inserts or replaces a key. On MCP_OK the object now owns `val`.
 * Returns `MCP_ERR_INVALID_ARGUMENT` if `obj` is not an object.
 * NULL `val` is allowed (stores a JSON null under `key`).
 */
mcp_status_t mcp_json_object_set(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                                 mcp_json_value_t *val);

/**
 * Returns a BORROWED pointer to the value stored under `key`, or
 * NULL if the key is absent. The pointer is valid only while `obj`
 * is alive.
 */
const mcp_json_value_t *mcp_json_object_get(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                            const char *key);

/** Returns true if the key is present. */
bool mcp_json_object_has(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);

/** Returns the number of keys in the object. */
size_t mcp_json_object_size(mcp_context_t *ctx, const mcp_json_value_t *obj);

/**
 * Returns a BORROWED pointer to the i-th key (insertion order).
 * Returns NULL if `i` is out of range.
 */
const char *mcp_json_object_key_at(mcp_context_t *ctx, const mcp_json_value_t *obj, size_t i);

#endif
