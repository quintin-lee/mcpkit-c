#ifndef MCPKIT_JSON_JSON_H
#define MCPKIT_JSON_JSON_H

#include "mcpkit/json/value.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/array.h"

#define MCP_JSON_MAX_DEPTH 128

typedef struct mcp_json_backend_ops mcp_json_backend_ops_t;
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
    mcp_status_t (*array_append)(mcp_context_t *ctx, mcp_json_value_t *arr,
                                 mcp_json_value_t *val);
    const mcp_json_value_t *(*array_get)(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                         size_t i);
    size_t (*array_size)(mcp_context_t *ctx, const mcp_json_value_t *arr);
};

const mcp_json_backend_ops_t *mcp_json_builtin_backend(void);
void mcp_json_set_backend(mcp_context_t *ctx, const mcp_json_backend_ops_t *ops);

mcp_json_value_t *mcp_json_parse(mcp_context_t *ctx, const char *text, size_t len);
char *mcp_json_serialize(mcp_context_t *ctx, const mcp_json_value_t *v);
void mcp_json_free_string(mcp_context_t *ctx, char *s);

#endif
