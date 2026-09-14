#include <assert.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"

static mcp_json_value_t *make_params(mcp_context_t *ctx) {
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    assert(p != NULL);
    mcp_json_value_t *v = mcp_json_number_new(ctx, 1);
    assert(v != NULL);
    assert(mcp_json_object_set(ctx, p, "n", v) == MCP_OK);
    return p;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    // 1. string-id request roundtrip
    mcp_message_t *req = mcp_request_new_string_id(ctx, "r1", "tools/call", make_params(ctx));
    assert(req != NULL);
    char *s = mcp_message_serialize(ctx, req);
    assert(s != NULL);
    assert(strstr(s, "\"jsonrpc\":\"2.0\"") != NULL);
    assert(strstr(s, "\"id\":\"r1\"") != NULL);
    mcp_message_t *back = mcp_message_parse(ctx, s, strlen(s));
    mcp_json_free_string(ctx, s);
    assert(back != NULL);
    assert(mcp_message_kind(ctx, back) == MCP_MSG_REQUEST);
    assert(strcmp(mcp_message_method(ctx, back), "tools/call") == 0);
    assert(mcp_message_id_type(ctx, back) == MCP_ID_STRING);
    assert(strcmp(mcp_message_id_string(ctx, back), "r1") == 0);
    assert(mcp_message_params(ctx, back) != NULL);

    // 4. ok response copies id
    mcp_json_value_t *res = mcp_json_string_new(ctx, "done");
    assert(res != NULL);
    mcp_message_t *ok = mcp_response_ok_new(ctx, back, res);
    assert(ok != NULL);
    assert(mcp_message_kind(ctx, ok) == MCP_MSG_RESPONSE);
    assert(mcp_message_id_type(ctx, ok) == MCP_ID_STRING);
    assert(strcmp(mcp_message_id_string(ctx, ok), "r1") == 0);
    const mcp_json_value_t *rv = mcp_message_result(ctx, ok);
    assert(rv != NULL);
    const char *rs = NULL;
    assert(mcp_json_string_value(ctx, rv, &rs) == MCP_OK);
    assert(strcmp(rs, "done") == 0);

    // 5. error response carries code/text
    mcp_message_t *err = mcp_response_err_new(ctx, back, -32601, "Method not found", NULL);
    assert(err != NULL);
    int code = 0;
    assert(mcp_message_error_code(ctx, err, &code) == MCP_OK);
    assert(code == -32601);
    assert(strcmp(mcp_message_error_text(ctx, err), "Method not found") == 0);

    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, back);
    mcp_message_destroy(ctx, ok);
    mcp_message_destroy(ctx, err);

    // 2. number-id request
    mcp_message_t *nreq = mcp_request_new_number_id(ctx, 7, "ping", NULL);
    assert(nreq != NULL);
    s = mcp_message_serialize(ctx, nreq);
    assert(s != NULL);
    assert(strstr(s, "\"params\"") == NULL);
    mcp_message_t *nback = mcp_message_parse(ctx, s, strlen(s));
    mcp_json_free_string(ctx, s);
    assert(nback != NULL);
    assert(mcp_message_id_type(ctx, nback) == MCP_ID_NUMBER);
    double d = 0;
    assert(mcp_message_id_number(ctx, nback, &d) == MCP_OK);
    assert(d == 7);
    assert(mcp_message_params(ctx, nback) == NULL);
    mcp_message_destroy(ctx, nreq);
    mcp_message_destroy(ctx, nback);

    // 3. notification has no id
    mcp_message_t *ntf = mcp_notification_new(ctx, "notifications/initialized", NULL);
    assert(ntf != NULL);
    s = mcp_message_serialize(ctx, ntf);
    assert(s != NULL);
    mcp_message_t *ntfback = mcp_message_parse(ctx, s, strlen(s));
    mcp_json_free_string(ctx, s);
    assert(ntfback != NULL);
    assert(mcp_message_kind(ctx, ntfback) == MCP_MSG_NOTIFICATION);
    assert(mcp_message_id_type(ctx, ntfback) == MCP_ID_NONE);
    mcp_message_destroy(ctx, ntf);
    mcp_message_destroy(ctx, ntfback);

    // 6. error response with NULL req → id null
    mcp_message_t *perr = mcp_response_err_new(ctx, NULL, -32700, "Parse error", NULL);
    assert(perr != NULL);
    assert(mcp_message_id_type(ctx, perr) == MCP_ID_NONE);
    s = mcp_message_serialize(ctx, perr);
    assert(s != NULL);
    assert(strstr(s, "\"id\":null") != NULL);
    mcp_json_free_string(ctx, s);
    mcp_message_destroy(ctx, perr);

    // 8. status<->code mapping roundtrip
    assert(mcp_status_to_rpc_code(MCP_ERR_INVALID_ARGUMENT) == -32602);
    assert(mcp_status_to_rpc_code(MCP_ERR_NOT_FOUND) == -32601);
    assert(mcp_status_to_rpc_code(MCP_ERR_PROTOCOL) == -32600);
    assert(mcp_rpc_code_to_status(-32602) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_rpc_code_to_status(-32601) == MCP_ERR_NOT_FOUND);
    assert(mcp_rpc_code_to_status(-32600) == MCP_ERR_PROTOCOL);
    assert(mcp_rpc_code_to_status(-32000) == MCP_ERR_PROTOCOL);

    mcp_context_destroy(ctx);
    return 0;
}
