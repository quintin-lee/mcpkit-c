/**
 * @file json.h
 * Pluggable JSON backend interface.
 *
 * The SDK's JSON layer is a thin dispatch layer: every public JSON
 * function resolves a backend pointer and then calls through the
 * `mcp_json_backend_ops_t` table. The built-in backend (a hand-written
 * strict-JSON DOM parser, UTF-8-validated via the Hoehrmann DFA,
 * depth-capped at 128) is registered automatically.
 *
 * A context may be re-pointed to a different backend at any time
 * with `mcp_json_set_backend`; values created through one backend
 * MUST be destroyed through the same backend.
 *
 * @defgroup mcpkit-json JSON DOM
 * @brief Pluggable JSON value tree: parse, serialize, clone, schema check.
 * @ingroup mcpkit-json
 * @see mcpkit-protocol, mcpkit-server
 */

#ifndef MCPKIT_JSON_JSON_H
#define MCPKIT_JSON_JSON_H

#include "mcpkit/json/value.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/array.h"


/**
 * @brief Maximum nesting depth accepted by the built-in parser.
 */
#define MCP_JSON_MAX_DEPTH 128

typedef struct mcp_json_backend_ops mcp_json_backend_ops_t;

/**
 * @brief Function-pointer table for JSON backend implementations.
 *
 * All functions take the context first so backends can allocate
 * through the context's allocator.
 *
 * @note Contract summary:
 *       - `parse`        - returns a caller-owned value, or NULL on error.
 *       - `serialize`    - returns a caller-owned NUL-terminated string
 *                          (free with `free_string`, NOT with `free`).
 *       - `free_string`  - releases a string returned by `serialize`.
 *       - `destroy`      - recursively releases a value tree.
 *       - `object_set`   - on MCP_OK the container takes ownership of `val`;
 *                          on error the caller retains it.
 *       - `array_append` - same ownership contract as `object_set`.
 *       - `object_get` / `array_get` / `object_key_at` - return BORROWED
 *                          pointers valid only while the container is alive.
 *       - `new_number`   - must reject NaN and ±Inf (non-finite values are
 *                          not representable in strict JSON).
 */
struct mcp_json_backend_ops {
    const char *name;
    mcp_json_value_t *(*parse)(mcp_context_t *ctx, const char *text, size_t len);
    char *(*serialize)(mcp_context_t *ctx, const mcp_json_value_t *value);
    void (*free_string)(mcp_context_t *ctx, char *s);
    void (*destroy)(mcp_context_t *ctx, mcp_json_value_t *value);
    mcp_json_type_t (*type_of)(mcp_context_t *ctx, const mcp_json_value_t *value);
    mcp_status_t (*get_bool)(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out);
    mcp_status_t (*get_number)(mcp_context_t *ctx, const mcp_json_value_t *v, double *out);
    mcp_status_t (*get_string)(mcp_context_t *ctx, const mcp_json_value_t *v, const char **out);
    mcp_json_value_t *(*new_null)(mcp_context_t *ctx);
    mcp_json_value_t *(*new_bool)(mcp_context_t *ctx, bool b);
    mcp_json_value_t *(*new_number)(mcp_context_t *ctx, double d);
    mcp_json_value_t *(*new_string_n)(mcp_context_t *ctx, const char *s, size_t n);
    mcp_json_value_t *(*new_array)(mcp_context_t *ctx);
    mcp_json_value_t *(*new_object)(mcp_context_t *ctx);
    mcp_status_t (*object_set)(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                               mcp_json_value_t *val);
    const mcp_json_value_t *(*object_get)(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                          const char *key);
    bool (*object_has)(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);
    size_t (*object_size)(mcp_context_t *ctx, const mcp_json_value_t *obj);
    const char *(*object_key_at)(mcp_context_t *ctx, const mcp_json_value_t *obj, size_t i);
    mcp_status_t (*array_append)(mcp_context_t *ctx, mcp_json_value_t *arr,
                                 mcp_json_value_t *val);
    const mcp_json_value_t *(*array_get)(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                         size_t i);
    size_t (*array_size)(mcp_context_t *ctx, const mcp_json_value_t *arr);
};

/**
 * @brief Returns the built-in backend's ops table.
 *
 * @return Borrowed ops table, valid for the lifetime of the process.
 */
const mcp_json_backend_ops_t *mcp_json_builtin_backend(void);

/**
 * @brief Re-points a context to a custom JSON backend.
 *
 * Pass NULL to restore the built-in backend.
 *
 * @note Existing values created through the old backend must still be
 *       destroyed through that old backend's `destroy` (the context
 *       stores the ops table, so this works via the same ctx).
 *
 * @param ctx  Context; NULL is a no-op.
 * @param ops  Ops table to bind, or NULL for built-in.
 */
void mcp_json_set_backend(mcp_context_t *ctx, const mcp_json_backend_ops_t *ops);

/**
 * @brief Parses strict JSON.
 *
 * @param ctx   Context; NULL uses the default libc allocator and built-in backend.
 * @param text  JSON text.
 * @param len   Length in bytes.
 * @return Caller-owned value, or NULL on any parse error
 *         (malformed input, depth > MCP_JSON_MAX_DEPTH, non-finite number, NOMEM).
 */
mcp_json_value_t *mcp_json_parse(mcp_context_t *ctx, const char *text, size_t len);

/**
 * @brief Serializes a value to a canonical, minimal JSON string.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @param v    Value to serialize.
 * @return Caller-owned NUL-terminated buffer (free with
 *         `mcp_json_free_string` using the SAME ctx), or NULL on NOMEM.
 */
char *mcp_json_serialize(mcp_context_t *ctx, const mcp_json_value_t *v);

/**
 * @brief Releases a string returned by `mcp_json_serialize`.
 *
 * @param ctx  Context; NULL falls back to the default allocator.
 * @param s    String to free; NULL is a no-op.
 */
void mcp_json_free_string(mcp_context_t *ctx, char *s);

#endif
