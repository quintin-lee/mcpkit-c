#ifndef MCPKIT_SERVER_TOOL_H
#define MCPKIT_SERVER_TOOL_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;

typedef mcp_status_t (*mcp_tool_handler_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                            const mcp_json_value_t *args, void *user_data,
                                            mcp_json_value_t **result_out);

mcp_tool_t *mcp_tool_new(mcp_context_t *ctx, const char *name, const char *description,
                         mcp_json_value_t *input_schema, mcp_tool_handler_fn handler,
                         void *user_data);
void mcp_tool_destroy(mcp_context_t *ctx, mcp_tool_t *tool);

#endif
