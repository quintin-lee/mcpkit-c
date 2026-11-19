#include "bench_common.h"
#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/streamable_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>

#define MAX_CONCURRENCY 256
#define MAX_SAMPLES_PER_THREAD 100000

typedef struct {
    int fd;
} sock_io_t;

static mcp_status_t sock_read_cb(mcp_context_t *ctx, void *user, char *buf, size_t cap, size_t *n_out) {
    (void)ctx;
    sock_io_t *s = (sock_io_t *)user;
    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MCP_ERR_TIMEOUT;
        return MCP_ERR_IO;
    }
    *n_out = (size_t)n;
    return MCP_OK;
}

static mcp_status_t sock_write_cb(mcp_context_t *ctx, void *user, const char *data, size_t len) {
    (void)ctx;
    sock_io_t *s = (sock_io_t *)user;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(s->fd, data + sent, len - sent, 0);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return MCP_ERR_IO;
        }
        sent += (size_t)n;
    }
    return MCP_OK;
}

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                  const mcp_json_value_t *args, void *user_data,
                                  mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *text = mcp_json_object_get(ctx, args, "text");
    const char *s = "bench-pong";
    if (text != NULL) {
        (void)mcp_json_string_value(ctx, text, &s);
    }
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, item, "type", mcp_json_string_new(ctx, "text"));
    mcp_json_object_set_take(ctx, item, "text", mcp_json_string_new(ctx, s != NULL ? s : ""));
    mcp_json_array_append(ctx, content, item);
    mcp_json_object_set_take(ctx, result, "content", content);
    *result_out = result;
    return MCP_OK;
}

typedef struct {
    int server_fd;
    uint16_t port;
    volatile bool stop;
    mcp_context_t *ctx;
    mcp_server_t *srv;
} server_info_t;

typedef struct {
    int client_fd;
    mcp_context_t *ctx;
    mcp_server_t *srv;
} conn_worker_arg_t;

static void *conn_worker_fn(void *arg) {
    conn_worker_arg_t *c = (conn_worker_arg_t *)arg;
    sock_io_t io_state = { .fd = c->client_fd };
    mcp_http_io_t io = {
        .user = &io_state,
        .read = sock_read_cb,
        .write = sock_write_cb,
    };
    (void)mcp_http_serve(c->ctx, c->srv, &io);
    close(c->client_fd);
    free(c);
    return NULL;
}

static void *server_listener_fn(void *arg) {
    server_info_t *info = (server_info_t *)arg;
    while (!info->stop) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(info->server_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (info->stop) break;
            continue;
        }
        int flag = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        conn_worker_arg_t *c = (conn_worker_arg_t *)malloc(sizeof(conn_worker_arg_t));
        if (c == NULL) {
            close(cfd);
            continue;
        }
        c->client_fd = cfd;
        c->ctx = info->ctx;
        c->srv = info->srv;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, conn_worker_fn, c) != 0) {
            close(cfd);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

typedef struct {
    uint16_t port;
    volatile bool *stop;
    size_t target_iterations;
    uint64_t *latencies;
    size_t count;
} client_worker_arg_t;

static ssize_t read_http_response(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end != NULL) {
            char *cl_str = strstr(buf, "Content-Length: ");
            if (cl_str != NULL) {
                size_t content_len = (size_t)strtoul(cl_str + 16, NULL, 10);
                size_t body_received = total - (size_t)(hdr_end + 4 - buf);
                if (body_received >= content_len) {
                    break;
                }
            } else {
                break;
            }
        }
    }
    return (ssize_t)total;
}

static void *client_worker_fn(void *arg) {
    client_worker_arg_t *cw = (client_worker_arg_t *)arg;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return NULL;

    int flag = 1;
    setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(cw->port);
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

    if (connect(sfd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        close(sfd);
        return NULL;
    }

    char rx[4096];
    /* 1. Send initialize */
    const char *kInit =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 135\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"b\",\"version\":\"1\"}}}";
    if (send(sfd, kInit, strlen(kInit), 0) <= 0) {
        close(sfd);
        return NULL;
    }
    ssize_t rx_len = read_http_response(sfd, rx, sizeof(rx));
    if (rx_len <= 0) {
        close(sfd);
        return NULL;
    }

    char sid[64] = "sess-1";
    char *sid_hdr = strstr(rx, "Mcp-Session-Id: ");
    if (sid_hdr != NULL) {
        char *end = strstr(sid_hdr + 16, "\r\n");
        if (end != NULL) {
            size_t slen = (size_t)(end - (sid_hdr + 16));
            if (slen < sizeof(sid)) {
                memcpy(sid, sid_hdr + 16, slen);
                sid[slen] = '\0';
            }
        }
    }

    /* 2. Send notifications/initialized */
    char notif_req[256];
    int notif_len = snprintf(notif_req, sizeof(notif_req),
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nMcp-Session-Id: %s\r\nContent-Length: 53\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
        sid);
    (void)send(sfd, notif_req, (size_t)notif_len, 0);
    (void)read_http_response(sfd, rx, sizeof(rx));

    /* 3. Benchmark loop: pipelined tools/call */
    char call_req[512];
    int call_len = snprintf(call_req, sizeof(call_req),
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nMcp-Session-Id: %s\r\nContent-Length: 95\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"ping\"}}}",
        sid);

    while (!*(cw->stop) && (cw->target_iterations == 0 || cw->count < cw->target_iterations)) {
        if (cw->count >= MAX_SAMPLES_PER_THREAD) break;
        uint64_t t0 = bench_now_ns();
        if (send(sfd, call_req, (size_t)call_len, 0) <= 0) break;
        ssize_t n = read_http_response(sfd, rx, sizeof(rx));
        if (n <= 0) break;
        uint64_t t1 = bench_now_ns();
        cw->latencies[cw->count++] = (t1 - t0);
    }

    close(sfd);
    return NULL;
}

int main(int argc, char **argv) {
    size_t concurrency = 10;
    double duration_sec = 2.0;
    size_t target_total_iters = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            concurrency = 2;
            duration_sec = 0.5;
            target_total_iters = 500;
        } else if (strncmp(argv[i], "--concurrency=", 14) == 0) {
            concurrency = (size_t)strtoul(argv[i] + 14, NULL, 10);
            if (concurrency == 0) concurrency = 1;
            if (concurrency > MAX_CONCURRENCY) concurrency = MAX_CONCURRENCY;
        } else if (strncmp(argv[i], "--duration=", 11) == 0) {
            duration_sec = strtod(argv[i] + 11, NULL);
            if (duration_sec <= 0.0) duration_sec = 1.0;
        }
    }

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) return 1;

    mcp_server_t *srv = mcp_server_create(ctx, "bench-http-srv", "1.0.0");
    if (srv == NULL) {
        mcp_context_destroy(ctx);
        return 1;
    }
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "Echo", schema, echo_handler, NULL));

    /* Setup server socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = 0; /* ephemeral */
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 512) != 0) {
        close(listen_fd);
        return 1;
    }

    socklen_t slen = sizeof(saddr);
    getsockname(listen_fd, (struct sockaddr *)&saddr, &slen);
    uint16_t port = ntohs(saddr.sin_port);

    server_info_t sinfo = {
        .server_fd = listen_fd,
        .port = port,
        .stop = false,
        .ctx = ctx,
        .srv = srv,
    };

    pthread_t listener_tid;
    pthread_create(&listener_tid, NULL, server_listener_fn, &sinfo);

    /* Setup client workers */
    client_worker_arg_t *cargs = (client_worker_arg_t *)calloc(concurrency, sizeof(client_worker_arg_t));
    pthread_t *ctids = (pthread_t *)calloc(concurrency, sizeof(pthread_t));
    volatile bool stop_clients = false;

    size_t iters_per_client = (target_total_iters > 0) ? (target_total_iters / concurrency) : 0;

    for (size_t i = 0; i < concurrency; i++) {
        cargs[i].port = port;
        cargs[i].stop = &stop_clients;
        cargs[i].target_iterations = iters_per_client;
        cargs[i].latencies = (uint64_t *)malloc(MAX_SAMPLES_PER_THREAD * sizeof(uint64_t));
        cargs[i].count = 0;
    }

    uint64_t bench_start = bench_now_ns();
    for (size_t i = 0; i < concurrency; i++) {
        pthread_create(&ctids[i], NULL, client_worker_fn, &cargs[i]);
    }

    if (target_total_iters == 0) {
        struct timespec sleep_ts;
        sleep_ts.tv_sec = (time_t)duration_sec;
        sleep_ts.tv_nsec = (long)((duration_sec - (double)sleep_ts.tv_sec) * 1e9);
        nanosleep(&sleep_ts, NULL);
        stop_clients = true;
    }

    for (size_t i = 0; i < concurrency; i++) {
        pthread_join(ctids[i], NULL);
    }
    uint64_t bench_end = bench_now_ns();
    double actual_duration = (double)(bench_end - bench_start) / 1000000000.0;

    /* Stop server */
    sinfo.stop = true;
    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    pthread_join(listener_tid, NULL);

    /* Consolidate latencies */
    size_t total_samples = 0;
    for (size_t i = 0; i < concurrency; i++) {
        total_samples += cargs[i].count;
    }

    uint64_t *all_latencies = (uint64_t *)malloc((total_samples > 0 ? total_samples : 1) * sizeof(uint64_t));
    size_t offset = 0;
    for (size_t i = 0; i < concurrency; i++) {
        if (cargs[i].count > 0 && cargs[i].latencies != NULL) {
            memcpy(all_latencies + offset, cargs[i].latencies, cargs[i].count * sizeof(uint64_t));
            offset += cargs[i].count;
        }
        free(cargs[i].latencies);
    }

    size_t rss_kb = bench_get_rss_kb(0);

    bench_stats_t stats;
    bench_calc_stats("streamable_http_throughput", all_latencies, total_samples, actual_duration, rss_kb, &stats);
    bench_print_stats(&stats);

    free(all_latencies);
    free(cargs);
    free(ctids);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
