#ifndef MCPKIT_CLIENT_CLIENT_H
#define MCPKIT_CLIENT_CLIENT_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

typedef struct mcp_client mcp_client_t;

mcp_client_t *mcp_client_create(mcp_context_t *ctx, mcp_transport_t *transport);
void mcp_client_destroy(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_connect(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_disconnect(mcp_context_t *ctx, mcp_client_t *client);
// Full handshake: sends initialize, validates response id, stores negotiated
// version, sends notifications/initialized. server_info_out (optional) receives
// a cloned serverInfo value owned by the caller, or NULL when absent.
mcp_status_t mcp_client_initialize(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *client_name, const char *client_version,
                                   mcp_json_value_t **server_info_out);
// Negotiated protocol version after initialize, or NULL before. Borrowed.
const char *mcp_client_protocol_version(mcp_context_t *ctx, const mcp_client_t *client);
// Generic call: params ownership transfers to the request on success (container
// owns on OK). result_out (optional) receives a cloned result owned by caller.
// Error responses map to mcp_status_t via mcp_rpc_code_to_status.
mcp_status_t mcp_client_request(mcp_context_t *ctx, mcp_client_t *client,
                                const char *method, mcp_json_value_t *params,
                                mcp_json_value_t **result_out);
mcp_status_t mcp_client_ping(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_list_tools(mcp_context_t *ctx, mcp_client_t *client,
                                   mcp_json_value_t **tools_out);
mcp_status_t mcp_client_call_tool(mcp_context_t *ctx, mcp_client_t *client,
                                  const char *name, mcp_json_value_t *args,
                                  mcp_json_value_t **result_out);
mcp_status_t mcp_client_read_resource(mcp_context_t *ctx, mcp_client_t *client,
                                      const char *uri, mcp_json_value_t **result_out);
mcp_status_t mcp_client_get_prompt(mcp_context_t *ctx, mcp_client_t *client,
                                    const char *name, mcp_json_value_t *args,
                                    mcp_json_value_t **result_out);
// completion/complete: ref must be a string like "prompt/argName".
// Returns a cloned completions array.
mcp_status_t mcp_client_complete(mcp_context_t *ctx, mcp_client_t *client,
                                 const char *ref, mcp_json_value_t *args,
                                 mcp_json_value_t **result_out);

#endif
