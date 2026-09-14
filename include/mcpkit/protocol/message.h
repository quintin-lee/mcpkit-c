#ifndef MCPKIT_PROTOCOL_MESSAGE_H
#define MCPKIT_PROTOCOL_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;

typedef enum {
    MCP_RPC_PARSE_ERROR = -32700,
    MCP_RPC_INVALID_REQUEST = -32600,
    MCP_RPC_METHOD_NOT_FOUND = -32601,
    MCP_RPC_INVALID_PARAMS = -32602,
    MCP_RPC_INTERNAL_ERROR = -32603,
} mcp_rpc_code_t;

typedef enum {
    MCP_MSG_INVALID = 0,
    MCP_MSG_REQUEST,
    MCP_MSG_NOTIFICATION,
    MCP_MSG_RESPONSE,
} mcp_msg_kind_t;

typedef enum {
    MCP_ID_NONE = 0,
    MCP_ID_STRING,
    MCP_ID_NUMBER,
} mcp_id_type_t;

#define MCP_PROTOCOL_MAX_MESSAGE_BYTES (4u * 1024u * 1024u)

typedef struct mcp_message mcp_message_t;

mcp_message_t *mcp_message_parse(mcp_context_t *ctx, const char *text, size_t len);
void mcp_message_destroy(mcp_context_t *ctx, mcp_message_t *msg);

mcp_msg_kind_t mcp_message_kind(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_jsonrpc(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_method(mcp_context_t *ctx, const mcp_message_t *msg);
const mcp_json_value_t *mcp_message_params(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_id_type_t mcp_message_id_type(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_id_string(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_status_t mcp_message_id_number(mcp_context_t *ctx, const mcp_message_t *msg, double *out);
const mcp_json_value_t *mcp_message_result(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_status_t mcp_message_error_code(mcp_context_t *ctx, const mcp_message_t *msg, int *out);
const char *mcp_message_error_text(mcp_context_t *ctx, const mcp_message_t *msg);

mcp_message_t *mcp_request_new_string_id(mcp_context_t *ctx, const char *id,
                                         const char *method, mcp_json_value_t *params);
mcp_message_t *mcp_request_new_number_id(mcp_context_t *ctx, double id,
                                         const char *method, mcp_json_value_t *params);
mcp_message_t *mcp_notification_new(mcp_context_t *ctx, const char *method,
                                    mcp_json_value_t *params);
mcp_message_t *mcp_response_ok_new(mcp_context_t *ctx, const mcp_message_t *req,
                                   mcp_json_value_t *result);
mcp_message_t *mcp_response_err_new(mcp_context_t *ctx, const mcp_message_t *req_or_null,
                                    int code, const char *message, mcp_json_value_t *data);

char *mcp_message_serialize(mcp_context_t *ctx, const mcp_message_t *msg);

int mcp_status_to_rpc_code(mcp_status_t status);
mcp_status_t mcp_rpc_code_to_status(int code);

#endif
