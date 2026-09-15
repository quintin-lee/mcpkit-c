#ifndef MCPKIT_JSON_VALUE_H
#define MCPKIT_JSON_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;

typedef enum {
    MCP_JSON_NULL = 0,
    MCP_JSON_BOOL,
    MCP_JSON_NUMBER,
    MCP_JSON_STRING,
    MCP_JSON_ARRAY,
    MCP_JSON_OBJECT,
} mcp_json_type_t;

typedef struct mcp_json_value mcp_json_value_t;

mcp_json_type_t mcp_json_type(mcp_context_t *ctx, const mcp_json_value_t *v);
void mcp_json_destroy(mcp_context_t *ctx, mcp_json_value_t *v);
mcp_status_t mcp_json_bool_value(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out);
mcp_status_t mcp_json_number_value(mcp_context_t *ctx, const mcp_json_value_t *v, double *out);
mcp_status_t mcp_json_string_value(mcp_context_t *ctx, const mcp_json_value_t *v, const char **out);

mcp_json_value_t *mcp_json_null_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_json_bool_new(mcp_context_t *ctx, bool b);
mcp_json_value_t *mcp_json_number_new(mcp_context_t *ctx, double d);
mcp_json_value_t *mcp_json_string_new(mcp_context_t *ctx, const char *s);
mcp_json_value_t *mcp_json_string_new_n(mcp_context_t *ctx, const char *s, size_t n);
mcp_json_value_t *mcp_json_array_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_json_object_new(mcp_context_t *ctx);

// Deep copy through the active backend. Returns NULL on NOMEM or bad input.
mcp_json_value_t *mcp_json_clone(mcp_context_t *ctx, const mcp_json_value_t *v);

#endif
