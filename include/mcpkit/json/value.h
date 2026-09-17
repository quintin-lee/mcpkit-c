#ifndef MCPKIT_JSON_VALUE_H
#define MCPKIT_JSON_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;

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
 */

/** Discriminant for `mcp_json_type`. */
typedef enum {
    MCP_JSON_NULL = 0, /**< JSON `null`. */
    MCP_JSON_BOOL,     /**< `true` / `false`. */
    MCP_JSON_NUMBER,   /**< Finite double (never NaN or ±Inf). */
    MCP_JSON_STRING,   /**< UTF-8 string. */
    MCP_JSON_ARRAY,    /**< Heterogeneous list. */
    MCP_JSON_OBJECT,   /**< Key-value map. */
} mcp_json_type_t;

/** Opaque JSON DOM node. */
typedef struct mcp_json_value mcp_json_value_t;

/** Returns the node's type; NULL node → `MCP_JSON_NULL`. */
mcp_json_type_t mcp_json_type(mcp_context_t *ctx, const mcp_json_value_t *v);

/**
 * Recursively destroys a value tree. NULL-safe.
 * The ctx used here must be the one that created (or is currently
 * bound to) the value, so the correct allocator is used.
 */
void mcp_json_destroy(mcp_context_t *ctx, mcp_json_value_t *v);

/**
 * Extracts a bool. `MCP_ERR_INVALID_ARGUMENT` if the node is not a
 * bool. The out-param is borrowed from the node; valid only while
 * the node is alive.
 */
mcp_status_t mcp_json_bool_value(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out);

/** Extracts a finite double; `MCP_ERR_INVALID_ARGUMENT` on non-number. */
mcp_status_t mcp_json_number_value(mcp_context_t *ctx, const mcp_json_value_t *v, double *out);

/**
 * Returns a borrowed NUL-terminated pointer into the node's string
 * buffer. The caller must NOT free it; it dies with the node.
 */
mcp_status_t mcp_json_string_value(mcp_context_t *ctx, const mcp_json_value_t *v,
                                   const char **out);

/** Creates a JSON `null` node. NULL on NOMEM. */
mcp_json_value_t *mcp_json_null_new(mcp_context_t *ctx);
/** Creates a JSON bool node. NULL on NOMEM. */
mcp_json_value_t *mcp_json_bool_new(mcp_context_t *ctx, bool b);
/**
 * Creates a JSON number node. Returns NULL if `d` is NaN or ±Inf
 * (non-finite values are not representable in strict JSON).
 */
mcp_json_value_t *mcp_json_number_new(mcp_context_t *ctx, double d);
/**
 * Creates a JSON string from a NUL-terminated UTF-8 source.
 * Embedded NUL bytes are rejected (returns NULL).
 */
mcp_json_value_t *mcp_json_string_new(mcp_context_t *ctx, const char *s);
/**
 * Length-explicit string constructor for buffers that may contain
 * embedded NUL bytes. The buffer must not contain NUL in `[0, n)`.
 */
mcp_json_value_t *mcp_json_string_new_n(mcp_context_t *ctx, const char *s, size_t n);
/** Creates an empty JSON array. NULL on NOMEM. */
mcp_json_value_t *mcp_json_array_new(mcp_context_t *ctx);
/** Creates an empty JSON object. NULL on NOMEM. */
mcp_json_value_t *mcp_json_object_new(mcp_context_t *ctx);

/**
 * Deep-copies a value tree through the active backend (the one
 * currently bound to `ctx`). The copy is fully independent of the
 * source; both can be destroyed separately. Returns NULL on NOMEM
 * or if `v` is NULL.
 */
mcp_json_value_t *mcp_json_clone(mcp_context_t *ctx, const mcp_json_value_t *v);

#endif
