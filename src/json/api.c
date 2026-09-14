#include "mcpkit/json/json.h"

#include "mcpkit/core/context.h"

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
