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
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *tyv = mcp_json_string_new(ctx, "text");
    mcp_json_value_t *stv = mcp_json_string_new(ctx, s);
    if (item == NULL || content == NULL || result == NULL || tyv == NULL || stv == NULL ||
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

int main(void) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "threadpool-server", "0.1.0");
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
    mcp_executor_t *ex = mcp_threadpool_create(ctx, 4);
    if (ex == NULL) {
        fprintf(stderr, "threadpool create failed\n");
        goto done;
    }
    mcp_timer_t *timer = mcp_timer_create(ctx);
    if (timer == NULL) {
        fprintf(stderr, "timer create failed\n");
        goto done_executor;
    }
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    if (t == NULL) {
        fprintf(stderr, "transport create failed\n");
        goto done_timer;
    }
    if (mcp_transport_start(ctx, t) != MCP_OK) {
        fprintf(stderr, "transport start failed\n");
        goto done_transport;
    }
    mcp_status_t st = mcp_loop_run(ctx, srv, t, ex, timer);
    if (st != MCP_OK && st != MCP_ERR_NOT_FOUND) {
        fprintf(stderr, "loop failed: %s\n", mcp_status_string(st));
        goto done_transport;
    }
    rc = 0;

done_transport:
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
done_timer:
    mcp_timer_destroy(ctx, timer);
done_executor:
    mcp_executor_destroy(ctx, ex);
done:
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
