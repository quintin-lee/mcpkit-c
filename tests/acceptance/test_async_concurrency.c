#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/ws.h"

#define NUM_HTTP_THREADS 6
#define NUM_WS_THREADS 6
#define NUM_SSE_THREADS 2
#define CALLS_PER_THREAD 85 /* (6 + 6) * 85 = 1020 requests */

static atomic_int g_errors = 0;
static atomic_int g_completed_calls = 0;

typedef struct {
    const char *host;
    unsigned short port;
    int thread_id;
} thread_arg_t;

static int tcp_connect(const char *host, unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sin.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int http_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int fd = tcp_connect(ta->host, ta->port);
    if (fd < 0) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    /* 1. Initialize */
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"http-conc\",\"version\":\"1.0\"}}}";
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n"
                     "%s",
                     ta->host, ta->port, strlen(init_body), init_body);
    if (write(fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }

    char resp_buf[2048] = {0};
    ssize_t rn = read(fd, resp_buf, sizeof(resp_buf) - 1);
    if (rn <= 0 || strstr(resp_buf, "200 OK") == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }

    const char *sid_hdr = strstr(resp_buf, "Mcp-Session-Id: ");
    if (sid_hdr == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    sid_hdr += 16;
    const char *sid_end = strstr(sid_hdr, "\r\n");
    if (sid_end == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    char session_id[64] = {0};
    memcpy(session_id, sid_hdr, (size_t)(sid_end - sid_hdr));

    /* 2. Initialized notification */
    const char *notif_body = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    n = snprintf(req, sizeof(req),
                 "POST /mcp HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: keep-alive\r\n"
                 "\r\n"
                 "%s",
                 ta->host, ta->port, session_id, strlen(notif_body), notif_body);
    if (write(fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    memset(resp_buf, 0, sizeof(resp_buf));
    rn = read(fd, resp_buf, sizeof(resp_buf) - 1);
    if (rn <= 0 || (strstr(resp_buf, "200 OK") == NULL && strstr(resp_buf, "202 Accepted") == NULL)) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }

    /* 3. Loop tools/call */
    for (int i = 0; i < CALLS_PER_THREAD; i++) {
        char call_body[256];
        int blen = snprintf(call_body, sizeof(call_body),
                            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
                            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"http-%d-%d\"}}}",
                            i + 10, ta->thread_id, i);
        n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Mcp-Session-Id: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n"
                     "%s",
                     ta->host, ta->port, session_id, blen, call_body);
        if (write(fd, req, (size_t)n) != n) {
            atomic_fetch_add(&g_errors, 1);
            break;
        }

        memset(resp_buf, 0, sizeof(resp_buf));
        rn = read(fd, resp_buf, sizeof(resp_buf) - 1);
        char expected[64];
        snprintf(expected, sizeof(expected), "http-%d-%d", ta->thread_id, i);
        if (rn <= 0 || strstr(resp_buf, expected) == NULL) {
            atomic_fetch_add(&g_errors, 1);
            break;
        }
        atomic_fetch_add(&g_completed_calls, 1);
    }

    close(fd);
    return 0;
}

static int ws_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    char ws_url[128];
    snprintf(ws_url, sizeof(ws_url), "ws://%s:%u/mcp", ta->host, ta->port);
    mcp_transport_t *t = mcp_ws_client_connect(ctx, ws_url);
    if (t == NULL) {
        atomic_fetch_add(&g_errors, 1);
        mcp_context_destroy(ctx);
        return -1;
    }

    mcp_client_t *c = mcp_client_create(ctx, t);
    if (c == NULL) {
        atomic_fetch_add(&g_errors, 1);
        mcp_transport_destroy(ctx, t);
        mcp_context_destroy(ctx);
        return -1;
    }

    if (mcp_client_connect(ctx, c) != MCP_OK ||
        mcp_client_initialize(ctx, c, "ws-conc", "1.0", NULL) != MCP_OK) {
        atomic_fetch_add(&g_errors, 1);
        mcp_client_destroy(ctx, c);
        mcp_transport_destroy(ctx, t);
        mcp_context_destroy(ctx);
        return -1;
    }

    for (int i = 0; i < CALLS_PER_THREAD; i++) {
        char text_buf[64];
        snprintf(text_buf, sizeof(text_buf), "ws-%d-%d", ta->thread_id, i);

        mcp_json_value_t *args = mcp_json_object_new(ctx);
        mcp_json_value_t *val = mcp_json_string_new(ctx, text_buf);
        mcp_json_object_set(ctx, args, "text", val);

        mcp_json_value_t *result = NULL;
        mcp_status_t st = mcp_client_call_tool(ctx, c, "echo", args, &result);

        if (st != MCP_OK || result == NULL) {
            atomic_fetch_add(&g_errors, 1);
            if (result != NULL) mcp_json_destroy(ctx, result);
            break;
        }

        char *serialized = mcp_json_serialize(ctx, result);
        mcp_json_destroy(ctx, result);
        if (serialized == NULL || strstr(serialized, text_buf) == NULL) {
            atomic_fetch_add(&g_errors, 1);
            if (serialized != NULL) mcp_json_free_string(ctx, serialized);
            break;
        }
        mcp_json_free_string(ctx, serialized);
        atomic_fetch_add(&g_completed_calls, 1);
    }

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    return 0;
}

static int sse_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int init_fd = tcp_connect(ta->host, ta->port);
    if (init_fd < 0) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    /* Initialize to get a valid session */
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"sse-conc\",\"version\":\"1.0\"}}}";
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     ta->host, ta->port, strlen(init_body), init_body);
    write(init_fd, req, (size_t)n);

    char resp_buf[2048] = {0};
    ssize_t rn = read(init_fd, resp_buf, sizeof(resp_buf) - 1);
    close(init_fd);

    if (rn <= 0 || strstr(resp_buf, "200 OK") == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    const char *sid_hdr = strstr(resp_buf, "Mcp-Session-Id: ");
    if (sid_hdr == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }
    sid_hdr += 16;
    const char *sid_end = strstr(sid_hdr, "\r\n");
    if (sid_end == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }
    char session_id[64] = {0};
    memcpy(session_id, sid_hdr, (size_t)(sid_end - sid_hdr));

    /* Open SSE connection */
    int sse_fd = tcp_connect(ta->host, ta->port);
    if (sse_fd < 0) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    n = snprintf(req, sizeof(req),
                 "GET /mcp HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Accept: text/event-stream\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "\r\n",
                 ta->host, ta->port, session_id);
    if (write(sse_fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(sse_fd);
        return -1;
    }

    memset(resp_buf, 0, sizeof(resp_buf));
    rn = read(sse_fd, resp_buf, sizeof(resp_buf) - 1);
    if (rn <= 0 || strstr(resp_buf, "200 OK") == NULL ||
        strstr(resp_buf, "text/event-stream") == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(sse_fd);
        return -1;
    }

    /* Keep SSE connection open for 500ms while traffic is ongoing */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    close(sse_fd);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned short port = argc > 2 ? (unsigned short)atoi(argv[2]) : 8080;

    printf("Starting concurrency stress test against %s:%u\n", host, port);
    printf("  HTTP workers: %d, WS workers: %d, SSE workers: %d, calls/worker: %d\n",
           NUM_HTTP_THREADS, NUM_WS_THREADS, NUM_SSE_THREADS, CALLS_PER_THREAD);

    thrd_t threads[NUM_HTTP_THREADS + NUM_WS_THREADS + NUM_SSE_THREADS];
    thread_arg_t args[NUM_HTTP_THREADS + NUM_WS_THREADS + NUM_SSE_THREADS];
    int t_idx = 0;

    for (int i = 0; i < NUM_SSE_THREADS; i++) {
        args[t_idx] = (thread_arg_t){.host = host, .port = port, .thread_id = t_idx};
        thrd_create(&threads[t_idx], sse_worker, &args[t_idx]);
        t_idx++;
    }

    for (int i = 0; i < NUM_HTTP_THREADS; i++) {
        args[t_idx] = (thread_arg_t){.host = host, .port = port, .thread_id = t_idx};
        thrd_create(&threads[t_idx], http_worker, &args[t_idx]);
        t_idx++;
    }

    for (int i = 0; i < NUM_WS_THREADS; i++) {
        args[t_idx] = (thread_arg_t){.host = host, .port = port, .thread_id = t_idx};
        thrd_create(&threads[t_idx], ws_worker, &args[t_idx]);
        t_idx++;
    }

    for (int i = 0; i < t_idx; i++) {
        thrd_join(threads[i], NULL);
    }

    int errors = atomic_load(&g_errors);
    int completed = atomic_load(&g_completed_calls);
    printf("Stress test completed: %d calls succeeded, %d errors\n", completed, errors);

    if (errors > 0 || completed < (NUM_HTTP_THREADS + NUM_WS_THREADS) * CALLS_PER_THREAD) {
        fprintf(stderr, "FAIL: errors detected during concurrency test\n");
        return 1;
    }

    printf("PASS: All concurrency checks passed with 0 errors\n");
    return 0;
}
