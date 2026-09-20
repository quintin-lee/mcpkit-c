#include <signal.h>
#include <stdio.h>

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
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *v = mcp_json_string_new(ctx, s);
    if (result == NULL || v == NULL ||
        mcp_json_object_set(ctx, result, "echo", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        if (result != NULL) {
            mcp_json_destroy(ctx, result);
        }
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

static int init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *sess,
                        const char *client, double id) {
    int rc = -1;
    mcp_message_t *req =
        mcp_request_new_number_id(ctx, id, "initialize",
                                  mcp_initialize_params_new(ctx, client, "0.1.0"));
    mcp_message_t *resp = NULL;
    mcp_message_t *notif = NULL;
    char *out = NULL;
    if (req == NULL || mcp_server_dispatch(ctx, srv, sess, req, &resp) != MCP_OK ||
        resp == NULL || (out = mcp_message_serialize(ctx, resp)) == NULL) {
        goto done;
    }
    printf("%s: %s\n", client, out);
    mcp_json_free_string(ctx, out);
    out = NULL;
    notif = mcp_initialized_notification_new(ctx);
    if (notif == NULL || mcp_server_notify(ctx, srv, sess, notif) != MCP_OK ||
        !mcp_session_is_initialized(ctx, sess)) {
        goto done;
    }
    rc = 0;
done:
    if (out != NULL) {
        mcp_json_free_string(ctx, out);
    }
    if (notif != NULL) {
        mcp_message_destroy(ctx, notif);
    }
    if (resp != NULL) {
        mcp_message_destroy(ctx, resp);
    }
    if (req != NULL) {
        mcp_message_destroy(ctx, req);
    }
    return rc;
}

static int call_echo(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *sess,
                     const char *tag, double id, const char *text) {
    int rc = -1;
    mcp_json_value_t *args = mcp_json_object_new(ctx);
    mcp_json_value_t *params = NULL;
    mcp_message_t *req = NULL;
    mcp_message_t *resp = NULL;
    char *out = NULL;
    mcp_json_value_t *v = (args == NULL) ? NULL : mcp_json_string_new(ctx, text);
    if (v == NULL || mcp_json_object_set(ctx, args, "text", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        goto done;
    }
    params = mcp_json_object_new(ctx);
    v = (params == NULL) ? NULL : mcp_json_string_new(ctx, "echo");
    if (v == NULL || mcp_json_object_set(ctx, params, "name", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        goto done;
    }
    if (mcp_json_object_set(ctx, params, "arguments", args) != MCP_OK) {
        goto done;
    }
    args = NULL;
    req = mcp_request_new_number_id(ctx, id, "tools/call", params);
    params = NULL;
    if (req == NULL || mcp_server_dispatch(ctx, srv, sess, req, &resp) != MCP_OK ||
        resp == NULL || (out = mcp_message_serialize(ctx, resp)) == NULL) {
        goto done;
    }
    printf("%s: %s\n", tag, out);
    rc = 0;
done:
    if (out != NULL) {
        mcp_json_free_string(ctx, out);
    }
    if (resp != NULL) {
        mcp_message_destroy(ctx, resp);
    }
    if (req != NULL) {
        mcp_message_destroy(ctx, req);
    }
    if (params != NULL) {
        mcp_json_destroy(ctx, params);
    }
    if (args != NULL) {
        mcp_json_destroy(ctx, args);
    }
    return rc;
}

int main(void) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "multi-session", "0.1.0");
    mcp_session_t *a = NULL;
    mcp_session_t *b = NULL;
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
    a = mcp_server_create_session(ctx, srv);
    b = mcp_server_create_session(ctx, srv);
    if (a == NULL || b == NULL) {
        fprintf(stderr, "session create failed\n");
        goto done;
    }
    if (init_session(ctx, srv, a, "client-a", 1) != 0 ||
        init_session(ctx, srv, b, "client-b", 2) != 0 ||
        call_echo(ctx, srv, a, "A", 3, "from-a") != 0 ||
        call_echo(ctx, srv, b, "B", 4, "from-b") != 0 ||
        call_echo(ctx, srv, a, "A", 5, "from-a-again") != 0) {
        fprintf(stderr, "session flow failed\n");
        goto done;
    }
    rc = 0;
done:
    if (a != NULL) {
        mcp_server_destroy_session(ctx, srv, a);
    }
    if (b != NULL) {
        mcp_server_destroy_session(ctx, srv, b);
    }
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
