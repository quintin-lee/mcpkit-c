#ifndef MCPKIT_JSON_SCHEMA_H
#define MCPKIT_JSON_SCHEMA_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;

mcp_json_value_t *mcp_schema_object_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_string_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_integer_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_number_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_boolean_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_array_new(mcp_context_t *ctx, mcp_json_value_t *items);
mcp_status_t mcp_schema_add_property(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name, mcp_json_value_t *prop);
mcp_status_t mcp_schema_add_required(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name);
mcp_status_t mcp_schema_add_enum(mcp_context_t *ctx, mcp_json_value_t *schema,
                                 mcp_json_value_t *values);
mcp_status_t mcp_schema_set_minimum(mcp_context_t *ctx, mcp_json_value_t *schema, double min);
mcp_status_t mcp_schema_set_maximum(mcp_context_t *ctx, mcp_json_value_t *schema, double max);
mcp_status_t mcp_schema_set_description(mcp_context_t *ctx, mcp_json_value_t *schema,
                                        const char *desc);
mcp_status_t mcp_schema_validate(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                 const mcp_json_value_t *instance);
mcp_status_t mcp_schema_validate_verbose(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                         const mcp_json_value_t *instance,
                                         char *buf, size_t cap);

#endif
