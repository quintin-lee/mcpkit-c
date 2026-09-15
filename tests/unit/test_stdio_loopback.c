#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *text = mcp_json_object_get(ctx, args, "text");
    const char *s = NULL;
    if (text == NULL || mcp_json_string_value(ctx, text, &s) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *stv = mcp_json_string_new(ctx, s);
    mcp_json_value_t *tyv = mcp_json_string_new(ctx, "text");
    if (item == NULL || content == NULL || result == NULL || stv == NULL || tyv == NULL ||
        mcp_json_object_set(ctx, item, "type", tyv) != MCP_OK ||
        mcp_json_object_set(ctx, item, "text", stv) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK ||
        mcp_json_object_set(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, tyv);
        mcp_json_destroy(ctx, stv);
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

static const char *kInit =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"loopback\",\"version\":\"0.1.0\"}}}\n";
static const char *kNotif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
static const char *kCall =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello stdio\"}}}\n";

static void check_init_result(mcp_context_t *ctx, const char *line) {
    mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
    assert(msg != NULL && mcp_message_kind(ctx, msg) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *res = mcp_message_result(ctx, msg);
    const mcp_json_value_t *pv = mcp_json_object_get(ctx, res, "protocolVersion");
    const char *s = NULL;
    assert(pv != NULL && mcp_json_string_value(ctx, pv, &s) == MCP_OK);
    assert(strcmp(s, "2025-06-18") == 0);
    mcp_message_destroy(ctx, msg);
}

static void check_call_result(mcp_context_t *ctx, const char *line) {
    mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
    assert(msg != NULL && mcp_message_kind(ctx, msg) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *res = mcp_message_result(ctx, msg);
    const mcp_json_value_t *content = mcp_json_object_get(ctx, res, "content");
    assert(content != NULL && mcp_json_array_size(ctx, content) == 1);
    const mcp_json_value_t *item = mcp_json_array_get(ctx, content, 0);
    const mcp_json_value_t *text = mcp_json_object_get(ctx, item, "text");
    const char *s = NULL;
    assert(text != NULL && mcp_json_string_value(ctx, text, &s) == MCP_OK);
    assert(strcmp(s, "hello stdio") == 0);
    mcp_message_destroy(ctx, msg);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "loopback", "0.1.0");
    assert(srv != NULL);

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    assert(schema != NULL);
    assert(mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) ==
           MCP_OK);
    assert(mcp_schema_add_required(ctx, schema, "text") == MCP_OK);
    assert(mcp_server_add_tool(ctx, srv,
                               mcp_tool_new(ctx, "echo", "Echo", schema, echo_handler,
                                            NULL)) == MCP_OK);

    FILE *in = tmpfile();
    assert(in != NULL);
    assert(fputs(kInit, in) != EOF && fputs(kNotif, in) != EOF && fputs(kCall, in) != EOF);
    rewind(in);
    FILE *out = tmpfile();
    assert(out != NULL);

    mcp_transport_t *t = mcp_stdio_transport_create(ctx, in, out);
    assert(t != NULL);
    assert(mcp_transport_start(ctx, t) == MCP_OK);
    assert(mcp_stdio_serve(ctx, srv, t) == MCP_OK);
    assert(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);

    rewind(out);
    char line[8192];
    assert(fgets(line, (int)sizeof(line), out) != NULL);
    check_init_result(ctx, line);
    assert(fgets(line, (int)sizeof(line), out) != NULL);
    check_call_result(ctx, line);
    assert(fgets(line, (int)sizeof(line), out) == NULL);

    fclose(in);
    fclose(out);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_stdio_loopback OK\n");
    return 0;
}
