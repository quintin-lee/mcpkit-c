/**
 * @file async_server.c
 * @brief High-concurrency unified async server for Streamable HTTP/SSE and WebSocket.
 */

#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "mcpkit/transport/async_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/shutdown.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/runtime/reactor.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/http.h"
#include "mcpkit/transport/ws.h"
#include "../core/sha1.h"

typedef struct async_conn async_conn_t;

typedef enum {
    CONN_HTTP_INIT,
    CONN_SSE,
    CONN_WS
} conn_type_t;

struct async_conn {
    int fd;
    atomic_uint ref_count;
    conn_type_t type;

    char *read_buf;
    size_t read_len;
    size_t read_cap;

    char *write_buf;
    size_t write_len;
    size_t write_cap;
    size_t write_off;

    /* WebSocket Continuation frame reassembly buffer */
    char *ws_frag_buf;
    size_t ws_frag_len;
    size_t ws_frag_cap;
    mcp_ws_opcode_t ws_frag_opcode;

    char session_id[64];
    mcp_session_t *session;
    mcp_async_server_t *server;
    atomic_bool closing;
    atomic_bool detached;
    bool http_in_flight;

    async_conn_t *prev;
    async_conn_t *next;
};

typedef struct session_entry {
    char id[64];
    mcp_session_t *session;
    async_conn_t *sse_conn;
    time_t last_active;
    atomic_uint ref_count;
    atomic_bool dead;
    mtx_t session_lock;
    struct session_entry *next;
} session_entry_t;

typedef struct response_item {
    async_conn_t *conn;
    char *data;
    size_t len;
    bool close_after;
    struct response_item *next;
} response_item_t;

struct mcp_async_server {
    mcp_context_t *ctx;
    mcp_server_t *server;
    mcp_executor_t *pool;
    mcp_async_server_config_t cfg;

    int listen_fd;
    uint16_t bound_port;
    mcp_reactor_t *reactor;

    atomic_bool running;
    atomic_bool stop_requested;

    mtx_t resp_mtx;
    response_item_t *resp_head;
    response_item_t *resp_tail;

    mtx_t sessions_mtx;
    session_entry_t *sessions;
    time_t last_reap_time;

    mtx_t conns_mtx;
    async_conn_t *conns;
    size_t conns_count;
};

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static void *srv_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void srv_free(mcp_context_t *ctx, void *p) {
    if (p == NULL) return;
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

static char *srv_strdup(mcp_context_t *ctx, const char *str) {
    if (str == NULL) return NULL;
    size_t len = strlen(str);
    char *res = srv_malloc(ctx, len + 1);
    if (res != NULL) {
        memcpy(res, str, len + 1);
    }
    return res;
}

static void conn_ref(async_conn_t *conn) {
    atomic_fetch_add(&conn->ref_count, 1);
}

static void conn_unref(async_conn_t *conn) {
    if (atomic_fetch_sub(&conn->ref_count, 1) == 1) {
        mcp_context_t *ctx = conn->server != NULL ? conn->server->ctx : NULL;
        if (conn->fd >= 0) {
            close(conn->fd);
            conn->fd = -1;
        }
        if (conn->type == CONN_WS && conn->session != NULL && conn->server != NULL) {
            mcp_server_destroy_session(ctx, conn->server->server, conn->session);
            conn->session = NULL;
        }
        if (conn->read_buf != NULL) {
            srv_free(ctx, conn->read_buf);
            conn->read_buf = NULL;
        }
        if (conn->write_buf != NULL) {
            srv_free(ctx, conn->write_buf);
            conn->write_buf = NULL;
        }
        if (conn->ws_frag_buf != NULL) {
            srv_free(ctx, conn->ws_frag_buf);
            conn->ws_frag_buf = NULL;
        }
        srv_free(ctx, conn);
    }
}

static void conn_detach_and_close(mcp_async_server_t *s, async_conn_t *conn) {
    if (conn == NULL) return;
    if (atomic_exchange(&conn->detached, true)) {
        return;
    }
    atomic_store(&conn->closing, true);
    if (conn->fd >= 0) {
        mcp_reactor_remove(s->ctx, s->reactor, conn->fd);
        close(conn->fd);
        conn->fd = -1;
    }
    if (conn->type == CONN_SSE && conn->session_id[0] != '\0') {
        mtx_lock(&s->sessions_mtx);
        for (session_entry_t *cur = s->sessions; cur != NULL; cur = cur->next) {
            if (cur->sse_conn == conn) {
                cur->sse_conn = NULL;
                break;
            }
        }
        mtx_unlock(&s->sessions_mtx);
    }
    mtx_lock(&s->conns_mtx);
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        s->conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    if (s->conns_count > 0) {
        s->conns_count--;
    }
    mtx_unlock(&s->conns_mtx);
    conn_unref(conn);
}

static void enqueue_response(mcp_async_server_t *s, async_conn_t *conn, char *data,
                             size_t len, bool close_after) {
    response_item_t *item = srv_malloc(s->ctx, sizeof(*item));
    if (item == NULL) {
        srv_free(s->ctx, data);
        return;
    }
    conn_ref(conn);
    item->conn = conn;
    item->data = data;
    item->len = len;
    item->close_after = close_after;
    item->next = NULL;

    mtx_lock(&s->resp_mtx);
    if (s->resp_tail != NULL) {
        s->resp_tail->next = item;
    } else {
        s->resp_head = item;
    }
    s->resp_tail = item;
    mtx_unlock(&s->resp_mtx);

    mcp_reactor_wakeup(s->ctx, s->reactor);
}

static session_entry_t *find_session(mcp_async_server_t *s, const char *sid) {
    if (sid == NULL) return NULL;
    mtx_lock(&s->sessions_mtx);
    for (session_entry_t *cur = s->sessions; cur != NULL; cur = cur->next) {
        if (strcmp(cur->id, sid) == 0) {
            mtx_unlock(&s->sessions_mtx);
            return cur;
        }
    }
    mtx_unlock(&s->sessions_mtx);
    return NULL;
}

static session_entry_t *create_session_entry(mcp_async_server_t *s) {
    session_entry_t *entry = srv_malloc(s->ctx, sizeof(*entry));
    if (entry == NULL) return NULL;
    memset(entry, 0, sizeof(*entry));

    entry->session = mcp_server_create_session(s->ctx, s->server);
    if (entry->session == NULL) {
        srv_free(s->ctx, entry);
        return NULL;
    }

    /* Generate a unique session id */
    static atomic_uint id_counter = 1000;
    unsigned val = atomic_fetch_add(&id_counter, 1);
    snprintf(entry->id, sizeof(entry->id), "sess-%u-%ld", val, (long)time(NULL));
    entry->last_active = time(NULL);
    mtx_init(&entry->session_lock, mtx_plain);

    mtx_lock(&s->sessions_mtx);
    entry->next = s->sessions;
    s->sessions = entry;
    mtx_unlock(&s->sessions_mtx);

    return entry;
}

typedef struct {
    mcp_async_server_t *server;
    async_conn_t *conn;
    mcp_session_t *session;
    session_entry_t *entry;
    char *json_body;
    bool is_ws;
    bool is_init;
    char session_id[64];
} worker_task_t;

static void worker_dispatch_fn(mcp_context_t *task_ctx, void *arg) {
    (void)task_ctx;
    worker_task_t *task = (worker_task_t *)arg;
    mcp_async_server_t *s = task->server;
    mcp_context_t *ctx = s->ctx;

    if (task->entry != NULL) {
        mtx_lock(&task->entry->session_lock);
    }

    size_t body_len_in = task->json_body != NULL ? strlen(task->json_body) : 0;
    mcp_message_t *msg = mcp_message_parse(ctx, task->json_body, body_len_in);
    mcp_message_t *resp = NULL;
    bool is_notification = false;
    if (msg != NULL) {
        if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
            is_notification = true;
            mcp_server_notify(ctx, s->server, task->session, msg);
        } else {
            mcp_server_dispatch(ctx, s->server, task->session, msg, &resp);
        }
        mcp_message_destroy(ctx, msg);
    }

    if (task->entry != NULL) {
        task->entry->last_active = time(NULL);
        mtx_unlock(&task->entry->session_lock);
    }

    if (task->is_ws) {
        if (!is_notification) {
            char *resp_text = NULL;
            if (resp != NULL) {
                resp_text = mcp_message_serialize(ctx, resp);
                mcp_message_destroy(ctx, resp);
            }
            if (resp_text == NULL) {
                resp_text = srv_strdup(ctx, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
            }
            if (resp_text != NULL) {
                /* Wrap in WS text frame */
                mcp_ws_frame_t frame = {
                    .fin = true,
                    .opcode = MCP_WS_OP_TEXT,
                    .masked = false,
                    .payload_len = strlen(resp_text),
                    .payload = resp_text,
                };
                uint8_t *ws_bytes = NULL;
                size_t ws_len = 0;
                if (mcp_ws_encode_frame(ctx, &frame, &ws_bytes, &ws_len) == MCP_OK) {
                    enqueue_response(s, task->conn, (char *)ws_bytes, ws_len, false);
                }
                mcp_json_free_string(ctx, resp_text);
            }
        }
    } else {
        if (is_notification) {
            /* HTTP 202 Accepted according to Streamable HTTP spec */
            static const char *k_accepted =
                "HTTP/1.1 202 Accepted\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n"
                "\r\n";
            char *acc_dup = srv_strdup(ctx, k_accepted);
            if (acc_dup != NULL) {
                enqueue_response(s, task->conn, acc_dup, strlen(k_accepted), false);
            }
        } else {
            char *resp_text = NULL;
            if (resp != NULL) {
                resp_text = mcp_message_serialize(ctx, resp);
                mcp_message_destroy(ctx, resp);
            }
            if (resp_text == NULL) {
                resp_text = srv_strdup(ctx, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
            }
            if (resp_text != NULL) {
                /* Format HTTP 200 response with Mcp-Session-Id header */
                size_t body_len = strlen(resp_text);
                char hdr_buf[512];
                int hlen = 0;
                if (task->session_id[0] != '\0') {
                    hlen = snprintf(hdr_buf, sizeof(hdr_buf),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/json\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Mcp-Session-Id: %s\r\n"
                                    "Connection: keep-alive\r\n"
                                    "\r\n",
                                    body_len, task->session_id);
                } else {
                    hlen = snprintf(hdr_buf, sizeof(hdr_buf),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/json\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Connection: keep-alive\r\n"
                                    "\r\n",
                                    body_len);
                }
                size_t full_len = (size_t)hlen + body_len;
                char *full_resp = srv_malloc(ctx, full_len + 1);
                if (full_resp != NULL) {
                    memcpy(full_resp, hdr_buf, (size_t)hlen);
                    memcpy(full_resp + hlen, resp_text, body_len);
                    full_resp[full_len] = '\0';
                    enqueue_response(s, task->conn, full_resp, full_len, false);
                }
                mcp_json_free_string(ctx, resp_text);
            }
        }
    }

    conn_unref(task->conn);
    srv_free(ctx, task->json_body);
    srv_free(ctx, task);
}

static void handle_client_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user);

static void close_conn(mcp_async_server_t *s, async_conn_t *conn) {
    if (conn == NULL || conn->fd < 0) return;
    atomic_store(&conn->closing, true);
    mcp_reactor_remove(s->ctx, s->reactor, conn->fd);
    close(conn->fd);
    conn->fd = -1;
}

static void send_http_error(mcp_async_server_t *s, async_conn_t *conn, int code, const char *msg) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     code, msg, strlen(msg), msg);
    char *dup = srv_malloc(s->ctx, (size_t)n + 1);
    if (dup != NULL) {
        memcpy(dup, buf, (size_t)n + 1);
        enqueue_response(s, conn, dup, (size_t)n, true);
    }
}

static void process_http_request(mcp_async_server_t *s, async_conn_t *conn) {
    mcp_context_t *ctx = s->ctx;
    mcp_http_request_t *req = mcp_http_parse_request(ctx, conn->read_buf, conn->read_len);
    if (req == NULL) {
        send_http_error(s, conn, 400, "Bad Request");
        return;
    }

    const char *upgrade = mcp_http_header(ctx, req, "Upgrade");
    if (upgrade != NULL && strcasecmp(upgrade, "websocket") == 0) {
        if (!s->cfg.enable_websocket) {
            mcp_http_request_destroy(ctx, req);
            send_http_error(s, conn, 403, "WebSocket Disabled");
            return;
        }
        const char *key = mcp_http_header(ctx, req, "Sec-WebSocket-Key");
        if (key == NULL) {
            mcp_http_request_destroy(ctx, req);
            send_http_error(s, conn, 400, "Missing Sec-WebSocket-Key");
            return;
        }
        char accept_key[32];
        if (mcp_ws_compute_accept_key(key, accept_key) != MCP_OK) {
            mcp_http_request_destroy(ctx, req);
            send_http_error(s, conn, 500, "Handshake Error");
            return;
        }

        char resp[512];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 101 Switching Protocols\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: %s\r\n"
                         "\r\n",
                         accept_key);
        char *dup = srv_malloc(ctx, (size_t)n + 1);
        if (dup != NULL) {
            memcpy(dup, resp, (size_t)n + 1);
            enqueue_response(s, conn, dup, (size_t)n, false);
        }

        conn->type = CONN_WS;
        conn->session = mcp_server_create_session(ctx, s->server);
        conn->read_len = 0;
        mcp_http_request_destroy(ctx, req);
        return;
    }

    mcp_http_method_t method = mcp_http_request_method(ctx, req);

    if (method == MCP_HTTP_GET) {
        const char *accept = mcp_http_header(ctx, req, "Accept");
        if (accept != NULL && strstr(accept, "text/event-stream") != NULL) {
            if (!s->cfg.enable_sse) {
                mcp_http_request_destroy(ctx, req);
                send_http_error(s, conn, 403, "SSE Disabled");
                return;
            }
            const char *sid = mcp_http_header(ctx, req, "Mcp-Session-Id");
            if (sid == NULL) {
                mcp_http_request_destroy(ctx, req);
                send_http_error(s, conn, 400, "Bad Request: Missing Mcp-Session-Id");
                return;
            }
            session_entry_t *entry = find_session(s, sid);
            if (entry == NULL) {
                mcp_http_request_destroy(ctx, req);
                send_http_error(s, conn, 404, "Session Not Found");
                return;
            }
            conn->type = CONN_SSE;
            snprintf(conn->session_id, sizeof(conn->session_id), "%s", sid);
            conn->session = entry->session;
            entry->sse_conn = conn;

            const char *sse_ok =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "\r\n";
            char *dup = srv_malloc(ctx, strlen(sse_ok) + 1);
            if (dup != NULL) {
                memcpy(dup, sse_ok, strlen(sse_ok) + 1);
                enqueue_response(s, conn, dup, strlen(sse_ok), false);
            }
            conn->read_len = 0;
            mcp_http_request_destroy(ctx, req);
            return;
        }
        mcp_http_request_destroy(ctx, req);
        send_http_error(s, conn, 405, "Method Not Allowed");
        return;
    }

    if (method == MCP_HTTP_POST) {
        size_t body_len = 0;
        const char *body = mcp_http_request_body(ctx, req, &body_len);

        const char *sid = mcp_http_header(ctx, req, "Mcp-Session-Id");
        session_entry_t *entry = NULL;
        bool is_init = false;

        if (sid == NULL) {
            /* Must be initialize request */
            if (body != NULL && strstr(body, "\"initialize\"") != NULL) {
                entry = create_session_entry(s);
                if (entry == NULL) {
                    mcp_http_request_destroy(ctx, req);
                    send_http_error(s, conn, 500, "Cannot Create Session");
                    return;
                }
                is_init = true;
            } else {
                mcp_http_request_destroy(ctx, req);
                send_http_error(s, conn, 400, "Bad Request: Missing Mcp-Session-Id");
                return;
            }
        } else {
            entry = find_session(s, sid);
            if (entry == NULL) {
                mcp_http_request_destroy(ctx, req);
                send_http_error(s, conn, 404, "Session Not Found");
                return;
            }
        }

        worker_task_t *task = srv_malloc(ctx, sizeof(*task));
        if (task == NULL) {
            mcp_http_request_destroy(ctx, req);
            send_http_error(s, conn, 500, "OOM");
            return;
        }
        conn_ref(conn);
        task->server = s;
        task->conn = conn;
        task->session = entry->session;
        task->entry = entry;
        task->is_ws = false;
        task->is_init = is_init;
        snprintf(task->session_id, sizeof(task->session_id), "%s", entry->id);
        task->json_body = srv_malloc(ctx, body_len + 1);
        if (task->json_body != NULL && body != NULL) {
            memcpy(task->json_body, body, body_len);
            task->json_body[body_len] = '\0';
        }

        mcp_executor_submit(ctx, s->pool, worker_dispatch_fn, task);
        conn->read_len = 0;
        mcp_http_request_destroy(ctx, req);
        return;
    }

    mcp_http_request_destroy(ctx, req);
    send_http_error(s, conn, 405, "Method Not Allowed");
}

static void handle_client_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    async_conn_t *conn = (async_conn_t *)user;
    mcp_async_server_t *s = conn->server;
    (void)ctx;

    if (events & (MCP_EVENT_ERROR)) {
        close_conn(s, conn);
        return;
    }

    if (events & MCP_EVENT_WRITE) {
        if (conn->write_buf != NULL && conn->write_off < conn->write_len) {
            ssize_t n = send(fd, conn->write_buf + conn->write_off,
                             conn->write_len - conn->write_off, MSG_NOSIGNAL);
            if (n > 0) {
                conn->write_off += (size_t)n;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close_conn(s, conn);
                return;
            }
        }
        if (conn->write_off >= conn->write_len) {
            srv_free(ctx, conn->write_buf);
            conn->write_buf = NULL;
            conn->write_len = 0;
            conn->write_cap = 0;
            conn->write_off = 0;
            if (atomic_load(&conn->closing)) {
                close_conn(s, conn);
                return;
            }
            mcp_reactor_modify(s->ctx, s->reactor, fd, MCP_EVENT_READ);
        }
    }

    if (events & MCP_EVENT_READ) {
        if (conn->read_len + 4096 > conn->read_cap) {
            size_t ncap = conn->read_cap == 0 ? 8192 : conn->read_cap * 2;
            if (ncap > (4u << 20)) {
                close_conn(s, conn);
                return;
            }
            char *nread = srv_malloc(ctx, ncap);
            if (nread == NULL) {
                close_conn(s, conn);
                return;
            }
            if (conn->read_buf != NULL && conn->read_len > 0) {
                memcpy(nread, conn->read_buf, conn->read_len);
            }
            srv_free(ctx, conn->read_buf);
            conn->read_buf = nread;
            conn->read_cap = ncap;
        }

        ssize_t n = recv(fd, conn->read_buf + conn->read_len,
                         conn->read_cap - conn->read_len, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close_conn(s, conn);
            }
            return;
        }
        if (n == 0) {
            close_conn(s, conn);
            return;
        }
        conn->read_len += (size_t)n;
        conn->read_buf[conn->read_len] = '\0';

        if (conn->type == CONN_HTTP_INIT) {
            if (strstr(conn->read_buf, "\r\n\r\n") != NULL) {
                process_http_request(s, conn);
            }
        } else if (conn->type == CONN_WS) {
            for (;;) {
                mcp_ws_frame_t frame = {0};
                size_t consumed = 0;
                mcp_status_t st = mcp_ws_parse_frame(ctx, (const uint8_t *)conn->read_buf,
                                                     conn->read_len, &frame, &consumed);
                if (st != MCP_OK) {
                    close_conn(s, conn);
                    return;
                }
                if (consumed == 0) {
                    break;
                }
                memmove(conn->read_buf, conn->read_buf + consumed, conn->read_len - consumed);
                conn->read_len -= consumed;

                if (frame.opcode == MCP_WS_OP_TEXT || frame.opcode == MCP_WS_OP_BINARY) {
                    worker_task_t *task = srv_malloc(ctx, sizeof(*task));
                    if (task != NULL) {
                        conn_ref(conn);
                        task->server = s;
                        task->conn = conn;
                        task->session = conn->session;
                        task->entry = NULL;
                        task->is_ws = true;
                        task->is_init = false;
                        task->session_id[0] = '\0';
                        task->json_body = frame.payload;
                        frame.payload = NULL; /* Transfer ownership */
                        mcp_executor_submit(ctx, s->pool, worker_dispatch_fn, task);
                    }
                } else if (frame.opcode == MCP_WS_OP_PING) {
                    mcp_ws_frame_t pong = {
                        .fin = true,
                        .opcode = MCP_WS_OP_PONG,
                        .masked = false,
                        .payload_len = frame.payload_len,
                        .payload = frame.payload,
                    };
                    uint8_t *pbytes = NULL;
                    size_t plen = 0;
                    if (mcp_ws_encode_frame(ctx, &pong, &pbytes, &plen) == MCP_OK) {
                        enqueue_response(s, conn, (char *)pbytes, plen, false);
                    }
                } else if (frame.opcode == MCP_WS_OP_CLOSE) {
                    close_conn(s, conn);
                    mcp_ws_frame_destroy(ctx, &frame);
                    return;
                }
                mcp_ws_frame_destroy(ctx, &frame);
            }
        }
    }
}

static void on_accept(mcp_context_t *ctx, int listen_fd, mcp_event_mask_t events, void *user) {
    mcp_async_server_t *s = (mcp_async_server_t *)user;
    (void)ctx;
    (void)events;

    for (;;) {
        struct sockaddr_in sin;
        socklen_t slen = sizeof(sin);
        int cfd = accept(listen_fd, (struct sockaddr *)&sin, &slen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        int flags = fcntl(cfd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        async_conn_t *conn = srv_malloc(s->ctx, sizeof(*conn));
        if (conn == NULL) {
            close(cfd);
            continue;
        }
        memset(conn, 0, sizeof(*conn));
        conn->fd = cfd;
        atomic_init(&conn->ref_count, 1);
        conn->type = CONN_HTTP_INIT;
        conn->server = s;

        mtx_lock(&s->conns_mtx);
        conn->next = s->conns;
        s->conns = conn;
        mtx_unlock(&s->conns_mtx);

        mcp_reactor_add(s->ctx, s->reactor, cfd, MCP_EVENT_READ, handle_client_read, conn);
    }
}

mcp_async_server_t *mcp_async_server_create(mcp_context_t *ctx, mcp_server_t *server,
                                            mcp_executor_t *pool,
                                            const mcp_async_server_config_t *cfg) {
    if (server == NULL || pool == NULL) {
        return NULL;
    }

    mcp_async_server_t *s = srv_malloc(ctx, sizeof(*s));
    if (s == NULL) return NULL;
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->server = server;
    s->pool = pool;
    if (cfg != NULL) {
        s->cfg = *cfg;
    } else {
        s->cfg.port = 0;
        s->cfg.bind_addr = "0.0.0.0";
        s->cfg.max_connections = 10000;
        s->cfg.keepalive_timeout_s = 60;
        s->cfg.enable_websocket = true;
        s->cfg.enable_sse = true;
    }
    if (s->cfg.bind_addr == NULL) {
        s->cfg.bind_addr = "0.0.0.0";
    }

    mtx_init(&s->resp_mtx, mtx_plain);
    mtx_init(&s->sessions_mtx, mtx_plain);
    mtx_init(&s->conns_mtx, mtx_plain);

    s->reactor = mcp_reactor_create(ctx);
    if (s->reactor == NULL) {
        srv_free(ctx, s);
        return NULL;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        mcp_reactor_destroy(ctx, s->reactor);
        srv_free(ctx, s);
        return NULL;
    }

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int flags = fcntl(lfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(lfd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(s->cfg.port);
    inet_pton(AF_INET, s->cfg.bind_addr, &sin.sin_addr);

    if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
        listen(lfd, 1024) != 0) {
        close(lfd);
        mcp_reactor_destroy(ctx, s->reactor);
        srv_free(ctx, s);
        return NULL;
    }

    socklen_t slen = sizeof(sin);
    if (getsockname(lfd, (struct sockaddr *)&sin, &slen) == 0) {
        s->bound_port = ntohs(sin.sin_port);
    }
    s->listen_fd = lfd;

    mcp_reactor_add(ctx, s->reactor, lfd, MCP_EVENT_READ, on_accept, s);
    return s;
}

uint16_t mcp_async_server_get_port(const mcp_async_server_t *s) {
    return s != NULL ? s->bound_port : 0;
}

static void drain_responses(mcp_async_server_t *s) {
    mtx_lock(&s->resp_mtx);
    response_item_t *head = s->resp_head;
    s->resp_head = NULL;
    s->resp_tail = NULL;
    mtx_unlock(&s->resp_mtx);

    while (head != NULL) {
        response_item_t *item = head;
        head = head->next;

        async_conn_t *conn = item->conn;
        if (conn->fd >= 0) {
            if (conn->write_buf == NULL) {
                conn->write_buf = item->data;
                conn->write_len = item->len;
                conn->write_cap = item->len;
                conn->write_off = 0;
            } else {
                size_t need = conn->write_len + item->len;
                char *nw = srv_malloc(s->ctx, need + 1);
                if (nw != NULL) {
                    memcpy(nw, conn->write_buf, conn->write_len);
                    memcpy(nw + conn->write_len, item->data, item->len);
                    nw[need] = '\0';
                    srv_free(s->ctx, conn->write_buf);
                    srv_free(s->ctx, item->data);
                    conn->write_buf = nw;
                    conn->write_len = need;
                    conn->write_cap = need;
                } else {
                    srv_free(s->ctx, item->data);
                }
            }
            if (item->close_after) {
                atomic_store(&conn->closing, true);
            }
            mcp_reactor_modify(s->ctx, s->reactor, conn->fd, MCP_EVENT_READ | MCP_EVENT_WRITE);
        } else {
            srv_free(s->ctx, item->data);
        }
        conn_unref(conn);
        srv_free(s->ctx, item);
    }
}

mcp_status_t mcp_async_server_step(mcp_context_t *ctx, mcp_async_server_t *s, int timeout_ms) {
    if (s == NULL) return MCP_ERR_INVALID_ARGUMENT;
    drain_responses(s);
    mcp_status_t st = mcp_reactor_step(ctx, s->reactor, timeout_ms);
    drain_responses(s);
    return st;
}

mcp_status_t mcp_async_server_run(mcp_context_t *ctx, mcp_async_server_t *s) {
    if (s == NULL) return MCP_ERR_INVALID_ARGUMENT;
    atomic_store(&s->running, true);
    atomic_store(&s->stop_requested, false);

    while (!atomic_load(&s->stop_requested) && !mcp_shutdown_requested()) {
        mcp_async_server_step(ctx, s, 50);
    }

    atomic_store(&s->running, false);
    return MCP_OK;
}

void mcp_async_server_stop(mcp_context_t *ctx, mcp_async_server_t *s) {
    (void)ctx;
    if (s != NULL) {
        atomic_store(&s->stop_requested, true);
        mcp_reactor_wakeup(s->ctx, s->reactor);
    }
}

void mcp_async_server_destroy(mcp_context_t *ctx, mcp_async_server_t *s) {
    if (s == NULL) return;
    mcp_async_server_stop(ctx, s);

    if (s->listen_fd >= 0) {
        mcp_reactor_remove(ctx, s->reactor, s->listen_fd);
        close(s->listen_fd);
        s->listen_fd = -1;
    }

    mtx_lock(&s->conns_mtx);
    async_conn_t *cur_conn = s->conns;
    s->conns = NULL;
    mtx_unlock(&s->conns_mtx);

    while (cur_conn != NULL) {
        async_conn_t *next = cur_conn->next;
        if (cur_conn->fd >= 0) {
            mcp_reactor_remove(ctx, s->reactor, cur_conn->fd);
            close(cur_conn->fd);
            cur_conn->fd = -1;
        }
        conn_unref(cur_conn);
        cur_conn = next;
    }

    drain_responses(s);

    mtx_lock(&s->sessions_mtx);
    session_entry_t *cur_sess = s->sessions;
    s->sessions = NULL;
    mtx_unlock(&s->sessions_mtx);

    while (cur_sess != NULL) {
        session_entry_t *next = cur_sess->next;
        if (cur_sess->session != NULL && s->server != NULL) {
            mcp_server_destroy_session(ctx, s->server, cur_sess->session);
        }
        mtx_destroy(&cur_sess->session_lock);
        srv_free(ctx, cur_sess);
        cur_sess = next;
    }

    if (s->reactor != NULL) {
        mcp_reactor_destroy(ctx, s->reactor);
        s->reactor = NULL;
    }

    mtx_destroy(&s->resp_mtx);
    mtx_destroy(&s->sessions_mtx);
    mtx_destroy(&s->conns_mtx);
    srv_free(ctx, s);
}
