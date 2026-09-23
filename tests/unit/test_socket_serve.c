#include "test_check.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <threads.h>
#include <time.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/socket.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/core/shutdown.h"

static const uint16_t kPort = 45680;

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                  const mcp_json_value_t *args, void *user_data,
                                  mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *text = mcp_json_object_get(ctx, args, "text");
    const char *s = NULL;
    if (text == NULL || mcp_json_string_value(ctx, text, &s) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *stv = mcp_json_string_new(ctx, s);
    mcp_json_value_t *tyv = mcp_json_string_new(ctx, "text");
    if (item == NULL || content == NULL || result == NULL || stv == NULL || tyv == NULL ||
        mcp_json_object_set(ctx, item, "type", tyv) != MCP_OK ||
        mcp_json_object_set(ctx, item, "text", stv) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK ||
        mcp_json_object_set(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, tyv);
        mcp_json_destroy(ctx, stv);
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

static mcp_server_t *make_server(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "sock-serve", "0.1.0");
    CHECK(srv != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    CHECK(schema != NULL);
    CHECK(mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) ==
           MCP_OK);
    CHECK(mcp_schema_add_required(ctx, schema, "text") == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv,
                               mcp_tool_new(ctx, "echo", "Echo", schema,
                                            echo_handler, NULL)) == MCP_OK);
    return srv;
}

/* Run one MCP request/response cycle on a socket transport.
 * Sends request, reads one line, parses and returns the response.
 * Returns NULL on failure. */
static mcp_message_t *rpc_roundtrip(mcp_context_t *ctx, mcp_transport_t *t,
                                     const char *request) {
    CHECK(mcp_transport_send(ctx, t, request, strlen(request)) == MCP_OK);
    char *line = NULL;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_OK);
    CHECK(line != NULL);
    mcp_message_t *resp = mcp_message_parse(ctx, line, strlen(line));
    mcp_json_free_string(ctx, line);
    return resp;
}

static const char *kInit =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"socket-serve\",\"version\":\"0.1.0\"}}}" /* mcp_transport_send appends \n */;
static const char *kNotif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
static const char *kCall =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello socket serve\"}}}";

static void verify_init(mcp_context_t *ctx, mcp_message_t *resp) {
    CHECK(resp != NULL);
    CHECK(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *res = mcp_message_result(ctx, resp);
    const mcp_json_value_t *pv = mcp_json_object_get(ctx, res, "protocolVersion");
    const char *s = NULL;
    CHECK(pv != NULL && mcp_json_string_value(ctx, pv, &s) == MCP_OK);
    CHECK(strcmp(s, "2025-06-18") == 0);
    mcp_message_destroy(ctx, resp);
}

static void verify_call(mcp_context_t *ctx, mcp_message_t *resp) {
    CHECK(resp != NULL);
    CHECK(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *res = mcp_message_result(ctx, resp);
    const mcp_json_value_t *content = mcp_json_object_get(ctx, res, "content");
    CHECK(content != NULL && mcp_json_array_size(ctx, content) == 1);
    const mcp_json_value_t *item = mcp_json_array_get(ctx, content, 0);
    const mcp_json_value_t *text = mcp_json_object_get(ctx, item, "text");
    const char *s = NULL;
    CHECK(text != NULL && mcp_json_string_value(ctx, text, &s) == MCP_OK);
    CHECK(strcmp(s, "hello socket serve") == 0);
    mcp_message_destroy(ctx, resp);
}

static const char *kSubA =
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/subscribe\","
    "\"params\":{\"uri\":\"file:///srv/a\"}}";
static const char *kSubB =
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/subscribe\","
    "\"params\":{\"uri\":\"file:///srv/b\"}}";

static void verify_ok_result(mcp_context_t *ctx, mcp_message_t *resp) {
    CHECK(resp != NULL);
    CHECK(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    CHECK(mcp_message_result(ctx, resp) != NULL);
    mcp_message_destroy(ctx, resp);
}

/* Background thread running mcp_socket_serve. */
typedef struct {
    mcp_context_t *ctx;
    mcp_server_t  *server;
    mcp_executor_t *pool;
    mcp_status_t   st;
} serve_args_t;

static void *serve_thread_fn(void *arg) {
    serve_args_t *a = (serve_args_t *)arg;
    a->st = mcp_socket_serve(a->ctx, a->server, kPort, a->pool);
    return NULL;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *srv = make_server(ctx);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    /* Start the serve loop in a background thread. */
    serve_args_t args = { .ctx = ctx, .server = srv, .pool = pool, .st = MCP_OK };
    pthread_t thr;
    CHECK(pthread_create(&thr, NULL, serve_thread_fn, &args) == 0);

    /* Give the accept loop a moment to start listening before we connect. */
    for (int i = 0; i < 20; i++) {
        mcp_transport_t *probe = mcp_socket_transport_create(ctx, "127.0.0.1", kPort, false);
        if (probe != NULL) {
            mcp_transport_stop(ctx, probe);
            mcp_transport_destroy(ctx, probe);
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
        thrd_sleep(&ts, NULL);
    }

    /* --- Two concurrent clients: init + echo call --- */
    mcp_transport_t *t1 = mcp_socket_transport_create(ctx, "127.0.0.1", kPort, false);
    CHECK(t1 != NULL);
    CHECK(mcp_transport_start(ctx, t1) == MCP_OK);

    mcp_transport_t *t2 = mcp_socket_transport_create(ctx, "127.0.0.1", kPort, false);
    CHECK(t2 != NULL);
    CHECK(mcp_transport_start(ctx, t2) == MCP_OK);

    /* Both send init simultaneously. */
    CHECK(mcp_transport_send(ctx, t1, kInit, strlen(kInit)) == MCP_OK);
    CHECK(mcp_transport_send(ctx, t2, kInit, strlen(kInit)) == MCP_OK);

    char *l1 = NULL, *l2 = NULL;
    CHECK(mcp_transport_recv(ctx, t1, &l1) == MCP_OK);
    CHECK(mcp_transport_recv(ctx, t2, &l2) == MCP_OK);
    verify_init(ctx, mcp_message_parse(ctx, l1, strlen(l1)));
    mcp_json_free_string(ctx, l1);
    verify_init(ctx, mcp_message_parse(ctx, l2, strlen(l2)));
    mcp_json_free_string(ctx, l2);

    /* Both send the initialized notification. */
    CHECK(mcp_transport_send(ctx, t1, kNotif, strlen(kNotif)) == MCP_OK);
    CHECK(mcp_transport_send(ctx, t2, kNotif, strlen(kNotif)) == MCP_OK);

    /* Both call echo. */
    mcp_message_t *r1 = rpc_roundtrip(ctx, t1, kCall);
    mcp_message_t *r2 = rpc_roundtrip(ctx, t2, kCall);
    verify_call(ctx, r1);
    verify_call(ctx, r2);

    /* --- Concurrent resources/subscribe (exercises subscribed_lock) --- */
    CHECK(mcp_transport_send(ctx, t1, kSubA, strlen(kSubA)) == MCP_OK);
    CHECK(mcp_transport_send(ctx, t2, kSubB, strlen(kSubB)) == MCP_OK);
    char *ls1 = NULL, *ls2 = NULL;
    CHECK(mcp_transport_recv(ctx, t1, &ls1) == MCP_OK);
    CHECK(mcp_transport_recv(ctx, t2, &ls2) == MCP_OK);
    verify_ok_result(ctx, mcp_message_parse(ctx, ls1, strlen(ls1)));
    mcp_json_free_string(ctx, ls1);
    verify_ok_result(ctx, mcp_message_parse(ctx, ls2, strlen(ls2)));
    mcp_json_free_string(ctx, ls2);

    /* --- Shutdown: close both clients, then signal the serve loop --- */
    CHECK(mcp_transport_stop(ctx, t1) == MCP_OK);
    mcp_transport_destroy(ctx, t1);
    CHECK(mcp_transport_stop(ctx, t2) == MCP_OK);
    mcp_transport_destroy(ctx, t2);

    mcp_request_shutdown();
    CHECK(pthread_join(thr, NULL) == 0);
    CHECK(args.st == MCP_OK || args.st == MCP_ERR_CANCELLED);

    /* The pool was drained inside mcp_socket_serve; verify it is usable
     * (no leak) by destroying it cleanly. */
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, srv);
    mcp_shutdown_clear();
    mcp_context_destroy(ctx);
    printf("test_socket_serve OK\n");
    return 0;
}
