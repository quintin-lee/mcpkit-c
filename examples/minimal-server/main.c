#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t set_new_string(mcp_context_t *ctx, mcp_json_value_t *obj,
                                   const char *key, const char *val) {
    mcp_json_value_t *v = mcp_json_string_new(ctx, val);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_json_object_set(ctx, obj, key, v);
    if (st != MCP_OK) {
        mcp_json_destroy(ctx, v);
    }
    return st;
}

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
    if (item == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (set_new_string(ctx, item, "type", "text") != MCP_OK ||
        set_new_string(ctx, item, "text", s) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    if (content == NULL) {
        mcp_json_destroy(ctx, item);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_array_append(ctx, content, item) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        mcp_json_destroy(ctx, content);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

static const char *kCallText =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello mcpkit\"}}}";

int main(void) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    int rc = 1;
    mcp_message_t *init_req = NULL;
    mcp_message_t *init_resp = NULL;
    mcp_message_t *notif = NULL;
    mcp_message_t *call = NULL;
    mcp_message_t *resp = NULL;
    mcp_session_t *sess = NULL;
    char *out = NULL;

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "minimal-server", "0.1.0");
    if (srv == NULL) {
        fprintf(stderr, "server create failed\n");
        goto done;
    }

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    if (schema == NULL ||
        mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) != MCP_OK ||
        mcp_schema_add_required(ctx, schema, "text") != MCP_OK ||
        mcp_server_add_tool(ctx, srv,
                            mcp_tool_new(ctx, "echo", "Echo text back", schema, echo_handler,
                                         NULL)) != MCP_OK) {
        fprintf(stderr, "tool register failed\n");
        goto done;
    }
    if (mcp_server_add_resource(
            ctx, srv,
            mcp_resource_new(ctx, "file:///hello.txt", "hello", "text/plain", NULL,
                             NULL)) != MCP_OK) {
        fprintf(stderr, "resource register failed\n");
        goto done;
    }

    sess = mcp_server_create_session(ctx, srv);
    if (sess == NULL) {
        fprintf(stderr, "session create failed\n");
        goto done;
    }

    init_req = mcp_request_new_number_id(ctx, 1, "initialize",
                                        mcp_initialize_params_new(ctx, "minimal-client",
                                                                  "0.1.0"));
    if (init_req == NULL ||
        mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) != MCP_OK ||
        init_resp == NULL) {
        fprintf(stderr, "initialize failed\n");
        goto done;
    }
    mcp_message_destroy(ctx, init_req);
    init_req = NULL;
    mcp_message_destroy(ctx, init_resp);
    init_resp = NULL;

    notif = mcp_initialized_notification_new(ctx);
    if (notif == NULL || mcp_server_notify(ctx, srv, sess, notif) != MCP_OK ||
        !mcp_session_is_initialized(ctx, sess)) {
        fprintf(stderr, "initialized notify failed\n");
        goto done;
    }
    mcp_message_destroy(ctx, notif);
    notif = NULL;

    call = mcp_message_parse(ctx, kCallText, strlen(kCallText));
    if (call == NULL || mcp_server_dispatch(ctx, srv, sess, call, &resp) != MCP_OK ||
        resp == NULL) {
        fprintf(stderr, "tools/call failed\n");
        goto done;
    }
    out = mcp_message_serialize(ctx, resp);
    if (out == NULL) {
        fprintf(stderr, "serialize failed\n");
        goto done;
    }
    printf("%s\n", out);
    rc = 0;

done:
    if (out != NULL) {
        mcp_json_free_string(ctx, out);
    }
    if (resp != NULL) {
        mcp_message_destroy(ctx, resp);
    }
    if (call != NULL) {
        mcp_message_destroy(ctx, call);
    }
    if (notif != NULL) {
        mcp_message_destroy(ctx, notif);
    }
    if (init_resp != NULL) {
        mcp_message_destroy(ctx, init_resp);
    }
    if (init_req != NULL) {
        mcp_message_destroy(ctx, init_req);
    }
    if (sess != NULL) {
        mcp_server_destroy_session(ctx, srv, sess);
    }
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
