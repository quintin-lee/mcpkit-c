/**
 * @file api.c
 *
 * Public JSON API dispatch layer. Every public JSON function resolves
 * the active backend for `ctx` (falling back to the built-in when ctx
 * is NULL or has no custom backend) and delegates. This file contains
 * no JSON logic itself; all parsing/serialization lives in builtin.c
 * (or in user-supplied backend implementations).
 */
#include "mcpkit/json/json.h"

#include "mcpkit/core/context.h"

/**
 * Resolves which backend ops table to use:
 *  1. The backend bound to `ctx`, if one was set.
 *  2. Otherwise the built-in backend.
 * `ctx` NULL → built-in.
 */
static const mcp_json_backend_ops_t *resolve(mcp_context_t *ctx) {
    if (ctx != NULL) {
        const mcp_json_backend_ops_t *ops = mcp_context_json_backend(ctx);
        if (ops != NULL) {
            return ops;
        }
    }
    return mcp_json_builtin_backend();
}

void mcp_json_set_backend(mcp_context_t *ctx, const mcp_json_backend_ops_t *ops) {
    if (ctx == NULL) {
        return;
    }
    mcp_context_set_json_backend(ctx, ops);
}

mcp_json_value_t *mcp_json_parse(mcp_context_t *ctx, const char *text, size_t len) {
    return resolve(ctx)->parse(ctx, text, len);
}

char *mcp_json_serialize(mcp_context_t *ctx, const mcp_json_value_t *v) {
    return resolve(ctx)->serialize(ctx, v);
}

void mcp_json_free_string(mcp_context_t *ctx, char *s) {
    resolve(ctx)->free_string(ctx, s);
}

mcp_json_type_t mcp_json_type(mcp_context_t *ctx, const mcp_json_value_t *v) {
    return resolve(ctx)->type_of(ctx, v);
}

void mcp_json_destroy(mcp_context_t *ctx, mcp_json_value_t *v) {
    resolve(ctx)->destroy(ctx, v);
}

mcp_status_t mcp_json_bool_value(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out) {
    return resolve(ctx)->get_bool(ctx, v, out);
}

mcp_status_t mcp_json_number_value(mcp_context_t *ctx, const mcp_json_value_t *v, double *out) {
    return resolve(ctx)->get_number(ctx, v, out);
}

mcp_status_t mcp_json_string_value(mcp_context_t *ctx, const mcp_json_value_t *v,
                                   const char **out) {
    return resolve(ctx)->get_string(ctx, v, out);
}

mcp_json_value_t *mcp_json_null_new(mcp_context_t *ctx) {
    return resolve(ctx)->new_null(ctx);
}

mcp_json_value_t *mcp_json_bool_new(mcp_context_t *ctx, bool b) {
    return resolve(ctx)->new_bool(ctx, b);
}

mcp_json_value_t *mcp_json_number_new(mcp_context_t *ctx, double d) {
    return resolve(ctx)->new_number(ctx, d);
}

mcp_json_value_t *mcp_json_string_new(mcp_context_t *ctx, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return resolve(ctx)->new_string_n(ctx, s, n);
}

mcp_json_value_t *mcp_json_string_new_n(mcp_context_t *ctx, const char *s, size_t n) {
    return resolve(ctx)->new_string_n(ctx, s, n);
}

mcp_json_value_t *mcp_json_array_new(mcp_context_t *ctx) {
    return resolve(ctx)->new_array(ctx);
}

mcp_json_value_t *mcp_json_object_new(mcp_context_t *ctx) {
    return resolve(ctx)->new_object(ctx);
}

mcp_status_t mcp_json_object_set(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                                 mcp_json_value_t *val) {
    return resolve(ctx)->object_set(ctx, obj, key, val);
}

const mcp_json_value_t *mcp_json_object_get(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                            const char *key) {
    return resolve(ctx)->object_get(ctx, obj, key);
}

bool mcp_json_object_has(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key) {
    return resolve(ctx)->object_has(ctx, obj, key);
}

size_t mcp_json_object_size(mcp_context_t *ctx, const mcp_json_value_t *obj) {
    return resolve(ctx)->object_size(ctx, obj);
}

const char *mcp_json_object_key_at(mcp_context_t *ctx, const mcp_json_value_t *obj, size_t i) {
    return resolve(ctx)->object_key_at(ctx, obj, i);
}

mcp_status_t mcp_json_array_append(mcp_context_t *ctx, mcp_json_value_t *arr,
                                   mcp_json_value_t *val) {
    return resolve(ctx)->array_append(ctx, arr, val);
}

const mcp_json_value_t *mcp_json_array_get(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                           size_t i) {
    return resolve(ctx)->array_get(ctx, arr, i);
}

size_t mcp_json_array_size(mcp_context_t *ctx, const mcp_json_value_t *arr) {
    return resolve(ctx)->array_size(ctx, arr);
}

mcp_json_value_t *mcp_json_clone(mcp_context_t *ctx, const mcp_json_value_t *v) {
    if (v == NULL) {
        return NULL;
    }
    switch (mcp_json_type(ctx, v)) {
        case MCP_JSON_NULL:
            return mcp_json_null_new(ctx);
        case MCP_JSON_BOOL: {
            bool b = false;
            if (mcp_json_bool_value(ctx, v, &b) != MCP_OK) {
                return NULL;
            }
            return mcp_json_bool_new(ctx, b);
        }
        case MCP_JSON_NUMBER: {
            double n = 0;
            if (mcp_json_number_value(ctx, v, &n) != MCP_OK) {
                return NULL;
            }
            return mcp_json_number_new(ctx, n);
        }
        case MCP_JSON_STRING: {
            const char *s = NULL;
            if (mcp_json_string_value(ctx, v, &s) != MCP_OK || s == NULL) {
                return NULL;
            }
            return mcp_json_string_new(ctx, s);
        }
        case MCP_JSON_ARRAY: {
            mcp_json_value_t *out = mcp_json_array_new(ctx);
            if (out == NULL) {
                return NULL;
            }
            for (size_t i = 0, n = mcp_json_array_size(ctx, v); i < n; i++) {
                mcp_json_value_t *item = mcp_json_clone(ctx, mcp_json_array_get(ctx, v, i));
                if (item == NULL || mcp_json_array_append(ctx, out, item) != MCP_OK) {
                    mcp_json_destroy(ctx, item);
                    mcp_json_destroy(ctx, out);
                    return NULL;
                }
            }
            return out;
        }
        case MCP_JSON_OBJECT: {
            mcp_json_value_t *out = mcp_json_object_new(ctx);
            if (out == NULL) {
                return NULL;
            }
            for (size_t i = 0, n = mcp_json_object_size(ctx, v); i < n; i++) {
                const char *key = mcp_json_object_key_at(ctx, v, i);
                mcp_json_value_t *item = mcp_json_clone(ctx, mcp_json_object_get(ctx, v, key));
                if (key == NULL || item == NULL ||
                    mcp_json_object_set(ctx, out, key, item) != MCP_OK) {
                    mcp_json_destroy(ctx, item);
                    mcp_json_destroy(ctx, out);
                    return NULL;
                }
            }
            return out;
        }
    }
    return NULL;
}
