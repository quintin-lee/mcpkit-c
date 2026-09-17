/**
 * @file schema.h
 * JSON Schema builder and validator.
 *
 * All builders return caller-owned JSON values representing a JSON
 * Schema fragment. `mcp_schema_add_*` functions take ownership of
 * the schema value on MCP_OK; on error the caller retains it.
 *
 * `mcp_schema_validate` recursively checks an instance against a
 * schema supporting: type (string/number/integer/boolean/object/
 * array), required, properties, items, enum, minimum, maximum.
 */

#ifndef MCPKIT_JSON_SCHEMA_H
#define MCPKIT_JSON_SCHEMA_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;


/**
 * @brief Creates a new JSON Schema for an object type.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned schema value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_schema_object_new(mcp_context_t *ctx);

/**
 * @brief Creates a new JSON Schema for a string type.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned schema value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_schema_string_new(mcp_context_t *ctx);

/**
 * @brief Creates a new JSON Schema for an integer type.
 *
 * The integer check uses a ±2⁵³-1 boundary: a finite double is
 * accepted as an integer only if it is exactly representable.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned schema value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_schema_integer_new(mcp_context_t *ctx);

/**
 * @brief Creates a new JSON Schema for a number type.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned schema value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_schema_number_new(mcp_context_t *ctx);

/**
 * @brief Creates a new JSON Schema for a boolean type.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned schema value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_schema_boolean_new(mcp_context_t *ctx);

/**
 * @brief Creates a new JSON Schema for an array type.
 *
 * @param ctx    Context; NULL uses the default allocator.
 * @param items  Schema for array elements; ownership transfers on MCP_OK.
 * @return Caller-owned schema value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_schema_array_new(mcp_context_t *ctx, mcp_json_value_t *items);

/**
 * @brief Adds a property to an object schema.
 *
 * @param ctx     Context.
 * @param schema  Object schema to modify.
 * @param name    Property name.
 * @param prop    Property schema; ownership transfers on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `schema` is not an object.
 */
mcp_status_t mcp_schema_add_property(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name, mcp_json_value_t *prop);

/**
 * @brief Adds a property name to the `required` array of an object schema.
 *
 * @param ctx     Context.
 * @param schema  Object schema to modify.
 * @param name    Property name to mark as required.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `schema` is not an object.
 */
mcp_status_t mcp_schema_add_required(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name);

/**
 * @brief Adds an `enum` constraint to a schema.
 *
 * @param ctx     Context.
 * @param schema  Schema to modify.
 * @param values  Array of allowed enum values; ownership transfers on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `values` is not an array.
 */
mcp_status_t mcp_schema_add_enum(mcp_context_t *ctx, mcp_json_value_t *schema,
                                 mcp_json_value_t *values);

/**
 * @brief Sets the `minimum` constraint on a number/integer schema.
 *
 * @param ctx     Context.
 * @param schema  Schema to modify.
 * @param min     Minimum allowed value (inclusive).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `schema` is not a number type.
 */
mcp_status_t mcp_schema_set_minimum(mcp_context_t *ctx, mcp_json_value_t *schema, double min);

/**
 * @brief Sets the `maximum` constraint on a number/integer schema.
 *
 * @param ctx     Context.
 * @param schema  Schema to modify.
 * @param max     Maximum allowed value (inclusive).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `schema` is not a number type.
 */
mcp_status_t mcp_schema_set_maximum(mcp_context_t *ctx, mcp_json_value_t *schema, double max);

/**
 * @brief Sets the `description` field on a schema.
 *
 * @param ctx     Context.
 * @param schema  Schema to modify.
 * @param desc    Description string (borrowed; not copied).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if `schema` is not a schema object.
 */
mcp_status_t mcp_schema_set_description(mcp_context_t *ctx, mcp_json_value_t *schema,
                                        const char *desc);

/**
 * @brief Validates an instance against a schema.
 *
 * Recursively checks type, required, properties, items, enum,
 * minimum, and maximum constraints.
 *
 * @param ctx       Context.
 * @param schema    Schema to validate against.
 * @param instance  JSON value to check.
 * @return MCP_OK if valid; MCP_ERR_INVALID_ARGUMENT if invalid.
 */
mcp_status_t mcp_schema_validate(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                 const mcp_json_value_t *instance);

/**
 * @brief Validates an instance and writes a human-readable error to a buffer.
 *
 * If validation fails, a descriptive path message (e.g. `"$.items[0].name:
 * required property missing"`) is written to `buf`.
 *
 * @param ctx       Context.
 * @param schema    Schema to validate against.
 * @param instance  JSON value to check.
 * @param buf       Output buffer; may be NULL if `cap` is 0.
 * @param cap       Buffer size in bytes.
 * @return MCP_OK if valid; MCP_ERR_INVALID_ARGUMENT if invalid.
 */
mcp_status_t mcp_schema_validate_verbose(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                        const mcp_json_value_t *instance,
                                        char *buf, size_t cap);

#endif
