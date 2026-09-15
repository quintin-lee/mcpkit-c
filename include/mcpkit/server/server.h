#ifndef MCPKIT_SERVER_SERVER_H
#define MCPKIT_SERVER_SERVER_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_prompt mcp_prompt_t;

mcp_server_t *mcp_server_create(mcp_context_t *ctx, const char *name, const char *version);
void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *server);

mcp_status_t mcp_server_add_tool(mcp_context_t *ctx, mcp_server_t *server, mcp_tool_t *tool);
mcp_status_t mcp_server_add_resource(mcp_context_t *ctx, mcp_server_t *server,
                                     mcp_resource_t *res);
mcp_status_t mcp_server_add_prompt(mcp_context_t *ctx, mcp_server_t *server, mcp_prompt_t *prompt);
mcp_status_t mcp_server_remove_tool(mcp_context_t *ctx, mcp_server_t *server, const char *name);
mcp_status_t mcp_server_remove_resource(mcp_context_t *ctx, mcp_server_t *server, const char *uri);
mcp_status_t mcp_server_remove_prompt(mcp_context_t *ctx, mcp_server_t *server, const char *name);

mcp_session_t *mcp_server_create_session(mcp_context_t *ctx, mcp_server_t *server);
void mcp_server_destroy_session(mcp_context_t *ctx, mcp_server_t *server, mcp_session_t *session);

mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *server,
                                 mcp_session_t *session, const mcp_message_t *req,
                                 mcp_message_t **resp_out);
mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *server,
                               mcp_session_t *session, const mcp_message_t *notif);

#endif
