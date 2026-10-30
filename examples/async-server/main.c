#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/transport/async_server.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
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

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    unsigned short port = (argc > 1) ? (unsigned short)atoi(argv[1]) : 8080;

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    mcp_server_t *server = mcp_server_create(ctx, "async-demo-server", "1.0.0");
    if (server == NULL) {
        fprintf(stderr, "Failed to create server\n");
        mcp_context_destroy(ctx);
        return 1;
    }

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    mcp_schema_add_required(ctx, schema, "text");
    mcp_server_add_tool(ctx, server,
                        mcp_tool_new(ctx, "echo", "Echo text back", schema, echo_handler, NULL));

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    if (pool == NULL) {
        fprintf(stderr, "Failed to create thread pool\n");
        mcp_server_destroy(ctx, server);
        mcp_context_destroy(ctx);
        return 1;
    }

    mcp_async_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = port,
        .enable_websocket = true,
        .enable_sse = true,
        .max_connections = 1024,
    };

    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    if (srv == NULL) {
        fprintf(stderr, "Failed to start async server on port %u\n", port);
        mcp_executor_destroy(ctx, pool);
        mcp_server_destroy(ctx, server);
        mcp_context_destroy(ctx);
        return 1;
    }

    uint16_t actual_port = mcp_async_server_get_port(srv);
    printf("Async MCP Server listening on 127.0.0.1:%u\n", actual_port);
    printf("  Streamable HTTP POST: http://127.0.0.1:%u/mcp\n", actual_port);
    printf("  SSE Stream GET:       http://127.0.0.1:%u/mcp\n", actual_port);
    printf("  WebSocket Endpoint:   ws://127.0.0.1:%u/mcp\n", actual_port);
    fflush(stdout);

    while (g_running) {
        mcp_async_server_step(ctx, srv, 100);
    }

    printf("Shutting down async server...\n");
    mcp_async_server_stop(ctx, srv);
    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
    printf("Shutdown complete.\n");
    return 0;
}
