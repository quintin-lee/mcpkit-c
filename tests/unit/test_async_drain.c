#include "test_check.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/transport/ws.h"

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *tv = mcp_json_object_get(ctx, args, "text");
    const char *text = NULL;
    if (tv == NULL || mcp_json_string_value(ctx, tv, &text) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *res = mcp_json_object_new(ctx);

    mcp_json_object_set(ctx, item, "type", mcp_json_string_new(ctx, "text"));
    mcp_json_object_set(ctx, item, "text", mcp_json_string_new(ctx, text));
    mcp_json_array_append(ctx, content, item);
    mcp_json_object_set(ctx, res, "content", content);

    *result_out = res;
    return MCP_OK;
}

typedef struct {
    mcp_context_t *ctx;
    mcp_async_server_t *srv;
} server_thread_arg_t;

static int server_run_worker(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    mcp_async_server_run(a->ctx, a->srv);
    return 0;
}

static int connect_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_drain_clean(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "drain-server", "1.0");
    CHECK(srv != NULL);

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, srv,
                              mcp_tool_new(ctx, "echo", "Echo text", schema,
                                           echo_handler, NULL)) == MCP_OK);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .enable_sse = true,
        .enable_websocket = true,
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(async_srv != NULL);

    uint16_t port = mcp_async_server_get_port(async_srv);
    CHECK(port > 0);

    server_thread_arg_t targ = { .ctx = ctx, .srv = async_srv };
    thrd_t th;
    CHECK(thrd_create(&th, server_run_worker, &targ) == thrd_success);

    /* Client 1: Initialize session over HTTP POST */
    int fd1 = connect_tcp(port);
    CHECK(fd1 >= 0);
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"drain-test\",\"version\":\"1.0\"}}}";
    char req1[1024];
    int n = snprintf(req1, sizeof(req1),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n\r\n%s",
                     port, strlen(init_body), init_body);
    CHECK(send(fd1, req1, (size_t)n, 0) == n);

    char resp1[2048];
    ssize_t r1 = recv(fd1, resp1, sizeof(resp1) - 1, 0);
    CHECK(r1 > 0);
    resp1[r1] = '\0';
    CHECK(strstr(resp1, "200 OK") != NULL);

    const char *sid_header = strstr(resp1, "Mcp-Session-Id: ");
    CHECK(sid_header != NULL);
    char sid[64];
    CHECK(sscanf(sid_header + 16, "%63s", sid) == 1);

    /* Client 2: Connect SSE */
    int fd2 = connect_tcp(port);
    CHECK(fd2 >= 0);
    char sse_req[512];
    n = snprintf(sse_req, sizeof(sse_req),
                 "GET /sse HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Accept: text/event-stream\r\n"
                 "Mcp-Session-Id: %s\r\n\r\n",
                 port, sid);
    CHECK(send(fd2, sse_req, (size_t)n, 0) == n);
    char sse_resp[512];
    ssize_t sr = recv(fd2, sse_resp, sizeof(sse_resp) - 1, 0);
    CHECK(sr > 0);
    sse_resp[sr] = '\0';
    CHECK(strstr(sse_resp, "200 OK") != NULL);

    CHECK(mcp_async_server_active_connections(async_srv) >= 1);

    /* Start graceful draining with 2000ms timeout */
    mcp_status_t dst = mcp_async_server_drain(ctx, async_srv, 2000);
    CHECK(dst == MCP_OK);

    /* Verify listener was closed: new connection attempts fail */
    int fd_new = connect_tcp(port);
    CHECK(fd_new < 0);

    /* Clean up client sockets */
    close(fd1);
    close(fd2);

    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

static void test_drain_timeout_forced(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "drain-to-server", "1.0");
    CHECK(srv != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .enable_sse = true,
        .enable_websocket = true,
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(async_srv != NULL);

    uint16_t port = mcp_async_server_get_port(async_srv);
    CHECK(port > 0);

    server_thread_arg_t targ = { .ctx = ctx, .srv = async_srv };
    thrd_t th;
    CHECK(thrd_create(&th, server_run_worker, &targ) == thrd_success);

    /* Connect a stubborn client that holds connection with incomplete request */
    int fd_stubborn = connect_tcp(port);
    CHECK(fd_stubborn >= 0);
    const char *partial = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10000\r\n\r\npartial";
    CHECK(send(fd_stubborn, partial, strlen(partial), 0) == (ssize_t)strlen(partial));

    /* Brief wait for accept */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);
    CHECK(mcp_async_server_active_connections(async_srv) == 1);

    /* Drain with very short timeout (100ms) - client won't close voluntarily */
    mcp_status_t dst = mcp_async_server_drain(ctx, async_srv, 100);
    /* Upon timeout expiration, drain forcefully closes all lingering connections */
    CHECK(dst == MCP_ERR_TIMEOUT);
    CHECK(mcp_async_server_active_connections(async_srv) == 0);

    close(fd_stubborn);

    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_drain_clean();
    test_drain_timeout_forced();
    printf("test_async_drain: all tests passed\n");
    return 0;
}
