/**
 * @file tool.h
 * @brief Tool descriptor: name, description, input schema, and a handler
 *        callback.
 * @ingroup mcpkit-server
 *
 * Ownership:
 * - mcp_tool_new(): caller owns the returned tool.
 *   On MCP_OK the input_schema JSON value is TAKEN into the tool and must
 *   NOT be destroyed separately.
 * - mcp_tool_destroy(): destroys the tool and its stored input_schema.
 *   If the tool was already added to a server, remove it from the server
 *   first (mcp_server_remove_tool).
 */

#ifndef MCPKIT_SERVER_TOOL_H
#define MCPKIT_SERVER_TOOL_H

#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;

/**
 * @brief Tool handler callback signature.
 *
 * @param ctx Context for allocation.
 * @param session Current session (borrowed; not owned).
 * @param args Borrowed JSON object — the request's "arguments" field; not
 *             owned, valid only for the duration of the call.
 * @param user_data Opaque pointer passed to mcp_tool_new.
 * @param result_out On MCP_OK, must be set to an owned JSON value. The
 *                   dispatcher clones it for the response and destroys the
 *                   original.
 * @return MCP_OK on success; any other MCP_ERR_* on handler failure.
 */
typedef mcp_status_t (*mcp_tool_handler_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                            const mcp_json_value_t *args, void *user_data,
                                            mcp_json_value_t **result_out);

/**
 * @brief Creates a tool descriptor.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param name Tool name; strdup'd internally; caller may free.
 * @param description Optional description; strdup'd internally; may be NULL.
 * @param input_schema Optional JSON Schema object; tool TAKES ownership on
 *                     MCP_OK — do not destroy it separately. May be NULL.
 * @param handler Handler callback.
 * @param user_data Opaque pointer forwarded to the handler.
 * @return Owned mcp_tool_t, or NULL on OOM.
 *         Caller destroys with mcp_tool_destroy (or mcp_server_remove_tool
 *         if already registered with a server).
 */
mcp_tool_t *mcp_tool_new(mcp_context_t *ctx, const char *name, const char *description,
                         mcp_json_value_t *input_schema, mcp_tool_handler_fn handler,
                         void *user_data);

/**
 * @brief Destroys a tool and its stored input_schema.
 *
 * If the tool was already added to a server, call
 * mcp_server_remove_tool first; removing from the server destroys the
 * item and a subsequent destroy here would be a double-free.
 *
 * @param ctx Context; may be NULL.
 * @param tool Tool to destroy; NULL is a no-op.
 */
void mcp_tool_destroy(mcp_context_t *ctx, mcp_tool_t *tool);

/**
 * @brief Tool visibility flags control which MCP hosts may see a tool:
 *   - MCP_TOOL_VIS_MODEL: only LLM/model-side hosts
 *   - MCP_TOOL_VIS_APP:   only MCP Apps hosts
 *   - MCP_TOOL_VIS_BOTH:  both (default)
 *
 * Permission masks (defined in mcpkit/apps/ui.h):
 *   - MCP_APPS_PERM_CALL_TOOL:  (1u << 0)
 *   - MCP_APPS_PERM_READ_STATE: (1u << 1)
 */
typedef enum mcp_tool_visibility {
    MCP_TOOL_VIS_MODEL = 0,
    MCP_TOOL_VIS_APP = 1,
    MCP_TOOL_VIS_BOTH = 2,
} mcp_tool_visibility_t;

/**
 * @brief Sets the tool's visibility to a specific host class.
 * @param ctx Context; may be NULL.
 * @param tool Target tool.
 * @param vis One of mcp_tool_visibility_t.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if tool is NULL or
 *         vis is out of range.
 */
mcp_status_t mcp_tool_set_visibility(mcp_context_t *ctx, mcp_tool_t *tool,
                                     mcp_tool_visibility_t vis);

/**
 * @brief Adds a permission requirement to the tool.
 *
 * The tool's handler will fail with MCP_RPC_INVALID_PARAMS (-32602) if the
 * calling session has not been granted all of the specified permission bits.
 *
 * @param ctx Context; may be NULL.
 * @param tool Target tool.
 * @param perm_mask Bitwise OR of permission flags from mcpkit/apps/ui.h.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if tool is NULL.
 */
mcp_status_t mcp_tool_require_perms(mcp_context_t *ctx, mcp_tool_t *tool,
                                    uint32_t perm_mask);

#endif
