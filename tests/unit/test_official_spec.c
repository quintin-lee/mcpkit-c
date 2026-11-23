#define _DEFAULT_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    int call_count;
    char last_payload[2048];
} mock_sink_t;

static mcp_status_t mock_session_sink(mcp_context_t *ctx,
                                            mcp_session_t *sess,
                                            const char *data,
                                            size_t len,
                                            void *user_data) {
    (void)ctx;
    (void)sess;
    mock_sink_t *sink = (mock_sink_t *)user_data;
    if (sink != NULL && data != NULL) {
        sink->call_count++;
        size_t copy_len = len < sizeof(sink->last_payload) - 1 ? len : sizeof(sink->last_payload) - 1;
        memcpy(sink->last_payload, data, copy_len);
        sink->last_payload[copy_len] = '\0';
    }
    return MCP_OK;
}

static mcp_status_t dummy_tool_fn(mcp_context_t *ctx, mcp_session_t *session,
                                  const mcp_json_value_t *arguments,
                                  void *user_data,
                                  mcp_json_value_t **result) {
    (void)session;
    (void)arguments;
    (void)user_data;
    *result = mcp_json_object_new(ctx);
    return MCP_OK;
}

static mcp_status_t dummy_resource_read(mcp_context_t *ctx, mcp_session_t *session,
                                        const char *uri, void *user_data,
                                        mcp_json_value_t **contents) {
    (void)session;
    (void)uri;
    (void)user_data;
    *contents = mcp_json_array_new(ctx);
    return MCP_OK;
}

static mcp_status_t dummy_prompt_get(mcp_context_t *ctx, mcp_session_t *session,
                                     const mcp_json_value_t *arguments,
                                     void *user_data,
                                     mcp_json_value_t **messages) {
    (void)session;
    (void)arguments;
    (void)user_data;
    *messages = mcp_json_array_new(ctx);
    return MCP_OK;
}

/* =========================================================================
 * Item 1: Protocol handshake and capabilities negotiation
 * ========================================================================= */
static void test_item1_capabilities_and_resource_notifications(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "test-item1-srv", "1.0.0");
    assert(srv != NULL);

    /* 1. Register a tool */
    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_tool_t *tool = mcp_tool_new(ctx, "test_tool", "A dummy tool", schema, dummy_tool_fn, NULL);
    assert(tool != NULL);
    assert(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);
    (void)tool;

    /* 2. Register a resource */
    mcp_resource_t *res = mcp_resource_new(ctx, "file:///dummy/test.txt", "Test Resource",
                                          "text/plain", dummy_resource_read, NULL);
    assert(res != NULL);
    assert(mcp_server_add_resource(ctx, srv, res) == MCP_OK);
    (void)res;

    /* 3. Register a prompt */
    mcp_prompt_t *prompt = mcp_prompt_new(ctx, "test_prompt", "A dummy prompt", dummy_prompt_get, NULL);
    assert(prompt != NULL);
    assert(mcp_server_add_prompt(ctx, srv, prompt) == MCP_OK);
    (void)prompt;

    /* 4. Enable tasks extension */
    assert(mcp_server_enable_tasks(ctx, srv) == MCP_OK);

    /* 5. Set custom capabilities */
    mcp_json_value_t *custom = mcp_json_object_new(ctx);
    assert(custom != NULL);
    mcp_json_value_t *exp = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, exp, "foo", mcp_json_string_new(ctx, "bar"));
    mcp_json_object_set_take(ctx, custom, "experimental", exp);
    assert(mcp_server_set_capabilities(ctx, srv, custom) == MCP_OK);
    mcp_json_destroy(ctx, custom);

    /* 6. Create session and dispatch initialize */
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    mock_sink_t sink = {0};
    mcp_session_set_sink(sess, mock_session_sink, &sink);

    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "test-client", "1.0.0");
    assert(ip != NULL);
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "init-1", "initialize", ip);
    assert(ireq != NULL);

    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    assert(iresp != NULL);

    const mcp_json_value_t *result = mcp_message_result(ctx, iresp);
    assert(result != NULL);

    const mcp_json_value_t *caps = mcp_json_object_get(ctx, result, "capabilities");
    assert(caps != NULL);

    /* Verify logging */
    const mcp_json_value_t *log_cap = mcp_json_object_get(ctx, caps, "logging");
    assert(log_cap != NULL);
    (void)log_cap;

    /* Verify tools.listChanged */
    const mcp_json_value_t *tools_cap = mcp_json_object_get(ctx, caps, "tools");
    assert(tools_cap != NULL);
    const mcp_json_value_t *t_lc = mcp_json_object_get(ctx, tools_cap, "listChanged");
    assert(t_lc != NULL);
    (void)t_lc;
    bool b_val = false;
    assert(mcp_json_bool_value(ctx, t_lc, &b_val) == MCP_OK && b_val == true);

    /* Verify resources.subscribe & resources.listChanged */
    const mcp_json_value_t *res_cap = mcp_json_object_get(ctx, caps, "resources");
    assert(res_cap != NULL);
    const mcp_json_value_t *r_sub = mcp_json_object_get(ctx, res_cap, "subscribe");
    assert(r_sub != NULL && mcp_json_bool_value(ctx, r_sub, &b_val) == MCP_OK && b_val == true);
    (void)r_sub;
    const mcp_json_value_t *r_lc = mcp_json_object_get(ctx, res_cap, "listChanged");
    assert(r_lc != NULL && mcp_json_bool_value(ctx, r_lc, &b_val) == MCP_OK && b_val == true);
    (void)r_lc;

    /* Verify prompts.listChanged */
    const mcp_json_value_t *prompts_cap = mcp_json_object_get(ctx, caps, "prompts");
    assert(prompts_cap != NULL);
    const mcp_json_value_t *p_lc = mcp_json_object_get(ctx, prompts_cap, "listChanged");
    assert(p_lc != NULL && mcp_json_bool_value(ctx, p_lc, &b_val) == MCP_OK && b_val == true);
    (void)p_lc;

    /* Verify extensions */
    const mcp_json_value_t *exts_cap = mcp_json_object_get(ctx, caps, "extensions");
    assert(exts_cap != NULL);
    assert(mcp_json_object_get(ctx, exts_cap, "io.modelcontextprotocol/tasks") != NULL);
    (void)exts_cap;

    /* Verify custom capability merge */
    const mcp_json_value_t *exp_cap = mcp_json_object_get(ctx, caps, "experimental");
    assert(exp_cap != NULL);
    const mcp_json_value_t *foo_val = mcp_json_object_get(ctx, exp_cap, "foo");
    assert(foo_val != NULL);
    (void)foo_val;
    const char *foo_str = NULL;
    assert(mcp_json_string_value(ctx, foo_val, &foo_str) == MCP_OK && strcmp(foo_str, "bar") == 0);
    (void)foo_str;
    (void)b_val;

    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);

    /* Send initialized notification */
    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(init_notif != NULL);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* Test resources list changed notification emission */
    assert(sink.call_count == 0);
    assert(mcp_server_notify_resources_changed(ctx, srv) == MCP_OK);
    assert(sink.call_count == 1);
    assert(strstr(sink.last_payload, "notifications/resources/list_changed") != NULL);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_item1_capabilities_and_resource_notifications\n");
}

int main(void) {
    printf("Starting Official MCP Spec Compliance Tests...\n");
    test_item1_capabilities_and_resource_notifications();
    printf("All Official MCP Spec Compliance Tests Passed!\n");
    return 0;
}
