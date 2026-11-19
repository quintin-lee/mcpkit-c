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

static mcp_transport_t *connect_ws_with_rcvbuf(mcp_context_t *ctx, uint16_t port, int rcvbuf_size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    if (rcvbuf_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    }
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    CHECK(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);

    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     port, key);
    CHECK(send(fd, req, (size_t)n, 0) == n);

    char resp[1024];
    ssize_t rn = recv(fd, resp, sizeof(resp) - 1, 0);
    CHECK(rn > 0);
    CHECK(strstr(resp, "101") != NULL);

    mcp_transport_t *t = mcp_ws_transport_create(ctx, fd, false);
    CHECK(t != NULL);
    return t;
}

static void test_backpressure_pause_and_resume(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "backpressure-server", "1.0");
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
        .outbox_high_watermark = 32 * 1024, /* 32 KB */
        .outbox_low_watermark  = 8 * 1024,  /* 8 KB */
        .outbox_max_drop_bytes = 512 * 1024,
        .socket_sndbuf         = 4096,
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(async_srv != NULL);

    uint16_t port = mcp_async_server_get_port(async_srv);
    CHECK(port > 0);

    server_thread_arg_t targ = { .ctx = ctx, .srv = async_srv };
    thrd_t th;
    CHECK(thrd_create(&th, server_run_worker, &targ) == thrd_success);

    /* Connect client via WebSocket with restricted receive buffer */
    mcp_transport_t *ws_cli = connect_ws_with_rcvbuf(ctx, port, 2048);
    CHECK(ws_cli != NULL);
    CHECK(mcp_transport_start(ctx, ws_cli) == MCP_OK);

    /* Initialize session over WebSocket */
    const char *ws_init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"ws-test\",\"version\":\"1.0\"}}}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_init, strlen(ws_init)) == MCP_OK);
    char *ws_resp = NULL;
    CHECK(mcp_transport_recv(ctx, ws_cli, &ws_resp) == MCP_OK);
    CHECK(ws_resp != NULL);
    mcp_json_free_string(ctx, ws_resp);

    /* Send initialized notification */
    const char *ws_inited = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_inited, strlen(ws_inited)) == MCP_OK);

    /* Send repeated tools/call requests without receiving responses */
    char large_text[4096];
    memset(large_text, 'A', sizeof(large_text) - 1);
    large_text[sizeof(large_text) - 1] = '\0';

    for (int i = 0; i < 200; i++) {
        char payload[5000];
        int plen = snprintf(payload, sizeof(payload),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"echo\",\"arguments\":{\"text\":\"%s\"}}}",
            i + 10, large_text);
        mcp_status_t st = mcp_transport_send(ctx, ws_cli, payload, (size_t)plen);
        if (st != MCP_OK) {
            break;
        }

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1 * 1000 * 1000};
        thrd_sleep(&ts, NULL);

        if (mcp_async_server_get_backpressured_count(async_srv) > 0) {
            break;
        }
    }

    CHECK(mcp_async_server_get_backpressured_count(async_srv) > 0);
    CHECK(mcp_async_server_get_queued_write_bytes(async_srv) >= 32 * 1024);

    /* Drain responses until queued bytes drop below low watermark (8 KB) */
    while (mcp_async_server_get_queued_write_bytes(async_srv) > 8 * 1024) {
        char *line = NULL;
        mcp_status_t rst = mcp_transport_recv(ctx, ws_cli, &line);
        if (rst != MCP_OK) break;
        mcp_json_free_string(ctx, line);
    }

    /* Wait briefly for reactor write loop to update watermark */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    CHECK(mcp_async_server_get_backpressured_count(async_srv) == 0);

    mcp_transport_stop(ctx, ws_cli);
    mcp_transport_destroy(ctx, ws_cli);

    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

static void test_backpressure_stall_eviction(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "stall-server", "1.0");
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
        .outbox_high_watermark = 8 * 1024,
        .outbox_low_watermark  = 2 * 1024,
        .outbox_max_drop_bytes = 16 * 1024, /* 16 KB max drop threshold */
        .stall_timeout_ms      = 100,       /* 100 ms stall timeout */
        .socket_sndbuf         = 4096,
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(async_srv != NULL);

    uint16_t port = mcp_async_server_get_port(async_srv);
    CHECK(port > 0);

    server_thread_arg_t targ = { .ctx = ctx, .srv = async_srv };
    thrd_t th;
    CHECK(thrd_create(&th, server_run_worker, &targ) == thrd_success);

    mcp_transport_t *ws_cli = connect_ws_with_rcvbuf(ctx, port, 2048);
    CHECK(ws_cli != NULL);
    CHECK(mcp_transport_start(ctx, ws_cli) == MCP_OK);

    /* Initialize session over WebSocket */
    const char *ws_init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"ws-test\",\"version\":\"1.0\"}}}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_init, strlen(ws_init)) == MCP_OK);
    char *ws_resp = NULL;
    CHECK(mcp_transport_recv(ctx, ws_cli, &ws_resp) == MCP_OK);
    mcp_json_free_string(ctx, ws_resp);

    /* Send initialized notification */
    const char *ws_inited = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    CHECK(mcp_transport_send(ctx, ws_cli, ws_inited, strlen(ws_inited)) == MCP_OK);

    /* Pump enough data to exceed 16 KB */
    for (int i = 0; i < 100; i++) {
        char payload[512];
        int plen = snprintf(payload, sizeof(payload),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"echo\",\"arguments\":{\"text\":\"Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.\"}}}",
            i + 10);
        mcp_status_t st = mcp_transport_send(ctx, ws_cli, payload, (size_t)plen);
        if (st != MCP_OK) break;
    }

    /* Wait for stall timeout (100 ms) to trigger eviction */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    /* Verify ws_cli gets closed by server stall eviction */
    char *evict_resp = NULL;
    mcp_status_t rst = mcp_transport_recv(ctx, ws_cli, &evict_resp);
    while (rst == MCP_OK) {
        mcp_json_free_string(ctx, evict_resp);
        evict_resp = NULL;
        rst = mcp_transport_recv(ctx, ws_cli, &evict_resp);
    }
    CHECK(rst != MCP_OK);

    mcp_transport_stop(ctx, ws_cli);
    mcp_transport_destroy(ctx, ws_cli);

    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

static int connect_tcp(uint16_t port, int rcvbuf) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    if (rcvbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    CHECK(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
    return fd;
}

static void test_http_session_backpressure(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "http-backpressure-server", "1.0");
    CHECK(srv != NULL);

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, srv,
                              mcp_tool_new(ctx, "echo", "Echo text", schema,
                                           echo_handler, NULL)) == MCP_OK);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .enable_sse = true,
        .enable_websocket = false,
        .outbox_high_watermark = 16 * 1024, /* 16 KB */
        .outbox_low_watermark  = 4 * 1024,  /* 4 KB */
        .outbox_max_drop_bytes = 256 * 1024,
        .socket_sndbuf         = 4096,
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(async_srv != NULL);

    uint16_t port = mcp_async_server_get_port(async_srv);
    CHECK(port > 0);

    server_thread_arg_t targ = { .ctx = ctx, .srv = async_srv };
    thrd_t th;
    CHECK(thrd_create(&th, server_run_worker, &targ) == thrd_success);

    /* Step 1: POST initialize request to establish session */
    int fd_post1 = connect_tcp(port, 0);
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"http-test\",\"version\":\"1.0\"}}}";
    char init_http[1024];
    int n = snprintf(init_http, sizeof(init_http),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     port, strlen(init_body), init_body);
    CHECK(send(fd_post1, init_http, (size_t)n, 0) == n);

    char resp1[2048];
    ssize_t r1 = recv(fd_post1, resp1, sizeof(resp1) - 1, 0);
    CHECK(r1 > 0);
    resp1[r1] = '\0';
    CHECK(strstr(resp1, "200 OK") != NULL);
    close(fd_post1);

    const char *sid_header = strstr(resp1, "Mcp-Session-Id: ");
    CHECK(sid_header != NULL);
    char sid[64];
    CHECK(sscanf(sid_header + 16, "%63s", sid) == 1);

    /* Send initialized notification */
    int fd_post_init = connect_tcp(port, 0);
    const char *inited_body = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    n = snprintf(init_http, sizeof(init_http),
                 "POST /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "Connection: close\r\n\r\n%s",
                 port, strlen(inited_body), sid, inited_body);
    CHECK(send(fd_post_init, init_http, (size_t)n, 0) == n);
    char resp_inited[512];
    CHECK(recv(fd_post_init, resp_inited, sizeof(resp_inited), 0) > 0);
    close(fd_post_init);

    /* Step 2: Connect SSE stream on socket B with small rcvbuf */
    int fd_sse = connect_tcp(port, 2048);
    char sse_req[512];
    n = snprintf(sse_req, sizeof(sse_req),
                 "GET /sse HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Accept: text/event-stream\r\n"
                 "Mcp-Session-Id: %s\r\n\r\n",
                 port, sid);
    CHECK(send(fd_sse, sse_req, (size_t)n, 0) == n);

    char sse_resp[512];
    ssize_t sr = recv(fd_sse, sse_resp, sizeof(sse_resp) - 1, 0);
    CHECK(sr > 0);
    sse_resp[sr] = '\0';
    CHECK(strstr(sse_resp, "200 OK") != NULL);
    CHECK(strstr(sse_resp, "text/event-stream") != NULL);

    /* Step 3: Pump SSE data to congested session (> 16 KB) without reading */
    char chunk[4096];
    memset(chunk, 'X', sizeof(chunk) - 1);
    chunk[sizeof(chunk) - 1] = '\0';
    char event_buf[4200];
    int elen = snprintf(event_buf, sizeof(event_buf), "event: message\ndata: %s\n\n", chunk);

    for (int i = 0; i < 10; i++) {
        CHECK(mcp_async_server_push_sse(ctx, async_srv, sid, event_buf, (size_t)elen) == MCP_OK);
    }

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    /* Step 4: Send POST /mcp while SSE outbox is congested -> expect 503 */
    int fd_post2 = connect_tcp(port, 0);
    const char *call_body =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"echo\",\"arguments\":{\"text\":\"hello\"}}}";
    char post_http[1024];
    n = snprintf(post_http, sizeof(post_http),
                 "POST /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "Connection: close\r\n\r\n%s",
                 port, strlen(call_body), sid, call_body);
    CHECK(send(fd_post2, post_http, (size_t)n, 0) == n);

    char resp2[1024];
    ssize_t r2 = recv(fd_post2, resp2, sizeof(resp2) - 1, 0);
    CHECK(r2 > 0);
    resp2[r2] = '\0';
    CHECK(strstr(resp2, "503 Service Unavailable") != NULL);
    CHECK(strstr(resp2, "Retry-After: 1") != NULL);
    close(fd_post2);

    /* Step 5: Drain SSE client socket until queued bytes drop */
    int flags = fcntl(fd_sse, F_GETFL, 0);
    fcntl(fd_sse, F_SETFL, flags | O_NONBLOCK);
    char drain_buf[8192];
    for (int retry = 0; retry < 50; retry++) {
        while (recv(fd_sse, drain_buf, sizeof(drain_buf), 0) > 0) {}
        if (mcp_async_server_get_queued_write_bytes(async_srv) <= 4 * 1024) {
            break;
        }
        struct timespec dts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        thrd_sleep(&dts, NULL);
    }

    thrd_sleep(&ts, NULL);

    /* Step 6: Send POST /mcp again after recovery -> expect 200 OK */
    int fd_post3 = connect_tcp(port, 0);
    CHECK(send(fd_post3, post_http, (size_t)n, 0) == n);

    char resp3[2048];
    ssize_t r3 = recv(fd_post3, resp3, sizeof(resp3) - 1, 0);
    CHECK(r3 > 0);
    resp3[r3] = '\0';
    CHECK(strstr(resp3, "200 OK") != NULL);
    close(fd_post3);

    close(fd_sse);

    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_backpressure_pause_and_resume();
    test_backpressure_stall_eviction();
    test_http_session_backpressure();
    printf("test_async_backpressure: all tests passed\n");
    return 0;
}
