#ifndef MCPKIT_SERVER_PROMPT_H
#define MCPKIT_SERVER_PROMPT_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file prompt.h
 * Prompt descriptor: name, description, and a handler that returns a
 * JSON array of chat messages.
 *
 * Ownership: caller owns the prompt from mcp_prompt_new() until
 * mcp_prompt_destroy(). If added to a server, remove it first.
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_prompt mcp_prompt_t;

/**
 * Handler callback. args_or_null is the "arguments" object from the
 * prompts/get request (NULL if absent). *messages_out must be set to an
 * owned JSON array on MCP_OK; the dispatcher destroys it after building
 * the response.
 */
typedef mcp_status_t (*mcp_prompt_get_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                          const mcp_json_value_t *args_or_null, void *user_data,
                                          mcp_json_value_t **messages_out);

mcp_prompt_t *mcp_prompt_new(mcp_context_t *ctx, const char *name,
                              const char *description_or_null,
                              mcp_prompt_get_fn on_get, void *user_data);
void mcp_prompt_destroy(mcp_context_t *ctx, mcp_prompt_t *prompt);

#endif
