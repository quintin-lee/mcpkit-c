/**
 * @file main.c
 * @brief Basic MCP HTTP Server & Tool Implementation for Verification Testing.
 *
 * Implements a standards-compliant Streamable HTTP MCP server featuring:
 *  - Tool 1: `echo` (String echo with timestamp)
 *  - Tool 2: `calculate` (Basic arithmetic: add, subtract, multiply, divide)
 *  - Tool 3: `get_system_time` (Current server UTC/local ISO-8601 time)
 *  - Resource: `test://info` (Server metadata resource)
 *
 * Supports Streamable HTTP POST (/mcp), SSE events, and WebSocket transports.
 */

#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"

static void on_signal(int sig) {
    (void)sig;
    mcp_request_shutdown();
}

/**
 * @brief Helper: Construct MCP standard content response {"content":[{"type":"text","text":"..."}]}
 */
static mcp_json_value_t *make_text_response(mcp_context_t *ctx, const char *text) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *item = mcp_json_object_new(ctx);

    if (result == NULL || content == NULL || item == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, item);
        return NULL;
    }

    mcp_json_object_set_take(ctx, item, "type", mcp_json_string_new(ctx, "text"));
    mcp_json_object_set_take(ctx, item, "text", mcp_json_string_new(ctx, text));
    mcp_json_array_append(ctx, content, item);
    mcp_json_object_set_take(ctx, result, "content", content);
    return result;
}

/**
 * @brief Tool Handler: echo
 * Params: {"message": string}
 */
static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;

    const mcp_json_value_t *val = mcp_json_object_get(ctx, args, "message");
    const char *msg = NULL;
    if (val == NULL || mcp_json_string_value(ctx, val, &msg) != MCP_OK || msg == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "[Echo] %s", msg);

    *result_out = make_text_response(ctx, buf);
    return *result_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

/**
 * @brief Tool Handler: calculate
 * Params: {"operation": string, "a": number, "b": number}
 */
static mcp_status_t calculate_handler(mcp_context_t *ctx, mcp_session_t *session,
                                     const mcp_json_value_t *args, void *user_data,
                                     mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;

    const mcp_json_value_t *op_val = mcp_json_object_get(ctx, args, "operation");
    const mcp_json_value_t *a_val = mcp_json_object_get(ctx, args, "a");
    const mcp_json_value_t *b_val = mcp_json_object_get(ctx, args, "b");

    const char *op = NULL;
    double a = 0.0, b = 0.0;

    if (op_val == NULL || mcp_json_string_value(ctx, op_val, &op) != MCP_OK || op == NULL ||
        a_val == NULL || mcp_json_number_value(ctx, a_val, &a) != MCP_OK ||
        b_val == NULL || mcp_json_number_value(ctx, b_val, &b) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    double res = 0.0;
    if (strcmp(op, "add") == 0) {
        res = a + b;
    } else if (strcmp(op, "subtract") == 0) {
        res = a - b;
    } else if (strcmp(op, "multiply") == 0) {
        res = a * b;
    } else if (strcmp(op, "divide") == 0) {
        if (b == 0.0) {
            *result_out = make_text_response(ctx, "Error: Division by zero");
            return *result_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        res = a / b;
    } else {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Calculation result: %f %s %f = %f", a, op, b, res);
    *result_out = make_text_response(ctx, buf);
    return *result_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

/**
 * @brief Tool Handler: get_system_time
 * Params: {}
 */
static mcp_status_t system_time_handler(mcp_context_t *ctx, mcp_session_t *session,
                                       const mcp_json_value_t *args, void *user_data,
                                       mcp_json_value_t **result_out) {
    (void)session;
    (void)args;
    (void)user_data;

    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);

    char iso_buf[64];
    strftime(iso_buf, sizeof(iso_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    char buf[256];
    snprintf(buf, sizeof(buf), "Current Server Time (UTC): %s", iso_buf);
    *result_out = make_text_response(ctx, buf);
    return *result_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

/**
 * @brief Resource Read Handler: test://info
 */
static mcp_status_t info_resource_read(mcp_context_t *ctx, mcp_session_t *session,
                                       const char *uri, void *user_data,
                                       mcp_json_value_t **contents_out) {
    (void)session;
    (void)user_data;

    mcp_json_value_t *content_obj = mcp_json_object_new(ctx);
    mcp_json_value_t *contents_arr = mcp_json_array_new(ctx);
    if (!content_obj || !contents_arr) {
        mcp_json_destroy(ctx, content_obj);
        mcp_json_destroy(ctx, contents_arr);
        return MCP_ERR_NOMEM;
    }

    const char *info_json = "{\"server\":\"mcpkit-c\",\"version\":\"0.2.0\",\"status\":\"ready\"}";
    mcp_json_object_set_take(ctx, content_obj, "uri", mcp_json_string_new(ctx, uri));
    mcp_json_object_set_take(ctx, content_obj, "mimeType", mcp_json_string_new(ctx, "application/json"));
    mcp_json_object_set_take(ctx, content_obj, "text", mcp_json_string_new(ctx, info_json));

    mcp_json_array_append(ctx, contents_arr, content_obj);
    *contents_out = contents_arr;
    return MCP_OK;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    unsigned short port = 8080;
    if (argc > 1) {
        port = (unsigned short)atoi(argv[1]);
        if (port == 0) port = 8080;
    }

    /* 1. Create global root context */
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    /* 2. Create MCP server instance */
    mcp_server_t *server = mcp_server_create(ctx, "mcpkit-basic-http-server", "1.0.0");
    if (server == NULL) {
        fprintf(stderr, "Failed to create server\n");
        mcp_context_destroy(ctx);
        return 1;
    }

    /* 3. Register Tool 1: echo */
    {
        mcp_json_value_t *schema = mcp_schema_object_new(ctx);
        mcp_schema_add_property(ctx, schema, "message", mcp_schema_string_new(ctx));
        mcp_schema_add_required(ctx, schema, "message");

        mcp_tool_t *t = mcp_tool_new(ctx, "echo", "Echo back input text message",
                                     schema, echo_handler, NULL);
        mcp_server_add_tool(ctx, server, t);
    }

    /* 4. Register Tool 2: calculate */
    {
        mcp_json_value_t *schema = mcp_schema_object_new(ctx);
        mcp_schema_add_property(ctx, schema, "operation", mcp_schema_string_new(ctx));
        mcp_schema_add_property(ctx, schema, "a", mcp_schema_number_new(ctx));
        mcp_schema_add_property(ctx, schema, "b", mcp_schema_number_new(ctx));
        mcp_schema_add_required(ctx, schema, "operation");
        mcp_schema_add_required(ctx, schema, "a");
        mcp_schema_add_required(ctx, schema, "b");

        mcp_tool_t *t = mcp_tool_new(ctx, "calculate", "Perform basic arithmetic: add, subtract, multiply, divide",
                                     schema, calculate_handler, NULL);
        mcp_server_add_tool(ctx, server, t);
    }

    /* 5. Register Tool 3: get_system_time */
    {
        mcp_json_value_t *schema = mcp_schema_object_new(ctx);
        mcp_tool_t *t = mcp_tool_new(ctx, "get_system_time", "Get current server UTC timestamp",
                                     schema, system_time_handler, NULL);
        mcp_server_add_tool(ctx, server, t);
    }

    /* 6. Register Resource: test://info */
    {
        mcp_resource_t *r = mcp_resource_new(ctx, "test://info", "System Status Info",
                                             "application/json", info_resource_read, NULL);
        mcp_server_add_resource(ctx, server, r);
    }

    /* 7. Create worker thread pool for parallel tool execution */
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    if (pool == NULL) {
        fprintf(stderr, "Failed to create thread pool\n");
        mcp_server_destroy(ctx, server);
        mcp_context_destroy(ctx);
        return 1;
    }

    /* 8. Configure and start Streamable HTTP Async Server */
    mcp_async_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = port,
        .enable_sse = true,
        .enable_websocket = true,
        .max_connections = 1024,
    };

    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, server, pool, &cfg);
    if (async_srv == NULL) {
        fprintf(stderr, "Failed to start server on port %u\n", port);
        mcp_executor_destroy(ctx, pool);
        mcp_server_destroy(ctx, server);
        mcp_context_destroy(ctx);
        return 1;
    }

    uint16_t actual_port = mcp_async_server_get_port(async_srv);
    printf("==================================================================\n");
    printf("🚀 MCP HTTP Tool Server is running on http://127.0.0.1:%u\n", actual_port);
    printf("------------------------------------------------------------------\n");
    printf("  Endpoint: POST http://127.0.0.1:%u/mcp\n", actual_port);
    printf("  SSE Stream: GET http://127.0.0.1:%u/mcp\n", actual_port);
    printf("  WebSocket:  ws://127.0.0.1:%u/mcp\n", actual_port);
    printf("------------------------------------------------------------------\n");
    printf("  Registered Tools:\n");
    printf("    - echo(message: string)\n");
    printf("    - calculate(operation: string, a: number, b: number)\n");
    printf("    - get_system_time()\n");
    printf("  Registered Resources:\n");
    printf("    - test://info (application/json)\n");
    printf("==================================================================\n");
    printf("Press Ctrl+C to terminate server.\n\n");
    fflush(stdout);

    /* 9. Event loop running until SIGINT/SIGTERM */
    mcp_async_server_run(ctx, async_srv);

    /* 10. Clean teardown */
    printf("\nShutting down server gracefully...\n");
    mcp_async_server_stop(ctx, async_srv);
    mcp_async_server_destroy(ctx, async_srv);
    mcp_executor_wait(ctx, pool);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);

    printf("Server stopped cleanly.\n");
    return 0;
}
