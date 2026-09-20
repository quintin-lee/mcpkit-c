/**
 * @file test_notify_client.c
 *
 * Proves the server-originated push path: mcp_server_notify_client queues a
 * notification for the transport loop to drain, and the honest L2 method
 * matrix rejects unknown-but-spec-known methods with -32601.
 */
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/dispatcher.h"
#include "mcpkit/server/server.h"

#include "test_check.h"

static const char K_UNKNOWN[] = "sampling/createMessage";
static const char K_INITIALIZED_REQ[] = "notifications/initialized";

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "t", "0.0.1");
    CHECK(srv != NULL);
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);

    // --- outbox: NULL guards
    mcp_message_t *out = NULL;
    CHECK(mcp_server_outbox_pop(ctx, srv, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_ERR_NOT_FOUND);
    CHECK(out == NULL);
    CHECK(mcp_server_notify_client(ctx, NULL, "x", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_server_notify_client(ctx, srv, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    // --- push two notifications, drain in LIFO order.
    mcp_json_value_t *p1 = mcp_json_object_new(ctx);
    mcp_json_value_t *p2 = mcp_json_object_new(ctx);
    CHECK(p1 != NULL && p2 != NULL);
    CHECK(mcp_server_notify_client(ctx, srv, "notifications/tools/list_changed", p1) == MCP_OK);
    CHECK(mcp_server_notify_client(ctx, srv, K_UNKNOWN, p2) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    CHECK(out != NULL);
    const char *m = mcp_message_method(ctx, out);
    CHECK(m != NULL && strcmp(m, K_UNKNOWN) == 0);
    mcp_message_destroy(ctx, out);
    out = NULL;
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    CHECK(out != NULL);
    m = mcp_message_method(ctx, out);
    CHECK(m != NULL && strcmp(m, "notifications/tools/list_changed") == 0);
    mcp_message_destroy(ctx, out);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_ERR_NOT_FOUND);

    // --- outbox survives server destroy (entries freed, no leak).
    CHECK(mcp_server_notify_client(ctx, srv, K_UNKNOWN, mcp_json_object_new(ctx)) == MCP_OK);
    mcp_server_destroy(ctx, srv);

    // --- honest L2: spec-known method without a route is -32601, not a crash.
    srv = mcp_server_create(ctx, "t2", "0.0.1");
    CHECK(srv != NULL);
    s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);
    mcp_message_t *init_req =
        mcp_request_new_string_id(ctx, "1", "initialize",
                                  mcp_initialize_params_new(ctx, "cli", "0.0.1"));
    CHECK(init_req != NULL);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, init_req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    mcp_message_destroy(ctx, init_req);
    mcp_message_destroy(ctx, resp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);

    for (int i = 0; i < 2; i++) {
        const char *method = i == 0 ? K_UNKNOWN : K_INITIALIZED_REQ;
        char idbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%d", 100 + i);
        mcp_message_t *req = mcp_request_new_string_id(ctx, idbuf, method, NULL);
        CHECK(req != NULL);
        resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
        CHECK(resp != NULL);
        int code = 0;
        CHECK(mcp_message_error_code(ctx, resp, &code) == MCP_OK);
        CHECK(code == MCP_RPC_METHOD_NOT_FOUND);
        mcp_message_destroy(ctx, req);
        mcp_message_destroy(ctx, resp);
    }

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
