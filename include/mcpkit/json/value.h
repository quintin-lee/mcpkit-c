/**
 * @file value.h
 * JSON DOM node types, constructors, and accessors.
 *
 * Ownership rule that applies to every function in this file and in
 * object.h / array.h:
 *
 * - All constructors return a caller-OWNED value; destroy it with
 *   `mcp_json_destroy` (or transfer ownership into a container via
 *   `mcp_json_object_set` / `mcp_json_array_append`).
 * - `mcp_json_destroy` NULL-safely releases a whole value tree.
 * - Accessors that take a `const mcp_json_value_t *` return BORROWED
 *   pointers (out-params point inside the node; no copy is made).
 *
 * A NULL `ctx` is accepted by every function and routes through the
 * default (libc) allocator plus the built-in JSON backend.
 *
 * @ingroup mcpkit-json
 */

#ifndef MCPKIT_JSON_VALUE_H
#define MCPKIT_JSON_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;


/**
 * @brief Discriminant for `mcp_json_type`.
 */
typedef enum {
    MCP_JSON_NULL = 0, /**< JSON `null`. */
    MCP_JSON_BOOL,     /**< `true` / `false`. */
    MCP_JSON_NUMBER,   /**< Finite double (never NaN or ±Inf). */
    MCP_JSON_STRING,   /**< UTF-8 string. */
    MCP_JSON_ARRAY,    /**< Heterogeneous list. */
    MCP_JSON_OBJECT,   /**< Key-value map. */
} mcp_json_type_t;

/**
 * @brief Opaque JSON DOM node.
 */
typedef struct mcp_json_value mcp_json_value_t;

/**
 * @brief Returns the node's type.
 *
 * @param ctx  Context.
 * @param v    Node to inspect; NULL returns `MCP_JSON_NULL`.
 * @return The node's JSON type.
 */
mcp_json_type_t mcp_json_type(mcp_context_t *ctx, const mcp_json_value_t *v);

/**
 * @brief Recursively destroys a value tree. NULL-safe.
 *
 * The ctx used here must be the one that created (or is currently
 * bound to) the value, so the correct allocator is used.
 *
 * @param ctx  Context that owns the value's allocator.
 * @param v    Value to destroy; NULL is a no-op.
 */
void mcp_json_destroy(mcp_context_t *ctx, mcp_json_value_t *v);

/**
 * @brief Extracts a bool value from a node.
 *
 * @param ctx  Context.
 * @param v    Node to read.
 * @param out  Out-parameter; points into the node (borrowed).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `v` is not a bool.
 */
mcp_status_t mcp_json_bool_value(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out);

/**
 * @brief Extracts a finite double value from a node.
 *
 * @param ctx  Context.
 * @param v    Node to read.
 * @param out  Out-parameter.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `v` is not a number.
 */
mcp_status_t mcp_json_number_value(mcp_context_t *ctx, const mcp_json_value_t *v, double *out);

/**
 * @brief Returns a BORROWED NUL-terminated pointer into the node's string buffer.
 *
 * The caller must NOT free it; it dies with the node.
 *
 * @param ctx  Context.
 * @param v    Node to read.
 * @param out  Out-parameter; points into the node (borrowed).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `v` is not a string.
 */
mcp_status_t mcp_json_string_value(mcp_context_t *ctx, const mcp_json_value_t *v,
                                    const char **out);

/**
 * @brief Creates a JSON `null` node.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_json_null_new(mcp_context_t *ctx);

/**
 * @brief Creates a JSON bool node.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @param b    The boolean value to store.
 * @return Caller-owned value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_json_bool_new(mcp_context_t *ctx, bool b);

/**
 * @brief Creates a JSON number node.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @param d    Finite double to store. NaN and ±Inf are rejected.
 * @return Caller-owned value, or NULL if `d` is non-finite or NOMEM.
 */
mcp_json_value_t *mcp_json_number_new(mcp_context_t *ctx, double d);

/**
 * @brief Creates a JSON string from a NUL-terminated UTF-8 source.
 *
 * Embedded NUL bytes are rejected.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @param s    NUL-terminated UTF-8 source string.
 * @return Caller-owned value, or NULL on NOMEM or embedded NUL.
 */
mcp_json_value_t *mcp_json_string_new(mcp_context_t *ctx, const char *s);

/**
 * @brief Creates a JSON string from a length-explicit buffer.
 *
 * The buffer must not contain NUL in `[0, n)`.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @param s    Byte buffer.
 * @param n    Length in bytes.
 * @return Caller-owned value, or NULL on NOMEM or embedded NUL.
 */
mcp_json_value_t *mcp_json_string_new_n(mcp_context_t *ctx, const char *s, size_t n);

/**
 * @brief Creates an empty JSON array.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_json_array_new(mcp_context_t *ctx);

/**
 * @brief Creates an empty JSON object.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_json_object_new(mcp_context_t *ctx);

/**
 * @brief Deep-copies a value tree through the active backend.
 *
 * The copy is fully independent of the source; both can be destroyed
 * separately.
 *
 * @param ctx  Context; NULL uses the default allocator and built-in backend.
 * @param v    Source value; NULL returns NULL.
 * @return Caller-owned copy, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_json_clone(mcp_context_t *ctx, const mcp_json_value_t *v);

#endif
