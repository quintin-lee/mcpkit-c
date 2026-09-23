#include "test_check.h"
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"

static mcp_json_value_t *make_params(mcp_context_t *ctx) {
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    CHECK(p != NULL);
    mcp_json_value_t *v = mcp_json_number_new(ctx, 1);
    CHECK(v != NULL);
    CHECK(mcp_json_object_set(ctx, p, "n", v) == MCP_OK);
    return p;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. string-id request roundtrip
    mcp_message_t *req = mcp_request_new_string_id(ctx, "r1", "tools/call", make_params(ctx));
    CHECK(req != NULL);
    char *s = mcp_message_serialize(ctx, req);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"jsonrpc\":\"2.0\"") != NULL);
    CHECK(strstr(s, "\"id\":\"r1\"") != NULL);
    mcp_message_t *back = mcp_message_parse(ctx, s, strlen(s));
    mcp_json_free_string(ctx, s);
    CHECK(back != NULL);
    CHECK(mcp_message_kind(ctx, back) == MCP_MSG_REQUEST);
    CHECK(strcmp(mcp_message_method(ctx, back), "tools/call") == 0);
    CHECK(mcp_message_id_type(ctx, back) == MCP_ID_STRING);
    CHECK(strcmp(mcp_message_id_string(ctx, back), "r1") == 0);
    CHECK(mcp_message_params(ctx, back) != NULL);

    // 4. ok response copies id
    mcp_json_value_t *res = mcp_json_string_new(ctx, "done");
    CHECK(res != NULL);
    mcp_message_t *ok = mcp_response_ok_new(ctx, back, res);
    CHECK(ok != NULL);
    CHECK(mcp_message_kind(ctx, ok) == MCP_MSG_RESPONSE);
    CHECK(mcp_message_id_type(ctx, ok) == MCP_ID_STRING);
    CHECK(strcmp(mcp_message_id_string(ctx, ok), "r1") == 0);
    const mcp_json_value_t *rv = mcp_message_result(ctx, ok);
    CHECK(rv != NULL);
    const char *rs = NULL;
    CHECK(mcp_json_string_value(ctx, rv, &rs) == MCP_OK);
    CHECK(strcmp(rs, "done") == 0);

    // 5. error response carries code/text
    mcp_message_t *err = mcp_response_err_new(ctx, back, -32601, "Method not found", NULL);
    CHECK(err != NULL);
    int code = 0;
    CHECK(mcp_message_error_code(ctx, err, &code) == MCP_OK);
    CHECK(code == -32601);
    CHECK(strcmp(mcp_message_error_text(ctx, err), "Method not found") == 0);

    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, back);
    mcp_message_destroy(ctx, ok);
    mcp_message_destroy(ctx, err);

    // 2. number-id request
    mcp_message_t *nreq = mcp_request_new_number_id(ctx, 7, "ping", NULL);
    CHECK(nreq != NULL);
    s = mcp_message_serialize(ctx, nreq);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"params\"") == NULL);
    mcp_message_t *nback = mcp_message_parse(ctx, s, strlen(s));
    mcp_json_free_string(ctx, s);
    CHECK(nback != NULL);
    CHECK(mcp_message_id_type(ctx, nback) == MCP_ID_NUMBER);
    double d = 0;
    CHECK(mcp_message_id_number(ctx, nback, &d) == MCP_OK);
    CHECK(d == 7);
    CHECK(mcp_message_params(ctx, nback) == NULL);
    mcp_message_destroy(ctx, nreq);
    mcp_message_destroy(ctx, nback);

    // 3. notification has no id
    mcp_message_t *ntf = mcp_notification_new(ctx, "notifications/initialized", NULL);
    CHECK(ntf != NULL);
    s = mcp_message_serialize(ctx, ntf);
    CHECK(s != NULL);
    mcp_message_t *ntfback = mcp_message_parse(ctx, s, strlen(s));
    mcp_json_free_string(ctx, s);
    CHECK(ntfback != NULL);
    CHECK(mcp_message_kind(ctx, ntfback) == MCP_MSG_NOTIFICATION);
    CHECK(mcp_message_id_type(ctx, ntfback) == MCP_ID_NONE);
    mcp_message_destroy(ctx, ntf);
    mcp_message_destroy(ctx, ntfback);

    // 6. error response with NULL req → id null
    mcp_message_t *perr = mcp_response_err_new(ctx, NULL, -32700, "Parse error", NULL);
    CHECK(perr != NULL);
    CHECK(mcp_message_id_type(ctx, perr) == MCP_ID_NONE);
    s = mcp_message_serialize(ctx, perr);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"id\":null") != NULL);
    mcp_json_free_string(ctx, s);
    mcp_message_destroy(ctx, perr);

    // 7. response_ok with explicit string / number id
    mcp_json_value_t *r_str = mcp_json_string_new(ctx, "sub_done");
    mcp_message_t *ok_str = mcp_response_ok_string_id_new(ctx, "sub-99", r_str);
    CHECK(ok_str != NULL);
    CHECK(mcp_message_kind(ctx, ok_str) == MCP_MSG_RESPONSE);
    CHECK(mcp_message_id_type(ctx, ok_str) == MCP_ID_STRING);
    CHECK(strcmp(mcp_message_id_string(ctx, ok_str), "sub-99") == 0);
    mcp_message_destroy(ctx, ok_str);

    mcp_json_value_t *r_num = mcp_json_string_new(ctx, "sub_num_done");
    mcp_message_t *ok_num = mcp_response_ok_number_id_new(ctx, 99.0, r_num);
    CHECK(ok_num != NULL);
    CHECK(mcp_message_kind(ctx, ok_num) == MCP_MSG_RESPONSE);
    CHECK(mcp_message_id_type(ctx, ok_num) == MCP_ID_NUMBER);
    double got_d = 0;
    CHECK(mcp_message_id_number(ctx, ok_num, &got_d) == MCP_OK && got_d == 99.0);
    mcp_message_destroy(ctx, ok_num);

    // 8. status<->code mapping roundtrip
    CHECK(mcp_status_to_rpc_code(MCP_ERR_INVALID_ARGUMENT) == -32602);
    CHECK(mcp_status_to_rpc_code(MCP_ERR_NOT_FOUND) == -32601);
    CHECK(mcp_status_to_rpc_code(MCP_ERR_PROTOCOL) == -32600);
    CHECK(mcp_rpc_code_to_status(-32602) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_rpc_code_to_status(-32601) == MCP_ERR_NOT_FOUND);
    CHECK(mcp_rpc_code_to_status(-32600) == MCP_ERR_PROTOCOL);
    CHECK(mcp_rpc_code_to_status(-32000) == MCP_ERR_PROTOCOL);

    mcp_context_destroy(ctx);
    return 0;
}
