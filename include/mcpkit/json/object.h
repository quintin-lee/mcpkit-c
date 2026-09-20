/**
 * @file object.h
 * JSON object accessors.
 *
 * Ownership contract:
 * - `mcp_json_object_set` transfers ownership of `val` to `obj` on
 *   MCP_OK. On error the caller retains ownership and must free it.
 * - `mcp_json_object_set_take` takes `val` on ALL paths: on error `val`
 *   is destroyed and `obj` stays alive (holding earlier children).
 *   After calling it, never touch `val` again.
 * - All getters return BORROWED pointers valid while `obj` is alive.
 * - Re-setting an existing key replaces the old value (the old value
 *   is freed by the container; the new value is taken).
 */

#ifndef MCPKIT_JSON_OBJECT_H
#define MCPKIT_JSON_OBJECT_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


/**
 * @brief Inserts or replaces a key in a JSON object.
 *
 * On MCP_OK the object now owns `val`.
 *
 * @param ctx  Context; NULL uses the default allocator and built-in backend.
 * @param obj  Object to modify; must not be NULL.
 * @param key  Key string (borrowed, not copied by the caller's ownership).
 * @param val  Value to store; ownership transfers to `obj` on MCP_OK;
 *             caller retains ownership on error. NULL stores JSON null.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `obj` is not an object.
 */
mcp_status_t mcp_json_object_set(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                                 mcp_json_value_t *val);

/**
 * @brief Attaches a value to an object, consuming it on all paths.
 *
 * Same as mcp_json_object_set on success (`obj` owns `val`), but on
 * failure `val` is destroyed instead of returned to the caller. `obj`
 * stays alive holding any earlier children; the caller destroys `obj`.
 * After this call, never touch `val` again — it is owned or freed.
 * Sibling values not yet attached stay caller-owned and must be freed.
 * A NULL `val` yields MCP_ERR_INVALID_ARGUMENT without touching `obj`.
 *
 * @param ctx  Context; NULL uses the default allocator and built-in backend.
 * @param obj  Object to modify; must not be NULL.
 * @param key  Key string (borrowed).
 * @param val  Value to store; consumed on all paths.
 * @return MCP_OK on success; backend status (NOMEM/INVALID_ARGUMENT) on error.
 */
mcp_status_t mcp_json_object_set_take(mcp_context_t *ctx, mcp_json_value_t *obj,
                                       const char *key, mcp_json_value_t *val);

/**
 * @brief Returns a BORROWED pointer to the value stored under `key`.
 *
 * @param ctx  Context.
 * @param obj  Object to query; must not be NULL.
 * @param key  Key to look up.
 * @return Borrowed value pointer, or NULL if the key is absent.
 */
const mcp_json_value_t *mcp_json_object_get(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                            const char *key);

/**
 * @brief Returns true if the key is present in the object.
 *
 * @param ctx  Context.
 * @param obj  Object to query.
 * @param key  Key to check.
 * @return True if present; false otherwise.
 */
bool mcp_json_object_has(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);

/**
 * @brief Returns the number of keys in the object.
 *
 * @param ctx  Context.
 * @param obj  Object; NULL returns 0.
 * @return Key count.
 */
size_t mcp_json_object_size(mcp_context_t *ctx, const mcp_json_value_t *obj);

/**
 * @brief Returns a BORROWED pointer to the i-th key (insertion order).
 *
 * @param ctx  Context.
 * @param obj  Object to query.
 * @param i    Zero-based key index.
 * @return Borrowed key string, or NULL if `i` is out of range.
 */
const char *mcp_json_object_key_at(mcp_context_t *ctx, const mcp_json_value_t *obj, size_t i);

#endif
