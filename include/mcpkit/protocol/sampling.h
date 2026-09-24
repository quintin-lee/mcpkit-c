/**
 * @file sampling.h
 * @brief Sampling with Tools protocol definitions and builders (SEP-1577).
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_SAMPLING_H
#define MCPKIT_PROTOCOL_SAMPLING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_context mcp_context_t;

/**
 * @brief Tool choice behavior requested by server (SEP-1577).
 */
typedef enum {
    MCP_TOOL_CHOICE_AUTO = 0,
    MCP_TOOL_CHOICE_NONE,
    MCP_TOOL_CHOICE_REQUIRED,
    MCP_TOOL_CHOICE_SPECIFIC
} mcp_sampling_tool_choice_t;

/**
 * @brief Sampling tool definition descriptor.
 */
typedef struct {
    const char *name;
    const char *description;
    const mcp_json_value_t *input_schema;
} mcp_sampling_tool_def_t;

/**
 * @brief Creates a sampling/createMessage request parameters JSON object.
 *
 * Caller owns the returned value.
 *
 * @param ctx Context; may be NULL.
 * @param max_tokens Maximum tokens requested from the model.
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_sampling_params_new(mcp_context_t *ctx, uint32_t max_tokens);

/**
 * @brief Adds a tool definition to the sampling request parameters (SEP-1577).
 *
 * @param ctx Context; may be NULL.
 * @param params Target sampling request parameters object.
 * @param tool Tool definition descriptor to clone and append.
 * @return MCP_OK on success, or an error code.
 */
mcp_status_t mcp_sampling_params_add_tool(mcp_context_t *ctx, mcp_json_value_t *params,
                                          const mcp_sampling_tool_def_t *tool);

/**
 * @brief Sets the tool choice mode on the sampling parameters (SEP-1577).
 *
 * @param ctx Context; may be NULL.
 * @param params Target sampling request parameters object.
 * @param mode Tool choice mode (auto, none, required, specific).
 * @param specific_tool_name Required if mode is MCP_TOOL_CHOICE_SPECIFIC, ignored otherwise.
 * @return MCP_OK on success, or an error code.
 */
mcp_status_t mcp_sampling_params_set_tool_choice(mcp_context_t *ctx, mcp_json_value_t *params,
                                                 mcp_sampling_tool_choice_t mode,
                                                 const char *specific_tool_name);

/**
 * @brief Inspects tools count from a sampling/createMessage params object.
 *
 * @param ctx Context; may be NULL.
 * @param params Request params object.
 * @return Number of tools in params, or 0 if absent/not an array.
 */
size_t mcp_sampling_params_get_tool_count(mcp_context_t *ctx, const mcp_json_value_t *params);

/**
 * @brief Returns borrowed tool definition object at index.
 *
 * @param ctx Context; may be NULL.
 * @param params Request params object.
 * @param index 0-based tool index.
 * @return Borrowed JSON object, or NULL if out of bounds.
 */
const mcp_json_value_t *mcp_sampling_params_get_tool_at(mcp_context_t *ctx,
                                                        const mcp_json_value_t *params,
                                                        size_t index);

/**
 * @brief Parses toolChoice from a sampling/createMessage params object.
 *
 * @param ctx Context; may be NULL.
 * @param params Request params object.
 * @param out_mode Receives parsed mode.
 * @param out_specific_tool_name Optional pointer to receive borrowed specific tool name.
 * @return MCP_OK if found and parsed; NOT_FOUND if absent; INVALID_ARGUMENT if malformed.
 */
mcp_status_t mcp_sampling_params_get_tool_choice(mcp_context_t *ctx, const mcp_json_value_t *params,
                                                 mcp_sampling_tool_choice_t *out_mode,
                                                 const char **out_specific_tool_name);

/**
 * @brief Creates a tool_use content block (SEP-1577).
 *
 * Takes ownership of `input`.
 *
 * @param ctx Context; may be NULL.
 * @param id Tool call identifier (e.g. "call_1").
 * @param name Tool name being invoked.
 * @param input Input arguments object (consumed by call).
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_sampling_content_tool_use_new(mcp_context_t *ctx, const char *id,
                                                    const char *name, mcp_json_value_t *input);

/**
 * @brief Creates a tool_result content block (SEP-1577).
 *
 * @param ctx Context; may be NULL.
 * @param tool_use_id The corresponding tool call id.
 * @param content Result text or JSON string.
 * @param is_error True if tool execution resulted in an error.
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_sampling_content_tool_result_new(mcp_context_t *ctx, const char *tool_use_id,
                                                       const char *content, bool is_error);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_SAMPLING_H */
