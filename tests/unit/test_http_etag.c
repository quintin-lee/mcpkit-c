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

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/json.h"
#include "mcpkit/runtime/executor.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/async_server.h"

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

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

static mcp_status_t dummy_handler(mcp_context_t *c, mcp_session_t *s,
                                  const mcp_json_value_t *a, void *u,
                                  mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    *o = mcp_json_object_new(c);
    return *o ? MCP_OK : MCP_ERR_NOMEM;
}

static void test_revision_hashing(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "rev-test-srv", "1.0");
    ASSERT_TRUE(srv != NULL);

    uint64_t rev0 = mcp_server_get_tools_revision(srv, MCP_TOOL_VIS_MODEL);

    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_tool_t *t1 = mcp_tool_new(ctx, "tool1", "Description 1", schema, dummy_handler, NULL);
    ASSERT_TRUE(mcp_server_add_tool(ctx, srv, t1) == MCP_OK);

    uint64_t rev1 = mcp_server_get_tools_revision(srv, MCP_TOOL_VIS_MODEL);
    ASSERT_TRUE(rev1 != rev0);

    mcp_json_value_t *schema2 = mcp_json_object_new(ctx);
    mcp_tool_t *t2 = mcp_tool_new(ctx, "tool2", "Description 2", schema2, dummy_handler, NULL);
    ASSERT_TRUE(mcp_server_add_tool(ctx, srv, t2) == MCP_OK);

    uint64_t rev2 = mcp_server_get_tools_revision(srv, MCP_TOOL_VIS_MODEL);
    ASSERT_TRUE(rev2 != rev1);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void test_http_etag_304_and_pagination(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    ASSERT_TRUE(pool != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "etag-srv", "1.0");
    ASSERT_TRUE(srv != NULL);

    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_tool_t *t1 = mcp_tool_new(ctx, "calc", "Calculator tool", schema, dummy_handler, NULL);
    ASSERT_TRUE(mcp_server_add_tool(ctx, srv, t1) == MCP_OK);

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

    /* 1. POST initialize */
    int fd = tcp_connect_port(port);
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"etag-test\",\"version\":\"1.0\"}}}";
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

    /* 1.5 POST notifications/initialized */
    const char *inited_body =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
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

    /* 2. POST tools/list without ETag -> returns 200 OK + ETag */
    const char *list_body =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}";
    snprintf(req_buf, sizeof(req_buf),
             "POST /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Mcp-Session-Id: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             port, sid, strlen(list_body), list_body);
    ASSERT_TRUE(write(fd, req_buf, strlen(req_buf)) == (ssize_t)strlen(req_buf));

    memset(resp_buf, 0, sizeof(resp_buf));
    n = read(fd, resp_buf, sizeof(resp_buf) - 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp_buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(resp_buf, "calc") != NULL);

    const char *etag_hdr = strstr(resp_buf, "ETag: ");
    ASSERT_TRUE(etag_hdr != NULL);
    etag_hdr += strlen("ETag: ");
    char etag[64] = {0};
    size_t eidx = 0;
    while (*etag_hdr != '\r' && *etag_hdr != '\n' && eidx < sizeof(etag) - 1) {
        etag[eidx++] = *etag_hdr++;
    }
    etag[eidx] = '\0';
    ASSERT_TRUE(strlen(etag) > 0);

    /* 3. POST tools/list WITH matching If-None-Match -> returns 304 Not Modified */
    const char *list_body2 =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":{}}";
    snprintf(req_buf, sizeof(req_buf),
             "POST /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Mcp-Session-Id: %s\r\n"
             "If-None-Match: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             port, sid, etag, strlen(list_body2), list_body2);
    ASSERT_TRUE(write(fd, req_buf, strlen(req_buf)) == (ssize_t)strlen(req_buf));

    memset(resp_buf, 0, sizeof(resp_buf));
    n = read(fd, resp_buf, sizeof(resp_buf) - 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp_buf, "HTTP/1.1 304 Not Modified") != NULL);
    ASSERT_TRUE(strstr(resp_buf, "Content-Length: 0") != NULL);

    /* 4. POST tools/list WITH If-None-Match AND cursor -> bypasses 304, returns 200 OK */
    const char *list_page_body =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\",\"params\":{\"cursor\":\"page2\"}}";
    snprintf(req_buf, sizeof(req_buf),
             "POST /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Mcp-Session-Id: %s\r\n"
             "If-None-Match: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             port, sid, etag, strlen(list_page_body), list_page_body);
    ASSERT_TRUE(write(fd, req_buf, strlen(req_buf)) == (ssize_t)strlen(req_buf));

    memset(resp_buf, 0, sizeof(resp_buf));
    n = read(fd, resp_buf, sizeof(resp_buf) - 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp_buf, "HTTP/1.1 200 OK") != NULL);

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
    test_revision_hashing();
    test_http_etag_304_and_pagination();
    printf("All test_http_etag tests passed!\n");
    return 0;
}
