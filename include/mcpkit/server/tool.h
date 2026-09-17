#ifndef MCPKIT_SERVER_TOOL_H
#define MCPKIT_SERVER_TOOL_H

#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file tool.h
 * Tool descriptor: name, description, input schema, and a handler callback.
 *
 * Ownership:
 * - mcp_tool_new(): caller owns the returned tool.
 *   On MCP_OK the input_schema JSON value is TAKEN into the tool and must
 *   NOT be destroyed separately.
 * - mcp_tool_destroy(): destroys the tool and its stored input_schema.
 *   If the tool was already added to a server, remove it from the server
 *   first (mcp_server_remove_tool).
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;

/**
 * Handler callback. args is a borrowed JSON object (the request's
 * "arguments" field); result_out must be set to an owned JSON value on
 * MCP_OK (the dispatcher clones it for the response and destroys the
 * original).
 */
typedef mcp_status_t (*mcp_tool_handler_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                            const mcp_json_value_t *args, void *user_data,
                                            mcp_json_value_t **result_out);

/**
 * Creates a tool. description and input_schema may be NULL.
 * Returns NULL only on OOM.
 */
mcp_tool_t *mcp_tool_new(mcp_context_t *ctx, const char *name, const char *description,
                          mcp_json_value_t *input_schema, mcp_tool_handler_fn handler,
                          void *user_data);
void mcp_tool_destroy(mcp_context_t *ctx, mcp_tool_t *tool);

/*
 * Visibility controls which MCP hosts may see a tool:
 *   MCP_TOOL_VIS_MODEL - only LLM/model-side hosts
 *   MCP_TOOL_VIS_APP   - only MCP Apps hosts
 *   MCP_TOOL_VIS_BOTH  - both (default)
 *
 * Permission masks (defined in mcpkit/apps/ui.h):
 *   MCP_APPS_PERM_CALL_TOOL  - (1u << 0)
 *   MCP_APPS_PERM_READ_STATE - (1u << 1)
 */
typedef enum mcp_tool_visibility {
    MCP_TOOL_VIS_MODEL = 0,
    MCP_TOOL_VIS_APP = 1,
    MCP_TOOL_VIS_BOTH = 2,
} mcp_tool_visibility_t;

mcp_status_t mcp_tool_set_visibility(mcp_context_t *ctx, mcp_tool_t *tool,
                                     mcp_tool_visibility_t vis);
/**
 * Adds a permission requirement. The tool's handler will fail with
 * MCP_RPC_INVALID_PARAMS if the calling session has not been granted the
 * specified permission bits.
 */
mcp_status_t mcp_tool_require_perms(mcp_context_t *ctx, mcp_tool_t *tool,
                                    uint32_t perm_mask);

#endif
