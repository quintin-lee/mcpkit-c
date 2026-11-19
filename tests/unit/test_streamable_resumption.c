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
#include "mcpkit/runtime/executor.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/transport/ring_buffer.h"

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    uint64_t replayed_ids[16];
    size_t count;
} replay_tracker_t;

static void on_replay(uint64_t id, const char *data, size_t len, void *userdata) {
    (void)data;
    (void)len;
    replay_tracker_t *tracker = (replay_tracker_t *)userdata;
    if (tracker->count < 16) {
        tracker->replayed_ids[tracker->count++] = id;
    }
}

static void test_ring_buffer_basic(void) {
    mcp_event_ring_buffer_t *rb = mcp_event_ring_buffer_create(NULL, 4);
    ASSERT_TRUE(rb != NULL);
    ASSERT_TRUE(mcp_event_ring_buffer_count(rb) == 0);
    ASSERT_TRUE(mcp_event_ring_buffer_oldest_id(rb) == 1);
    ASSERT_TRUE(mcp_event_ring_buffer_latest_id(rb) == 0);

    uint64_t id1 = 0, id2 = 0, id3 = 0;
    ASSERT_TRUE(mcp_event_ring_buffer_push(rb, "first", 5, &id1) == MCP_OK);
    ASSERT_TRUE(id1 == 1);
    ASSERT_TRUE(mcp_event_ring_buffer_push(rb, "second", 6, &id2) == MCP_OK);
    ASSERT_TRUE(id2 == 2);
    ASSERT_TRUE(mcp_event_ring_buffer_push(rb, "third", 5, &id3) == MCP_OK);
    ASSERT_TRUE(id3 == 3);

    ASSERT_TRUE(mcp_event_ring_buffer_count(rb) == 3);
    ASSERT_TRUE(mcp_event_ring_buffer_oldest_id(rb) == 1);
    ASSERT_TRUE(mcp_event_ring_buffer_latest_id(rb) == 3);

    replay_tracker_t tracker = {0};
    mcp_status_t status = mcp_event_ring_buffer_replay(rb, 1, on_replay, &tracker);
    ASSERT_TRUE(status == MCP_OK);
    ASSERT_TRUE(tracker.count == 2);
    ASSERT_TRUE(tracker.replayed_ids[0] == 2);
    ASSERT_TRUE(tracker.replayed_ids[1] == 3);

    memset(&tracker, 0, sizeof(tracker));
    status = mcp_event_ring_buffer_replay(rb, 3, on_replay, &tracker);
    ASSERT_TRUE(status == MCP_OK);
    ASSERT_TRUE(tracker.count == 0);

    mcp_event_ring_buffer_destroy(rb);
}

static void test_ring_buffer_eviction_and_expiration(void) {
    mcp_event_ring_buffer_t *rb = mcp_event_ring_buffer_create(NULL, 3);
    ASSERT_TRUE(rb != NULL);

    uint64_t id = 0;
    mcp_event_ring_buffer_push(rb, "msg1", 4, &id);
    mcp_event_ring_buffer_push(rb, "msg2", 4, &id);
    mcp_event_ring_buffer_push(rb, "msg3", 4, &id);
    ASSERT_TRUE(mcp_event_ring_buffer_count(rb) == 3);
    ASSERT_TRUE(mcp_event_ring_buffer_oldest_id(rb) == 1);
    ASSERT_TRUE(mcp_event_ring_buffer_latest_id(rb) == 3);

    mcp_event_ring_buffer_push(rb, "msg4", 4, &id);
    ASSERT_TRUE(id == 4);
    ASSERT_TRUE(mcp_event_ring_buffer_count(rb) == 3);
    ASSERT_TRUE(mcp_event_ring_buffer_oldest_id(rb) == 2);
    ASSERT_TRUE(mcp_event_ring_buffer_latest_id(rb) == 4);

    mcp_event_ring_buffer_push(rb, "msg5", 4, &id);
    ASSERT_TRUE(id == 5);
    ASSERT_TRUE(mcp_event_ring_buffer_count(rb) == 3);
    ASSERT_TRUE(mcp_event_ring_buffer_oldest_id(rb) == 3);
    ASSERT_TRUE(mcp_event_ring_buffer_latest_id(rb) == 5);

    replay_tracker_t tracker = {0};
    mcp_status_t status = mcp_event_ring_buffer_replay(rb, 1, on_replay, &tracker);
    ASSERT_TRUE(status == MCP_ERR_EXPIRED);
    ASSERT_TRUE(tracker.count == 0);

    memset(&tracker, 0, sizeof(tracker));
    status = mcp_event_ring_buffer_replay(rb, 2, on_replay, &tracker);
    ASSERT_TRUE(status == MCP_OK);
    ASSERT_TRUE(tracker.count == 3);
    ASSERT_TRUE(tracker.replayed_ids[0] == 3);
    ASSERT_TRUE(tracker.replayed_ids[1] == 4);
    ASSERT_TRUE(tracker.replayed_ids[2] == 5);

    mcp_event_ring_buffer_destroy(rb);
}

static void on_check_frame(uint64_t id, const char *data, size_t len, void *userdata) {
    (void)id;
    (void)len;
    char **out = (char **)userdata;
    *out = strdup(data);
}

static void test_ring_buffer_framing(void) {
    mcp_event_ring_buffer_t *rb = mcp_event_ring_buffer_create(NULL, 4);
    ASSERT_TRUE(rb != NULL);

    uint64_t id = 0;
    mcp_event_ring_buffer_push(rb, "hello raw", 9, &id);
    ASSERT_TRUE(id == 1);

    char *captured = NULL;
    mcp_status_t status = mcp_event_ring_buffer_replay(rb, 0, on_check_frame, &captured);
    ASSERT_TRUE(status == MCP_OK);
    ASSERT_TRUE(captured != NULL);
    ASSERT_TRUE(strstr(captured, "id: 1\n") != NULL);
    ASSERT_TRUE(strstr(captured, "data: hello raw\n\n") != NULL);
    free(captured);

    const char *pre_framed = "event: ping\ndata: pong\n\n";
    mcp_event_ring_buffer_push(rb, pre_framed, strlen(pre_framed), &id);
    ASSERT_TRUE(id == 2);

    captured = NULL;
    status = mcp_event_ring_buffer_replay(rb, 1, on_check_frame, &captured);
    ASSERT_TRUE(status == MCP_OK);
    ASSERT_TRUE(captured != NULL);
    ASSERT_TRUE(strstr(captured, "id: 2\n") != NULL);
    ASSERT_TRUE(strstr(captured, "event: ping\ndata: pong\n\n") != NULL);
    free(captured);

    mcp_event_ring_buffer_destroy(rb);
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

static void test_server_streamable_resumption_and_410(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    ASSERT_TRUE(pool != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "resumption-test-server", "1.0");
    ASSERT_TRUE(srv != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .enable_sse = true,
        .enable_websocket = false,
        .sse_event_ring_capacity = 3, /* tiny capacity 3 to test eviction */
    };
    mcp_async_server_t *async_srv = mcp_async_server_create(ctx, srv, pool, &cfg);
    ASSERT_TRUE(async_srv != NULL);
    uint16_t port = mcp_async_server_get_port(async_srv);
    ASSERT_TRUE(port > 0);

    server_thread_arg_t targ = {.ctx = ctx, .srv = async_srv};
    thrd_t th;
    ASSERT_TRUE(thrd_create(&th, server_worker_thread, &targ) == thrd_success);

    /* 1. POST initialize to create session */
    int fd1 = tcp_connect_port(port);
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";
    char init_req[512];
    snprintf(init_req, sizeof(init_req),
             "POST /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             port, strlen(init_body), init_body);
    ASSERT_TRUE(write(fd1, init_req, strlen(init_req)) == (ssize_t)strlen(init_req));

    char init_resp[1024] = {0};
    ssize_t n = read(fd1, init_resp, sizeof(init_resp) - 1);
    ASSERT_TRUE(n > 0);
    close(fd1);

    /* Extract Mcp-Session-Id */
    const char *sid_hdr = strstr(init_resp, "Mcp-Session-Id: ");
    ASSERT_TRUE(sid_hdr != NULL);
    sid_hdr += strlen("Mcp-Session-Id: ");
    char sid[64] = {0};
    size_t sid_idx = 0;
    while (*sid_hdr != '\r' && *sid_hdr != '\n' && *sid_hdr != '\0' && sid_idx < sizeof(sid) - 1) {
        sid[sid_idx++] = *sid_hdr++;
    }
    sid[sid_idx] = '\0';
    ASSERT_TRUE(strlen(sid) > 0);

    /* 2. Push 2 events while SSE is disconnected -> must return MCP_OK */
    ASSERT_TRUE(mcp_async_server_push_sse(ctx, async_srv, sid, "event 1", 7) == MCP_OK);
    ASSERT_TRUE(mcp_async_server_push_sse(ctx, async_srv, sid, "event 2", 7) == MCP_OK);

    /* 3. Connect GET /mcp with Last-Event-ID: 1 -> must replay event 2 */
    int fd_sse = tcp_connect_port(port);
    char get_req[512];
    snprintf(get_req, sizeof(get_req),
             "GET /mcp HTTP/1.1\r\n"
             "Host: 127.0.0.1:%u\r\n"
             "Accept: text/event-stream\r\n"
             "Mcp-Session-Id: %s\r\n"
             "Last-Event-ID: 1\r\n"
             "Connection: keep-alive\r\n"
             "\r\n",
             port, sid);
    ASSERT_TRUE(write(fd_sse, get_req, strlen(get_req)) == (ssize_t)strlen(get_req));

    char sse_resp[1024] = {0};
    n = read(fd_sse, sse_resp, sizeof(sse_resp) - 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(sse_resp, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "text/event-stream") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "event 2") != NULL);
    close(fd_sse);

    /* 4. Push 3 more events -> events 3, 4, 5. Capacity is 3, so events 1 and 2 are evicted */
    ASSERT_TRUE(mcp_async_server_push_sse(ctx, async_srv, sid, "event 3", 7) == MCP_OK);
    ASSERT_TRUE(mcp_async_server_push_sse(ctx, async_srv, sid, "event 4", 7) == MCP_OK);
    ASSERT_TRUE(mcp_async_server_push_sse(ctx, async_srv, sid, "event 5", 7) == MCP_OK);

    /* 5. Connect GET /mcp with Last-Event-ID: 1 -> event 2 was evicted -> must return 410 Gone */
    int fd_410 = tcp_connect_port(port);
    ASSERT_TRUE(write(fd_410, get_req, strlen(get_req)) == (ssize_t)strlen(get_req));

    char resp_410[512] = {0};
    n = read(fd_410, resp_410, sizeof(resp_410) - 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp_410, "HTTP/1.1 410 Gone") != NULL || strstr(resp_410, "410") != NULL);
    close(fd_410);

    /* 6. Clean up */
    mcp_async_server_stop(ctx, async_srv);
    int res = 0;
    thrd_join(th, &res);

    mcp_async_server_destroy(ctx, async_srv);
    mcp_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_ring_buffer_basic();
    test_ring_buffer_eviction_and_expiration();
    test_ring_buffer_framing();
    test_server_streamable_resumption_and_410();
    printf("All test_streamable_resumption tests passed!\n");
    return 0;
}
