#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/dispatcher.h"
#include "mcpkit/server/resource.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    char last_payload[512];
    size_t last_len;
    int call_count;
} mock_sink_t;

static mcp_status_t mock_session_sink(mcp_context_t *ctx, mcp_session_t *session,
                                      const char *payload, size_t len, void *userdata) {
    (void)ctx;
    (void)session;
    mock_sink_t *s = (mock_sink_t *)userdata;
    s->call_count++;
    if (len < sizeof(s->last_payload)) {
        memcpy(s->last_payload, payload, len);
        s->last_payload[len] = '\0';
    }
    s->last_len = len;
    return MCP_OK;
}

static void init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *sess) {
    const char *init_req =
        "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";
    mcp_message_t *msg = mcp_message_parse(ctx, init_req, strlen(init_req));
    ASSERT_TRUE(msg != NULL);
    mcp_message_t *resp = NULL;
    ASSERT_TRUE(mcp_server_dispatch(ctx, srv, sess, msg, &resp) == MCP_OK);
    mcp_message_destroy(ctx, msg);
    mcp_message_destroy(ctx, resp);

    const char *inited_notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    msg = mcp_message_parse(ctx, inited_notif, strlen(inited_notif));
    ASSERT_TRUE(msg != NULL);
    ASSERT_TRUE(mcp_server_notify(ctx, srv, sess, msg) == MCP_OK);
    mcp_message_destroy(ctx, msg);
    ASSERT_TRUE(mcp_session_is_initialized(ctx, sess));
}

static void test_subscribe_unsubscribe_routes(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "sub-test-srv", "1.0");
    ASSERT_TRUE(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    ASSERT_TRUE(sess != NULL);

    init_session(ctx, srv, sess);

    /* 1. Subscribe */
    const char *sub_req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/subscribe\",\"params\":{\"uri\":\"resource://data\"}}";
    mcp_message_t *msg = mcp_message_parse(ctx, sub_req, strlen(sub_req));
    ASSERT_TRUE(msg != NULL);

    mcp_message_t *resp = NULL;
    mcp_status_t status = mcp_server_dispatch(ctx, srv, sess, msg, &resp);
    ASSERT_TRUE(status == MCP_OK && resp != NULL);

    char *resp_str = mcp_message_serialize(ctx, resp);
    ASSERT_TRUE(resp_str != NULL);
    ASSERT_TRUE(mcp_message_result(ctx, resp) != NULL);
    mcp_json_free_string(ctx, resp_str);
    mcp_message_destroy(ctx, msg);
    mcp_message_destroy(ctx, resp);

    ASSERT_TRUE(mcp_session_is_subscribed_to_resource(sess, "resource://data") == true);
    ASSERT_TRUE(mcp_session_is_subscribed_to_resource(sess, "resource://other") == false);

    /* 2. Unsubscribe */
    const char *unsub_req =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/unsubscribe\",\"params\":{\"uri\":\"resource://data\"}}";
    msg = mcp_message_parse(ctx, unsub_req, strlen(unsub_req));
    ASSERT_TRUE(msg != NULL);
    resp = NULL;
    status = mcp_server_dispatch(ctx, srv, sess, msg, &resp);
    ASSERT_TRUE(status == MCP_OK && resp != NULL);

    resp_str = mcp_message_serialize(ctx, resp);
    ASSERT_TRUE(resp_str != NULL);
    ASSERT_TRUE(mcp_message_result(ctx, resp) != NULL);
    mcp_json_free_string(ctx, resp_str);
    mcp_message_destroy(ctx, msg);
    mcp_message_destroy(ctx, resp);

    ASSERT_TRUE(mcp_session_is_subscribed_to_resource(sess, "resource://data") == false);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void test_targeted_resource_notification(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "sub-test-srv", "1.0");
    ASSERT_TRUE(srv != NULL);

    mcp_session_t *sess1 = mcp_server_create_session(ctx, srv);
    mcp_session_t *sess2 = mcp_server_create_session(ctx, srv);
    ASSERT_TRUE(sess1 != NULL && sess2 != NULL);

    mock_sink_t sink1 = {0};
    mock_sink_t sink2 = {0};
    mcp_session_set_sink(sess1, mock_session_sink, &sink1);
    mcp_session_set_sink(sess2, mock_session_sink, &sink2);

    /* sess1 subscribes to res://alpha; sess2 subscribes to res://beta */
    ASSERT_TRUE(mcp_session_subscribe_resource(ctx, sess1, "res://alpha") == MCP_OK);
    ASSERT_TRUE(mcp_session_subscribe_resource(ctx, sess2, "res://beta") == MCP_OK);

    /* Notify res://alpha */
    mcp_status_t status = mcp_server_notify_resource_updated(ctx, srv, "res://alpha");
    ASSERT_TRUE(status == MCP_OK);

    ASSERT_TRUE(sink1.call_count == 1);
    ASSERT_TRUE(strstr(sink1.last_payload, "notifications/resources/updated") != NULL);
    ASSERT_TRUE(strstr(sink1.last_payload, "res://alpha") != NULL);
    ASSERT_TRUE(strstr(sink1.last_payload, "subscriptionId") == NULL); /* No SEP-2575 id */

    ASSERT_TRUE(sink2.call_count == 0); /* sess2 was not subscribed to res://alpha */

    /* Notify tools changed */
    status = mcp_server_notify_tools_changed(ctx, srv);
    ASSERT_TRUE(status == MCP_OK);
    ASSERT_TRUE(sink1.call_count == 2);
    ASSERT_TRUE(sink2.call_count == 1);
    ASSERT_TRUE(strstr(sink1.last_payload, "notifications/tools/list_changed") != NULL);
    ASSERT_TRUE(strstr(sink2.last_payload, "notifications/tools/list_changed") != NULL);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_subscribe_unsubscribe_routes();
    test_targeted_resource_notification();
    printf("All test_subscription_engine tests passed!\n");
    return 0;
}
