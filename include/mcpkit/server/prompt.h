#ifndef MCPKIT_SERVER_PROMPT_H
#define MCPKIT_SERVER_PROMPT_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_prompt mcp_prompt_t;

typedef mcp_status_t (*mcp_prompt_get_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                          const mcp_json_value_t *args_or_null, void *user_data,
                                          mcp_json_value_t **messages_out);

mcp_prompt_t *mcp_prompt_new(mcp_context_t *ctx, const char *name, const char *description_or_null,
                            mcp_prompt_get_fn on_get, void *user_data);
void mcp_prompt_destroy(mcp_context_t *ctx, mcp_prompt_t *prompt);

#endif
