#ifndef MCPKIT_PROTOCOL_INITIALIZE_H
#define MCPKIT_PROTOCOL_INITIALIZE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

#define MCP_PROTOCOL_VERSION_LATEST "2025-06-18"

mcp_json_value_t *mcp_initialize_params_new(mcp_context_t *ctx, const char *client_name,
                                            const char *client_version);
mcp_json_value_t *mcp_initialize_params_new_v(mcp_context_t *ctx, const char *protocol_version,
                                              const char *client_name, const char *client_version);
mcp_status_t mcp_initialize_params_validate(mcp_context_t *ctx, const mcp_json_value_t *params);
const char *mcp_protocol_negotiate(mcp_context_t *ctx, const char *client_version);
mcp_json_value_t *mcp_initialize_result_new(mcp_context_t *ctx, const char *server_name,
                                            const char *server_version);
mcp_message_t *mcp_initialized_notification_new(mcp_context_t *ctx);

#endif
