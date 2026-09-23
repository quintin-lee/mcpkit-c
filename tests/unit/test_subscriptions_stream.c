#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/dispatcher.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

#include "test_check.h"

static void init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    mcp_message_t *init_req =
        mcp_request_new_string_id(ctx, "init-1", "initialize",
                                  mcp_initialize_params_new(ctx, "test-client", "1.0.0"));
    CHECK(init_req != NULL);
    mcp_message_t *init_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, init_req, &init_resp) == MCP_OK);
    CHECK(init_resp != NULL);
    mcp_message_destroy(ctx, init_req);
    mcp_message_destroy(ctx, init_resp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "sub-server", "1.0.0");
    CHECK(srv != NULL);
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);

    init_session(ctx, srv, s);
    CHECK(!mcp_session_has_active_subscription(ctx, s));

    // 1. Dispatch subscriptions/listen with string ID "sub-alpha"
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    mcp_json_value_t *notifs = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, notifs, "toolsListChanged", mcp_json_bool_new(ctx, true)) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, notifs, "promptsListChanged", mcp_json_bool_new(ctx, false)) == MCP_OK);
    mcp_json_value_t *uris = mcp_json_array_new(ctx);
    CHECK(mcp_json_array_append(ctx, uris, mcp_json_string_new(ctx, "file:///data/target.txt")) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, notifs, "resourceSubscriptions", uris) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "notifications", notifs) == MCP_OK);

    mcp_message_t *listen_req = mcp_request_new_string_id(ctx, "sub-alpha", "subscriptions/listen", p);
    CHECK(listen_req != NULL);
    mcp_message_t *ack = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, listen_req, &ack) == MCP_OK);
    CHECK(ack != NULL);
    CHECK(mcp_message_kind(ctx, ack) == MCP_MSG_NOTIFICATION);
    CHECK(strcmp(mcp_message_method(ctx, ack), "notifications/subscriptions/acknowledged") == 0);
    mcp_message_destroy(ctx, listen_req);
    mcp_message_destroy(ctx, ack);

    CHECK(mcp_session_has_active_subscription(ctx, s));

    // 2. Check filter: toolsListChanged should be allowed
    CHECK(mcp_session_is_subscribed_to_notification(ctx, s, "notifications/tools/list_changed", NULL));
    // promptsListChanged should be filtered out
    CHECK(!mcp_session_is_subscribed_to_notification(ctx, s, "notifications/prompts/list_changed", NULL));

    // 3. Notify toolsListChanged via mcp_server_session_notify
    CHECK(mcp_server_session_notify(ctx, srv, s, "notifications/tools/list_changed", NULL) == MCP_OK);
    mcp_message_t *out = NULL;
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    CHECK(out != NULL);
    CHECK(strcmp(mcp_message_method(ctx, out), "notifications/tools/list_changed") == 0);
    const mcp_json_value_t *out_params = mcp_message_params(ctx, out);
    CHECK(out_params != NULL);
    const mcp_json_value_t *meta = mcp_json_object_get(ctx, out_params, "_meta");
    CHECK(meta != NULL);
    const mcp_json_value_t *sub_id_v = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId");
    CHECK(sub_id_v != NULL);
    const char *sub_id_str = NULL;
    CHECK(mcp_json_string_value(ctx, sub_id_v, &sub_id_str) == MCP_OK);
    CHECK(strcmp(sub_id_str, "sub-alpha") == 0);
    mcp_message_destroy(ctx, out);
    out = NULL;

    // 4. Notify promptsListChanged: should be dropped silently
    CHECK(mcp_server_session_notify(ctx, srv, s, "notifications/prompts/list_changed", NULL) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_ERR_NOT_FOUND);

    // 5. Notify resources/updated with non-matching uri: should be dropped
    mcp_json_value_t *ru_bad = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, ru_bad, "uri", mcp_json_string_new(ctx, "file:///other.txt")) == MCP_OK);
    CHECK(mcp_server_session_notify(ctx, srv, s, "notifications/resources/updated", ru_bad) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_ERR_NOT_FOUND);

    // 6. Notify resources/updated with matching uri: should be tagged & enqueued
    mcp_json_value_t *ru_good = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, ru_good, "uri", mcp_json_string_new(ctx, "file:///data/target.txt")) == MCP_OK);
    CHECK(mcp_server_session_notify(ctx, srv, s, "notifications/resources/updated", ru_good) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    CHECK(out != NULL);
    CHECK(strcmp(mcp_message_method(ctx, out), "notifications/resources/updated") == 0);
    out_params = mcp_message_params(ctx, out);
    CHECK(out_params != NULL);
    const mcp_json_value_t *uri_back = mcp_json_object_get(ctx, out_params, "uri");
    const char *uri_str = NULL;
    CHECK(uri_back != NULL && mcp_json_string_value(ctx, uri_back, &uri_str) == MCP_OK);
    CHECK(strcmp(uri_str, "file:///data/target.txt") == 0);
    meta = mcp_json_object_get(ctx, out_params, "_meta");
    CHECK(meta != NULL);
    sub_id_v = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId");
    CHECK(sub_id_v != NULL);
    CHECK(mcp_json_string_value(ctx, sub_id_v, &sub_id_str) == MCP_OK && strcmp(sub_id_str, "sub-alpha") == 0);
    mcp_message_destroy(ctx, out);
    out = NULL;

    // 7. Graceful closure of subscription stream
    mcp_message_t *closure_resp = NULL;
    CHECK(mcp_server_session_close_subscription(ctx, srv, s, &closure_resp) == MCP_OK);
    CHECK(closure_resp != NULL);
    CHECK(mcp_message_kind(ctx, closure_resp) == MCP_MSG_RESPONSE);
    CHECK(mcp_message_id_type(ctx, closure_resp) == MCP_ID_STRING);
    CHECK(strcmp(mcp_message_id_string(ctx, closure_resp), "sub-alpha") == 0);
    const mcp_json_value_t *res = mcp_message_result(ctx, closure_resp);
    CHECK(res != NULL);
    const mcp_json_value_t *res_type = mcp_json_object_get(ctx, res, "resultType");
    const char *rt = NULL;
    CHECK(res_type != NULL && mcp_json_string_value(ctx, res_type, &rt) == MCP_OK);
    CHECK(strcmp(rt, "complete") == 0);
    meta = mcp_json_object_get(ctx, res, "_meta");
    CHECK(meta != NULL);
    sub_id_v = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId");
    CHECK(sub_id_v != NULL && mcp_json_string_value(ctx, sub_id_v, &sub_id_str) == MCP_OK);
    CHECK(strcmp(sub_id_str, "sub-alpha") == 0);
    mcp_message_destroy(ctx, closure_resp);

    // Subscription is no longer active
    CHECK(!mcp_session_has_active_subscription(ctx, s));
    CHECK(mcp_server_session_close_subscription(ctx, srv, s, &closure_resp) == MCP_ERR_NOT_FOUND);

    // 8. Test numeric subscription ID
    p = mcp_json_object_new(ctx);
    notifs = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, notifs, "toolsListChanged", mcp_json_bool_new(ctx, true)) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "notifications", notifs) == MCP_OK);
    listen_req = mcp_request_new_number_id(ctx, 501.0, "subscriptions/listen", p);
    CHECK(listen_req != NULL);
    ack = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, listen_req, &ack) == MCP_OK);
    CHECK(ack != NULL);
    mcp_message_destroy(ctx, listen_req);
    mcp_message_destroy(ctx, ack);

    CHECK(mcp_session_has_active_subscription(ctx, s));

    // Test notify with numeric ID
    CHECK(mcp_server_session_notify(ctx, srv, s, "notifications/tools/list_changed", NULL) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    out_params = mcp_message_params(ctx, out);
    meta = mcp_json_object_get(ctx, out_params, "_meta");
    sub_id_v = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId");
    double num_id = 0;
    CHECK(sub_id_v != NULL && mcp_json_number_value(ctx, sub_id_v, &num_id) == MCP_OK);
    CHECK(num_id == 501.0);
    mcp_message_destroy(ctx, out);

    // Close subscription directly to outbox
    CHECK(mcp_server_session_close_subscription(ctx, srv, s, NULL) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    CHECK(mcp_message_kind(ctx, out) == MCP_MSG_RESPONSE);
    CHECK(mcp_message_id_type(ctx, out) == MCP_ID_NUMBER);
    CHECK(mcp_message_id_number(ctx, out, &num_id) == MCP_OK && num_id == 501.0);
    res = mcp_message_result(ctx, out);
    meta = mcp_json_object_get(ctx, res, "_meta");
    sub_id_v = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId");
    CHECK(sub_id_v != NULL && mcp_json_number_value(ctx, sub_id_v, &num_id) == MCP_OK && num_id == 501.0);
    mcp_message_destroy(ctx, out);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
