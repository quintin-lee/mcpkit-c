#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

#include "mcpkit/core/arena.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/runtime/executor.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"
#include "mcpkit/transport/async_server.h"

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static mcp_status_t echo_handler(mcp_context_t *c, mcp_session_t *s,
                                 const mcp_json_value_t *a, void *u,
                                 mcp_json_value_t **o) {
    (void)s;
    (void)u;
    *o = mcp_json_object_new(c);
    if (*o == NULL) return MCP_ERR_NOMEM;
    const mcp_json_value_t *msg = a != NULL ? mcp_json_object_get(c, a, "msg") : NULL;
    const char *text = NULL;
    if (msg != NULL && mcp_json_string_value(c, msg, &text) == MCP_OK && text != NULL) {
        mcp_json_object_set_take(c, *o, "echo", mcp_json_string_new(c, text));
    }
    return MCP_OK;
}

static void test_arena_reset_session_isolation(void) {
    mcp_context_t *srv_ctx = mcp_context_create(NULL);
    ASSERT_TRUE(srv_ctx != NULL);
    mcp_server_t *srv = mcp_server_create(srv_ctx, "arena-lifecycle-srv", "1.0");
    ASSERT_TRUE(srv != NULL);

    mcp_json_value_t *schema = mcp_json_object_new(srv_ctx);
    mcp_tool_t *tool = mcp_tool_new(srv_ctx, "echo", "Echo tool", schema, echo_handler, NULL);
    ASSERT_TRUE(mcp_server_add_tool(srv_ctx, srv, tool) == MCP_OK);

    mcp_session_t *sess = mcp_server_create_session(srv_ctx, srv);
    ASSERT_TRUE(sess != NULL);

    mcp_arena_t *arena = mcp_arena_create(NULL, 4096);
    ASSERT_TRUE(arena != NULL);
    mcp_allocator_t alloc = mcp_arena_as_allocator(arena);
    mcp_context_config_t req_cfg = {.allocator = &alloc};

    /* 1. Initialize session using transient arena ctx */
    {
        mcp_context_t *req_ctx = mcp_context_create(&req_cfg);
        ASSERT_TRUE(req_ctx != NULL);

        const char *init_req =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
            "\"clientInfo\":{\"name\":\"persistent-agent\",\"version\":\"3.2.1\"}}}";
        mcp_message_t *msg = mcp_message_parse(req_ctx, init_req, strlen(init_req));
        ASSERT_TRUE(msg != NULL);
        mcp_message_t *resp = NULL;
        ASSERT_TRUE(mcp_server_dispatch(req_ctx, srv, sess, msg, &resp) == MCP_OK);
        ASSERT_TRUE(resp != NULL);
        mcp_message_destroy(req_ctx, msg);
        mcp_message_destroy(req_ctx, resp);

        const char *inited_notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        msg = mcp_message_parse(req_ctx, inited_notif, strlen(inited_notif));
        ASSERT_TRUE(msg != NULL);
        ASSERT_TRUE(mcp_server_notify(req_ctx, srv, sess, msg) == MCP_OK);
        mcp_message_destroy(req_ctx, msg);

        /* Subscribe to resource */
        const char *sub_req =
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\",\"params\":{"
            "\"uri\":\"mcp://persistent/resource\"}}";
        msg = mcp_message_parse(req_ctx, sub_req, strlen(sub_req));
        ASSERT_TRUE(msg != NULL);
        ASSERT_TRUE(mcp_server_dispatch(req_ctx, srv, sess, msg, &resp) == MCP_OK);
        ASSERT_TRUE(resp != NULL);
        mcp_message_destroy(req_ctx, msg);
        mcp_message_destroy(req_ctx, resp);

        mcp_context_destroy(req_ctx);
        mcp_arena_reset(arena);
    }

    /* Verify persistent session properties survived arena reset */
    const char *cname = mcp_session_get_client_name(sess);
    ASSERT_TRUE(cname != NULL);
    ASSERT_TRUE(strcmp(cname, "persistent-agent") == 0);

    const char *cver = mcp_session_get_client_version(sess);
    ASSERT_TRUE(cver != NULL);
    ASSERT_TRUE(strcmp(cver, "3.2.1") == 0);

    ASSERT_TRUE(mcp_session_is_subscribed_to_resource(sess, "mcp://persistent/resource"));

    /* 2. Run 1,000 rapid requests through arena with resets */
    for (int i = 0; i < 1000; i++) {
        mcp_context_t *req_ctx = mcp_context_create(&req_cfg);
        ASSERT_TRUE(req_ctx != NULL);

        char req_body[256];
        snprintf(req_body, sizeof(req_body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/list\",\"params\":{}}",
                 100 + i);
        mcp_message_t *msg = mcp_message_parse(req_ctx, req_body, strlen(req_body));
        ASSERT_TRUE(msg != NULL);
        mcp_message_t *resp = NULL;
        ASSERT_TRUE(mcp_server_dispatch(req_ctx, srv, sess, msg, &resp) == MCP_OK);
        ASSERT_TRUE(resp != NULL);

        char *serialized = mcp_message_serialize(req_ctx, resp);
        ASSERT_TRUE(serialized != NULL);
        ASSERT_TRUE(strstr(serialized, "echo") != NULL);

        mcp_message_destroy(req_ctx, msg);
        mcp_message_destroy(req_ctx, resp);
        mcp_context_destroy(req_ctx);
        mcp_arena_reset(arena);
    }

    /* Verify persistent session properties STILL intact after 1,000 arena resets */
    cname = mcp_session_get_client_name(sess);
    ASSERT_TRUE(cname != NULL);
    ASSERT_TRUE(strcmp(cname, "persistent-agent") == 0);

    cver = mcp_session_get_client_version(sess);
    ASSERT_TRUE(cver != NULL);
    ASSERT_TRUE(strcmp(cver, "3.2.1") == 0);

    ASSERT_TRUE(mcp_session_is_subscribed_to_resource(sess, "mcp://persistent/resource"));

    mcp_arena_destroy(arena);
    mcp_server_destroy(srv_ctx, srv);
    mcp_context_destroy(srv_ctx);
}

typedef struct {
    mcp_context_t *ctx;
    mcp_async_server_t *srv;
} server_thread_arg_t;

static int server_worker_thread(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    mcp_async_server_run(a->ctx, a->srv);
    return 0;
}

static int tcp_connect_port(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    ASSERT_TRUE(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
    return fd;
}

static void test_async_server_arena_concurrency(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    ASSERT_TRUE(pool != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "async-arena-srv", "1.0");
    ASSERT_TRUE(srv != NULL);

    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_tool_t *tool = mcp_tool_new(ctx, "echo", "Echo tool", schema, echo_handler, NULL);
    ASSERT_TRUE(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .enable_sse = true,
        .enable_websocket = false,
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    ASSERT_TRUE(async_srv != NULL);
    uint16_t port = mcp_async_server_get_port(async_srv);
    ASSERT_TRUE(port > 0);

    server_thread_arg_t targ = {.ctx = ctx, .srv = async_srv};
    thrd_t th;
    ASSERT_TRUE(thrd_create(&th, server_worker_thread, &targ) == thrd_success);

    int fd = tcp_connect_port(port);

    /* 1. Initialize */
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"async-arena-client\",\"version\":\"1.0.0\"}}}";
    char req_buf[1024];
    snprintf(req_buf, sizeof(req_buf),
             "POST /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             port, strlen(init_body), init_body);
    ASSERT_TRUE(write(fd, req_buf, strlen(req_buf)) == (ssize_t)strlen(req_buf));

    char resp_buf[2048] = {0};
    ssize_t n = read(fd, resp_buf, sizeof(resp_buf) - 1);
    ASSERT_TRUE(n > 0);

    const char *sid_hdr = strstr(resp_buf, "Mcp-Session-Id: ");
    ASSERT_TRUE(sid_hdr != NULL);
    sid_hdr += strlen("Mcp-Session-Id: ");
    char sid[64] = {0};
    size_t sidx = 0;
    while (*sid_hdr != '\r' && *sid_hdr != '\n' && sidx < sizeof(sid) - 1) {
        sid[sidx++] = *sid_hdr++;
    }
    sid[sidx] = '\0';

    /* 2. Initialized notification */
    const char *inited_body = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    snprintf(req_buf, sizeof(req_buf),
             "POST /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Mcp-Session-Id: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             port, sid, strlen(inited_body), inited_body);
    ASSERT_TRUE(write(fd, req_buf, strlen(req_buf)) == (ssize_t)strlen(req_buf));
    memset(resp_buf, 0, sizeof(resp_buf));
    n = read(fd, resp_buf, sizeof(resp_buf) - 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp_buf, "HTTP/1.1 202 Accepted") != NULL);

    /* 3. Send 200 rapid HTTP requests over the connection */
    for (int i = 0; i < 200; i++) {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{"
                 "\"name\":\"echo\",\"arguments\":{\"msg\":\"hello-%d\"}}}",
                 100 + i, i);
        snprintf(req_buf, sizeof(req_buf),
                 "POST /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: keep-alive\r\n"
                 "\r\n"
                 "%s",
                 port, sid, strlen(body), body);
        ASSERT_TRUE(write(fd, req_buf, strlen(req_buf)) == (ssize_t)strlen(req_buf));

        memset(resp_buf, 0, sizeof(resp_buf));
        n = read(fd, resp_buf, sizeof(resp_buf) - 1);
        ASSERT_TRUE(n > 0);
        ASSERT_TRUE(strstr(resp_buf, "HTTP/1.1 200 OK") != NULL);
        char expected_echo[64];
        snprintf(expected_echo, sizeof(expected_echo), "hello-%d", i);
        ASSERT_TRUE(strstr(resp_buf, expected_echo) != NULL);
    }

    close(fd);

    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_arena_reset_session_isolation();
    test_async_server_arena_concurrency();
    printf("All test_async_arena_lifecycle tests passed!\n");
    return 0;
}
