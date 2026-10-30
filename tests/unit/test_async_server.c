#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <threads.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/transport/ws.h"
#include "mcpkit/runtime/threadpool.h"

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

static int tcp_connect_loopback(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    CHECK(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
    return fd;
}

static void test_async_server_http_sse_and_ws(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "async-test", "0.1.0");
    CHECK(server != NULL);

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, server,
                              mcp_tool_new(ctx, "echo", "Echo text", schema,
                                           echo_handler, NULL)) == MCP_OK);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0, /* Ephemeral port */
        .bind_addr = "127.0.0.1",
        .max_connections = 100,
        .keepalive_timeout_s = 60,
        .enable_websocket = true,
        .enable_sse = true,
    };

    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    CHECK(srv != NULL);

    uint16_t port = mcp_async_server_get_port(srv);
    CHECK(port > 0);

    /* Run server in background thread */
    server_thread_arg_t sarg = {.ctx = ctx, .srv = srv};
    thrd_t sthrd;
    CHECK(thrd_create(&sthrd, server_run_worker, &sarg) == thrd_success);

    /* Sleep briefly for server to enter reactor step */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    /* --- 1. Test POST initialize --- */
    int http_fd = tcp_connect_loopback(port);
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";

    char http_req[1024];
    int n = snprintf(http_req, sizeof(http_req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     port, strlen(init_body), init_body);
    CHECK(write(http_fd, http_req, (size_t)n) == n);

    char resp_buf[2048] = {0};
    ssize_t rn = read(http_fd, resp_buf, sizeof(resp_buf) - 1);
    CHECK(rn > 0);
    CHECK(strstr(resp_buf, "200 OK") != NULL);
    CHECK(strstr(resp_buf, "Mcp-Session-Id:") != NULL);

    /* Extract session id */
    const char *sid_hdr = strstr(resp_buf, "Mcp-Session-Id: ");
    CHECK(sid_hdr != NULL);
    sid_hdr += 16;
    const char *sid_end = strstr(sid_hdr, "\r\n");
    CHECK(sid_end != NULL);
    char session_id[64] = {0};
    memcpy(session_id, sid_hdr, (size_t)(sid_end - sid_hdr));
    close(http_fd);

    /* --- 2. Test GET SSE with Session-Id --- */
    int sse_fd = tcp_connect_loopback(port);
    n = snprintf(http_req, sizeof(http_req),
                 "GET /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Accept: text/event-stream\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "\r\n",
                 port, session_id);
    CHECK(write(sse_fd, http_req, (size_t)n) == n);

    memset(resp_buf, 0, sizeof(resp_buf));
    rn = read(sse_fd, resp_buf, sizeof(resp_buf) - 1);
    CHECK(rn > 0);
    CHECK(strstr(resp_buf, "200 OK") != NULL);
    CHECK(strstr(resp_buf, "text/event-stream") != NULL);
    close(sse_fd);

    /* --- 3. Test Negative: GET SSE without Session-Id --- */
    int neg_fd = tcp_connect_loopback(port);
    n = snprintf(http_req, sizeof(http_req),
                 "GET /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Accept: text/event-stream\r\n"
                 "\r\n",
                 port);
    CHECK(write(neg_fd, http_req, (size_t)n) == n);
    memset(resp_buf, 0, sizeof(resp_buf));
    rn = read(neg_fd, resp_buf, sizeof(resp_buf) - 1);
    CHECK(rn > 0);
    CHECK(strstr(resp_buf, "400 Bad Request") != NULL);
    close(neg_fd);

    /* --- 4. Test WebSocket Upgrade, initialize, and tools/call --- */
    char ws_url[128];
    snprintf(ws_url, sizeof(ws_url), "ws://127.0.0.1:%u/mcp", port);
    mcp_transport_t *ws_cli = mcp_ws_client_connect(ctx, ws_url);
    CHECK(ws_cli != NULL);
    CHECK(mcp_transport_start(ctx, ws_cli) == MCP_OK);

    /* First initialize over WebSocket */
    const char *ws_init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"ws-test\",\"version\":\"1.0\"}}}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_init, strlen(ws_init)) == MCP_OK);
    char *ws_resp = NULL;
    CHECK(mcp_transport_recv(ctx, ws_cli, &ws_resp) == MCP_OK);
    CHECK(ws_resp != NULL);
    CHECK(strstr(ws_resp, "protocolVersion") != NULL);
    mcp_json_free_string(ctx, ws_resp);

    /* Send initialized notification */
    const char *ws_inited = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_inited, strlen(ws_inited)) == MCP_OK);
    thrd_sleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000}, NULL);

    /* Send tools/call over WebSocket */
    const char *ws_call =
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello async\"}}}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_call, strlen(ws_call)) == MCP_OK);

    ws_resp = NULL;
    CHECK(mcp_transport_recv(ctx, ws_cli, &ws_resp) == MCP_OK);
    CHECK(ws_resp != NULL);
    CHECK(strstr(ws_resp, "hello async") != NULL);
    mcp_json_free_string(ctx, ws_resp);

    mcp_transport_stop(ctx, ws_cli);
    mcp_transport_destroy(ctx, ws_cli);

    /* Stop server */
    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);

    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_async_server_http_sse_and_ws();
    printf("test_async_server: OK\n");
    return 0;
}
