#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "mcpkit/mcpkit.h"

// Reference host wiring: terminate gracefully so the in-flight request
// drains and the session is destroyed. The library itself installs no
// handlers; mcp_request_shutdown() is async-signal-safe.
static void on_term(int sig) {
    (void)sig;
    mcp_request_shutdown();
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
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *tyv = mcp_json_string_new(ctx, "text");
    mcp_json_value_t *stv = mcp_json_string_new(ctx, s);
    if (item == NULL || content == NULL || result == NULL || tyv == NULL || stv == NULL) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, tyv);
        mcp_json_destroy(ctx, stv);
        return MCP_ERR_NOMEM;
    }
    // Per-step attach: set_take destroys the value only when the set
    // failed, so an already-attached child is never freed twice.
    if (mcp_json_object_set_take(ctx, item, "type", tyv) != MCP_OK ||
        mcp_json_object_set_take(ctx, item, "text", stv) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK ||
        mcp_json_object_set_take(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

int main(int argc, char **argv) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }
    unsigned port = (unsigned)strtoul(argv[1], NULL, 10);

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "socket-server", "0.1.0");
    if (srv == NULL) {
        goto done;
    }
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    if (schema == NULL ||
        mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) != MCP_OK ||
        mcp_schema_add_required(ctx, schema, "text") != MCP_OK ||
        mcp_server_add_tool(ctx, srv,
                            mcp_tool_new(ctx, "echo", "Echo text back", schema,
                                         echo_handler, NULL)) != MCP_OK) {
        goto done;
    }
    mcp_transport_t *t = mcp_socket_transport_create(ctx, NULL, (uint16_t)port, true);
    if (t == NULL) {
        fprintf(stderr, "socket bind on port %u failed\n", port);
        goto done;
    }
    if (mcp_transport_start(ctx, t) != MCP_OK) {
        fprintf(stderr, "socket accept failed\n");
        goto done;
    }
    // Serve one connection, same loop shape as the stdio server.
    mcp_status_t st = mcp_stdio_serve(ctx, srv, t);
    (void)st;

    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
done:
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return 0;
}
