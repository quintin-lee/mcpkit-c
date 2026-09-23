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
 * @brief Tool handler callback signature (V1 — single-shot).
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
 * @brief Per-invocation context for V2 (MRTR-capable) tool handlers.
 *
 * Passed to `mcp_tool_handler_v2_fn` on every call, including subsequent
 * round-trips after the handler previously returned an InputRequiredResult.
 *
 * On the first call both fields are NULL.  On a retry call:
 *   - `input_responses` points to the client-supplied `inputResponses` array
 *     (borrowed; valid for the duration of the handler call).
 *   - `request_state` points to the `requestState` string the handler
 *     embedded in the previous InputRequiredResult (borrowed; valid for the
 *     duration of the handler call).
 */
typedef struct mcp_tool_call_ctx {
    /** Borrowed pointer to the JSON `arguments` object. Same value as the
     *  V1 `args` parameter.  Valid for the lifetime of the handler call. */
    const mcp_json_value_t *args;

    /** Borrowed JSON array from `params.inputResponses`; NULL on first
     *  call or when the client sent no responses. */
    const mcp_json_value_t *input_responses;

    /** Borrowed string from `params.requestState`; NULL on first call. */
    const char *request_state;
} mcp_tool_call_ctx_t;

/**
 * @brief Tool handler callback signature (V2 — MRTR-capable).
 *
 * A V2 handler may either:
 *   1. Return a regular result object (with `resultType: "complete"`, added
 *      automatically by the dispatcher).
 *   2. Return an `InputRequiredResult` built via
 *      `mcp_mrtr_result_input_required_new()`. The dispatcher forwards it
 *      to the client, which will re-issue the call with user responses.
 *
 * @param ctx      Context for allocation.
 * @param session  Current session (borrowed; not owned).
 * @param call_ctx Invocation context carrying args, inputResponses, and
 *                 requestState (all borrowed).
 * @param user_data Opaque pointer passed to mcp_tool_new_v2.
 * @param result_out On MCP_OK, must be set to an owned JSON value (either
 *                   a complete result or an InputRequiredResult).
 * @return MCP_OK on success; any other MCP_ERR_* on handler failure.
 */
typedef mcp_status_t (*mcp_tool_handler_v2_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                                const mcp_tool_call_ctx_t *call_ctx,
                                                void *user_data,
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

/**
 * @brief Creates an MRTR-capable (V2) tool descriptor.
 *
 * Identical to mcp_tool_new but accepts a `mcp_tool_handler_v2_fn` handler.
 * V2 handlers receive a `mcp_tool_call_ctx_t` with `inputResponses` and
 * `requestState` populated on retry calls.  They may return an
 * `InputRequiredResult` to request more user input.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param name Tool name; strdup'd internally; caller may free.
 * @param description Optional description; strdup'd internally; may be NULL.
 * @param input_schema Optional JSON Schema object; TAKEN on MCP_OK. May be NULL.
 * @param handler V2 handler callback.
 * @param user_data Opaque pointer forwarded to the handler.
 * @return Owned mcp_tool_t, or NULL on OOM.
 */
mcp_tool_t *mcp_tool_new_v2(mcp_context_t *ctx, const char *name,
                             const char *description,
                             mcp_json_value_t *input_schema,
                             mcp_tool_handler_v2_fn handler,
                             void *user_data);

#endif
