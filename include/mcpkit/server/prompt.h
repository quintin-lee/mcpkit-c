/**
 * @file prompt.h
 * @brief Prompt descriptor: name, description, and a handler that returns
 *        a JSON array of chat messages.
 *
 * Ownership: caller owns the prompt from mcp_prompt_new() until
 * mcp_prompt_destroy(). If added to a server, remove it first.
 */

#ifndef MCPKIT_SERVER_PROMPT_H
#define MCPKIT_SERVER_PROMPT_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_prompt mcp_prompt_t;

/**
 * @brief Prompt handler callback signature.
 *
 * @param ctx Context for allocation.
 * @param session Current session (borrowed; not owned).
 * @param args_or_null The "arguments" object from the prompts/get request;
 *                     NULL if absent. Borrowed, not owned.
 * @param user_data Opaque pointer passed to mcp_prompt_new.
 * @param messages_out On MCP_OK, must be set to an owned JSON array of
 *                     message objects. The dispatcher destroys it after
 *                     building the response.
 * @return MCP_OK on success; any other MCP_ERR_* on handler failure.
 */
typedef mcp_status_t (*mcp_prompt_get_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                          const mcp_json_value_t *args_or_null, void *user_data,
                                          mcp_json_value_t **messages_out);

/**
 * @brief Creates a prompt descriptor.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param name Prompt name; strdup'd internally; caller may free.
 * @param description_or_null Optional description; strdup'd internally; may be NULL.
 * @param on_get Handler callback.
 * @param user_data Opaque pointer forwarded to the handler.
 * @return Owned mcp_prompt_t, or NULL on OOM.
 *         Caller destroys with mcp_prompt_destroy (or
 *         mcp_server_remove_prompt if already registered).
 */
mcp_prompt_t *mcp_prompt_new(mcp_context_t *ctx, const char *name,
                             const char *description_or_null,
                             mcp_prompt_get_fn on_get, void *user_data);

/**
 * @brief Destroys a prompt descriptor.
 * @param ctx Context; may be NULL.
 * @param prompt Prompt to destroy; NULL is a no-op.
 */
void mcp_prompt_destroy(mcp_context_t *ctx, mcp_prompt_t *prompt);

#endif
