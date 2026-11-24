#define _DEFAULT_SOURCE
#undef NDEBUG

#include <assert.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/sampling.h"


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

/* =========================================================================
 * Item 2: Completion parameter schema alignment (ref/prompt, ref/resource)
 * ========================================================================= */
static mcp_json_value_t *test_completion_provider(mcp_context_t *ctx,
                                                 mcp_session_t *session,
                                                 const mcp_json_value_t *argument,
                                                 void *user_data) {
    (void)session;
    (void)argument;
    (void)user_data;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, "python"));
    mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, "perl"));
    return arr;
}

static void test_item2_completion_schema_alignment(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "test-item2-srv", "1.0.0");
    assert(srv != NULL);

    assert(mcp_server_register_completion_provider(ctx, srv, "code_template", test_completion_provider, NULL) == MCP_OK);
    assert(mcp_server_register_completion_provider(ctx, srv, "resource/docs", test_completion_provider, NULL) == MCP_OK);

    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    /* Initialize session */
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "test-client", "1.0.0");
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "init-2", "initialize", ip);
    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);

    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* Test 1: Official schema ref/prompt */
    {
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_value_t *ref_obj = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, ref_obj, "type", mcp_json_string_new(ctx, "ref/prompt"));
        mcp_json_object_set_take(ctx, ref_obj, "name", mcp_json_string_new(ctx, "code_template"));
        mcp_json_object_set_take(ctx, params, "ref", ref_obj);

        mcp_json_value_t *arg_obj = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, arg_obj, "name", mcp_json_string_new(ctx, "lang"));
        mcp_json_object_set_take(ctx, arg_obj, "value", mcp_json_string_new(ctx, "p"));
        mcp_json_object_set_take(ctx, params, "argument", arg_obj);

        mcp_message_t *req = mcp_request_new_string_id(ctx, "comp-1", "completion/complete", params);
        mcp_message_t *resp = NULL;
        assert(mcp_server_dispatch(ctx, srv, sess, req, &resp) == MCP_OK);
        assert(resp != NULL);

        const mcp_json_value_t *res = mcp_message_result(ctx, resp);
        assert(res != NULL);

        /* Verify official completion structure */
        const mcp_json_value_t *comp_obj = mcp_json_object_get(ctx, res, "completion");
        assert(comp_obj != NULL);
        const mcp_json_value_t *values = mcp_json_object_get(ctx, comp_obj, "values");
        assert(values != NULL && mcp_json_array_size(ctx, values) == 2);
        (void)values;

        const mcp_json_value_t *total = mcp_json_object_get(ctx, comp_obj, "total");
        double d_total = 0;
        assert(total != NULL && mcp_json_number_value(ctx, total, &d_total) == MCP_OK && d_total == 2.0);
        (void)total;
        (void)d_total;

        /* Verify backwards-compat completions */
        const mcp_json_value_t *legacy_comps = mcp_json_object_get(ctx, res, "completions");
        assert(legacy_comps != NULL && mcp_json_array_size(ctx, legacy_comps) == 2);
        (void)legacy_comps;

        mcp_message_destroy(ctx, req);
        mcp_message_destroy(ctx, resp);
    }

    /* Test 2: Official schema ref/resource */
    {
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_value_t *ref_obj = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, ref_obj, "type", mcp_json_string_new(ctx, "ref/resource"));
        mcp_json_object_set_take(ctx, ref_obj, "uri", mcp_json_string_new(ctx, "resource/docs/api"));
        mcp_json_object_set_take(ctx, params, "ref", ref_obj);

        mcp_json_value_t *arg_obj = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, arg_obj, "name", mcp_json_string_new(ctx, "uri"));
        mcp_json_object_set_take(ctx, arg_obj, "value", mcp_json_string_new(ctx, "res"));
        mcp_json_object_set_take(ctx, params, "argument", arg_obj);

        mcp_message_t *req = mcp_request_new_string_id(ctx, "comp-2", "completion/complete", params);
        mcp_message_t *resp = NULL;
        assert(mcp_server_dispatch(ctx, srv, sess, req, &resp) == MCP_OK);
        assert(resp != NULL);

        const mcp_json_value_t *res = mcp_message_result(ctx, resp);
        assert(res != NULL);
        const mcp_json_value_t *comp_obj = mcp_json_object_get(ctx, res, "completion");
        assert(comp_obj != NULL);
        const mcp_json_value_t *values = mcp_json_object_get(ctx, comp_obj, "values");
        assert(values != NULL && mcp_json_array_size(ctx, values) == 2);
        (void)values;
        (void)comp_obj;

        mcp_message_destroy(ctx, req);
        mcp_message_destroy(ctx, resp);
    }

    /* Test 3: Legacy schema {"type":"ref", "value":"code_template"} */
    {
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_value_t *ref_obj = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, ref_obj, "type", mcp_json_string_new(ctx, "ref"));
        mcp_json_object_set_take(ctx, ref_obj, "value", mcp_json_string_new(ctx, "code_template"));
        mcp_json_object_set_take(ctx, params, "ref", ref_obj);

        mcp_json_value_t *arg_obj = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, arg_obj, "name", mcp_json_string_new(ctx, "lang"));
        mcp_json_object_set_take(ctx, arg_obj, "value", mcp_json_string_new(ctx, "p"));
        mcp_json_object_set_take(ctx, params, "argument", arg_obj);


        mcp_message_t *req = mcp_request_new_string_id(ctx, "comp-3", "completion/complete", params);
        mcp_message_t *resp = NULL;
        assert(mcp_server_dispatch(ctx, srv, sess, req, &resp) == MCP_OK);
        assert(resp != NULL);

        const mcp_json_value_t *res = mcp_message_result(ctx, resp);
        assert(res != NULL);
        const mcp_json_value_t *comp_obj = mcp_json_object_get(ctx, res, "completion");
        assert(comp_obj != NULL);
        (void)comp_obj;

        mcp_message_destroy(ctx, req);
        mcp_message_destroy(ctx, resp);
    }

    (void)sess;

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_item2_completion_schema_alignment\n");
}

/* =========================================================================
 * Item 3: Request-level cancellation mechanism and notifications/cancelled
 * ========================================================================= */
typedef struct {
    mcp_server_t *srv;
    mcp_session_t *sess;
    atomic_bool tool_started;
} cancel_test_env_t;

static void *cancel_notifier_thread(void *arg) {
    cancel_test_env_t *env = (cancel_test_env_t *)arg;
    while (!atomic_load(&env->tool_started)) {
        usleep(1000); /* 1ms */
    }
    usleep(5000); /* 5ms */

    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, params, "requestId", mcp_json_string_new(ctx, "long-tool-1"));
    mcp_json_object_set_take(ctx, params, "reason", mcp_json_string_new(ctx, "user cancelled operation"));
    mcp_message_t *notif = mcp_notification_new(ctx, "notifications/cancelled", params);

    mcp_server_notify(ctx, env->srv, env->sess, notif);
    mcp_message_destroy(ctx, notif);
    mcp_context_destroy(ctx);
    return NULL;
}

static mcp_status_t cancelling_tool_v2(mcp_context_t *ctx,
                                      mcp_session_t *session,
                                      const mcp_tool_call_ctx_t *call_ctx,
                                      void *user_data,
                                      mcp_json_value_t **result) {
    (void)session;
    cancel_test_env_t *env = (cancel_test_env_t *)user_data;
    atomic_store(&env->tool_started, true);

    int elapsed_ms = 0;
    while (!mcp_tool_is_cancelled(call_ctx) && elapsed_ms < 500) {
        usleep(5000); /* 5ms */
        elapsed_ms += 5;
    }

    if (mcp_tool_is_cancelled(call_ctx)) {
        const char *reason = mcp_tool_cancel_reason(call_ctx);
        assert(reason != NULL && strcmp(reason, "user cancelled operation") == 0);
        return MCP_ERR_CANCELLED;
    }

    *result = mcp_json_object_new(ctx);
    return MCP_OK;
}

static void test_item3_cancellation_mechanism(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "test-item3-srv", "1.0.0");
    assert(srv != NULL);

    cancel_test_env_t env;
    memset(&env, 0, sizeof(env));
    env.srv = srv;
    atomic_init(&env.tool_started, false);

    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "long_op", "A long operation tool", NULL,
                                       cancelling_tool_v2, &env);
    assert(tool != NULL);
    assert(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);
    env.sess = sess;

    mock_sink_t sink = {0};
    mcp_session_set_sink(sess, mock_session_sink, &sink);

    /* Initialize session */
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "test-client", "1.0.0");
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "init-3", "initialize", ip);
    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);

    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* Test 1: Server cancel request notification */
    assert(mcp_server_cancel_request(ctx, srv, sess, "srv-req-1", 0, "server abort") == MCP_OK);
    assert(sink.call_count == 1);
    assert(strstr(sink.last_payload, "notifications/cancelled") != NULL);
    assert(strstr(sink.last_payload, "srv-req-1") != NULL);
    assert(strstr(sink.last_payload, "server abort") != NULL);

    /* Test 2: In-flight tool cancellation */
    pthread_t th;
    int rc = pthread_create(&th, NULL, cancel_notifier_thread, &env);
    assert(rc == 0);
    (void)rc;

    mcp_json_value_t *call_params = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, call_params, "name", mcp_json_string_new(ctx, "long_op"));
    mcp_json_object_set_take(ctx, call_params, "arguments", mcp_json_object_new(ctx));

    mcp_message_t *req = mcp_request_new_string_id(ctx, "long-tool-1", "tools/call", call_params);
    mcp_message_t *resp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, req, &resp) == MCP_OK);
    assert(resp != NULL);

    pthread_join(th, NULL);

    /* Verify response is tool error with cancelled status */
    const mcp_json_value_t *res = mcp_message_result(ctx, resp);
    assert(res != NULL);
    const mcp_json_value_t *is_err = mcp_json_object_get(ctx, res, "isError");
    bool b_err = false;
    assert(is_err != NULL && mcp_json_bool_value(ctx, is_err, &b_err) == MCP_OK && b_err == true);
    (void)is_err;
    (void)b_err;
    (void)tool;


    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_item3_cancellation_mechanism\n");
}

/* =========================================================================
 * Item 4: Server logging notifications (notifications/message)
 * ========================================================================= */
static void test_item4_server_logging_notifications(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "test-item4-srv", "1.0.0");
    assert(srv != NULL);

    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    mock_sink_t sink = {0};
    mcp_session_set_sink(sess, mock_session_sink, &sink);

    /* Initialize session */
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "test-client", "1.0.0");
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "init-4", "initialize", ip);
    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);

    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* 1. Send log without level filter set */
    assert(mcp_server_send_log_text(ctx, srv, sess, MCP_LOG_INFO, "db", "db connected") == MCP_OK);
    assert(sink.call_count == 1);
    assert(strstr(sink.last_payload, "notifications/message") != NULL);
    assert(strstr(sink.last_payload, "\"level\":\"info\"") != NULL);
    assert(strstr(sink.last_payload, "\"logger\":\"db\"") != NULL);
    assert(strstr(sink.last_payload, "\"data\":\"db connected\"") != NULL);

    /* 2. Set level to warning */
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, params, "level", mcp_json_string_new(ctx, "warning"));
    mcp_message_t *set_lvl_req = mcp_request_new_string_id(ctx, "lvl-1", "logging/setLevel", params);
    mcp_message_t *set_lvl_resp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, set_lvl_req, &set_lvl_resp) == MCP_OK);
    assert(set_lvl_resp != NULL);
    assert(mcp_message_result(ctx, set_lvl_resp) != NULL);
    mcp_message_destroy(ctx, set_lvl_req);
    mcp_message_destroy(ctx, set_lvl_resp);

    /* 3. Send DEBUG: should be filtered out */
    sink.call_count = 0;
    assert(mcp_server_send_log_text(ctx, srv, sess, MCP_LOG_DEBUG, "db", "debug detail") == MCP_OK);
    assert(sink.call_count == 0);

    /* 4. Send INFO: should be filtered out */
    assert(mcp_server_send_log_text(ctx, srv, sess, MCP_LOG_INFO, "db", "info detail") == MCP_OK);
    assert(sink.call_count == 0);

    /* 5. Send WARN: should pass */
    assert(mcp_server_send_log_text(ctx, srv, sess, MCP_LOG_WARN, "db", "warning detail") == MCP_OK);
    assert(sink.call_count == 1);
    assert(strstr(sink.last_payload, "notifications/message") != NULL);
    assert(strstr(sink.last_payload, "\"level\":\"warning\"") != NULL);
    assert(strstr(sink.last_payload, "\"data\":\"warning detail\"") != NULL);

    /* 6. Send ERROR with structured JSON data */
    mcp_json_value_t *struct_data = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, struct_data, "status", mcp_json_number_new(ctx, 500));
    mcp_json_object_set_take(ctx, struct_data, "msg", mcp_json_string_new(ctx, "internal error"));
    assert(mcp_server_send_log(ctx, srv, sess, MCP_LOG_ERROR, "auth", struct_data) == MCP_OK);
    mcp_json_destroy(ctx, struct_data);
    assert(sink.call_count == 2);
    assert(strstr(sink.last_payload, "\"level\":\"error\"") != NULL);
    assert(strstr(sink.last_payload, "\"status\":500") != NULL);

    /* 7. Verify all official log level strings acceptably set */
    const char *official_levels[] = {
        "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"
    };
    for (size_t i = 0; i < sizeof(official_levels) / sizeof(official_levels[0]); i++) {
        mcp_json_value_t *p = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, p, "level", mcp_json_string_new(ctx, official_levels[i]));
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "lvl-loop-%zu", i);
        mcp_message_t *r = mcp_request_new_string_id(ctx, id_buf, "logging/setLevel", p);
        mcp_message_t *rep = NULL;
        assert(mcp_server_dispatch(ctx, srv, sess, r, &rep) == MCP_OK);
        assert(rep != NULL && mcp_message_result(ctx, rep) != NULL);
        mcp_message_destroy(ctx, r);
        mcp_message_destroy(ctx, rep);
    }


    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_item4_server_logging_notifications\n");
}

/* =========================================================================
 * Item 5: Client standard methods and notification dispatching
 * ========================================================================= */
typedef struct {
    char sent[4096];
    size_t sent_len;
    char all_sent[16384];
    size_t all_sent_len;
    const char **script;
    size_t nscript;
    size_t cursor;
    int nsent;
} fake_client_transport_t;

static mcp_status_t fake_t_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t fake_t_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data, size_t len) {
    (void)ctx;
    fake_client_transport_t *f = (fake_client_transport_t *)mcp_transport_backend(ctx, t);
    assert(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->sent_len = len;
    if (f->all_sent_len + len < sizeof(f->all_sent) - 1) {
        memcpy(f->all_sent + f->all_sent_len, data, len);
        f->all_sent_len += len;
        f->all_sent[f->all_sent_len] = '\0';
    }
    f->nsent++;
    return MCP_OK;
}

static mcp_status_t fake_t_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    fake_client_transport_t *f = (fake_client_transport_t *)mcp_transport_backend(ctx, t);
    assert(f->cursor < f->nscript);
    const char *line = f->script[f->cursor++];
    size_t n = strlen(line) + 1;
    const mcp_allocator_t *alloc = ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
    char *buf = alloc->malloc_fn(n, alloc->userdata);
    assert(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t fake_t_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static const mcp_transport_ops_t kFakeClientOps = {
    .start = fake_t_start,
    .send  = fake_t_send,
    .recv  = fake_t_recv,
    .stop  = fake_t_stop,
    .sendv = NULL,
};

typedef struct {
    int call_count;
    char last_method[64];
    char last_level[32];
} test_client_notif_sink_t;

static bool on_client_notification_cb(mcp_client_t *client,
                                      const char *method,
                                      const mcp_json_value_t *params,
                                      void *user_data) {
    (void)client;
    test_client_notif_sink_t *sink = (test_client_notif_sink_t *)user_data;
    if (sink == NULL) return true;
    sink->call_count++;
    if (method != NULL) {
        snprintf(sink->last_method, sizeof(sink->last_method), "%s", method);
    }
    if (params != NULL) {
        const mcp_json_value_t *lv = mcp_json_object_get(NULL, params, "level");
        if (lv != NULL) {
            const char *lvs = NULL;
            if (mcp_json_string_value(NULL, lv, &lvs) == MCP_OK && lvs != NULL) {
                snprintf(sink->last_level, sizeof(sink->last_level), "%s", lvs);
            }
        }
    }
    return true;
}


static void test_item5_client_apis_and_notifications(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    fake_client_transport_t fake;
    memset(&fake, 0, sizeof(fake));

    const char *script[] = {
        /* 1. resources/list response */
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"resources\":[]}}",
        /* 2. resources/templates/list response */
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"resourceTemplates\":[]}}",
        /* 3. resources/subscribe response */
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}",
        /* 4. resources/unsubscribe response */
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}",
        /* 5. prompts/list response */
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"prompts\":[]}}",
        /* 6. logging/setLevel response */
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{}}",
        /* 7. notification interleaved before response */
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}",
        /* 8. notification received directly via recv_message */
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{\"level\":\"info\",\"data\":\"test\"}}"
    };
    fake.script = script;
    fake.nscript = sizeof(script) / sizeof(script[0]);

    mcp_transport_t *t = mcp_transport_create(ctx, &kFakeClientOps, &fake);
    assert(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    assert(c != NULL);

    test_client_notif_sink_t notif_sink = {0};
    mcp_client_set_notification_handler(ctx, c, on_client_notification_cb, &notif_sink);

    /* 1. Test resources/list */
    mcp_json_value_t *res1 = NULL;
    assert(mcp_client_list_resources(ctx, c, "c1", &res1) == MCP_OK);
    assert(res1 != NULL);
    assert(strstr(fake.sent, "\"method\":\"resources/list\"") != NULL);
    assert(strstr(fake.sent, "\"cursor\":\"c1\"") != NULL);
    mcp_json_destroy(ctx, res1);

    /* 2. Test resources/templates/list */
    mcp_json_value_t *res2 = NULL;
    assert(mcp_client_list_resource_templates(ctx, c, NULL, &res2) == MCP_OK);
    assert(res2 != NULL);
    assert(strstr(fake.sent, "\"method\":\"resources/templates/list\"") != NULL);
    mcp_json_destroy(ctx, res2);

    /* 3. Test resources/subscribe */
    mcp_json_value_t *res3 = NULL;
    assert(mcp_client_subscribe_resource(ctx, c, "res://test", &res3) == MCP_OK);
    assert(res3 != NULL);
    assert(strstr(fake.sent, "\"method\":\"resources/subscribe\"") != NULL);
    assert(strstr(fake.sent, "\"uri\":\"res://test\"") != NULL);
    mcp_json_destroy(ctx, res3);

    /* 4. Test resources/unsubscribe */
    mcp_json_value_t *res4 = NULL;
    assert(mcp_client_unsubscribe_resource(ctx, c, "res://test", &res4) == MCP_OK);
    assert(res4 != NULL);
    assert(strstr(fake.sent, "\"method\":\"resources/unsubscribe\"") != NULL);
    assert(strstr(fake.sent, "\"uri\":\"res://test\"") != NULL);
    mcp_json_destroy(ctx, res4);

    /* 5. Test prompts/list */
    mcp_json_value_t *res5 = NULL;
    assert(mcp_client_list_prompts(ctx, c, "p_cur", &res5) == MCP_OK);
    assert(res5 != NULL);
    assert(strstr(fake.sent, "\"method\":\"prompts/list\"") != NULL);
    assert(strstr(fake.sent, "\"cursor\":\"p_cur\"") != NULL);
    mcp_json_destroy(ctx, res5);

    /* 6. Test logging/setLevel */
    mcp_json_value_t *res6 = NULL;
    assert(mcp_client_set_logging_level(ctx, c, "warning", &res6) == MCP_OK);
    assert(res6 != NULL);
    assert(strstr(fake.sent, "\"method\":\"logging/setLevel\"") != NULL);
    assert(strstr(fake.sent, "\"level\":\"warning\"") != NULL);
    mcp_json_destroy(ctx, res6);

    /* 7. Interleaved notification dispatch during roundtrip */
    assert(notif_sink.call_count == 0);
    assert(mcp_client_ping(ctx, c) == MCP_OK);
    assert(notif_sink.call_count == 1);
    assert(strcmp(notif_sink.last_method, "notifications/tools/list_changed") == 0);

    /* 8. Notification dispatch during recv_message */
    mcp_message_t *msg_in = NULL;
    assert(mcp_client_recv_message(ctx, c, &msg_in) == MCP_OK);
    assert(msg_in != NULL);
    assert(notif_sink.call_count == 2);
    assert(strcmp(notif_sink.last_method, "notifications/message") == 0);
    assert(strcmp(notif_sink.last_level, "info") == 0);
    mcp_message_destroy(ctx, msg_in);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    printf("PASS: test_item5_client_apis_and_notifications\n");
}

static void test_item6_multimedia_content(void) {
    mcp_context_t *ctx = NULL;

    /* 1. Null handling */
    assert(mcp_content_text_new(ctx, NULL) == NULL);
    assert(mcp_content_image_new(ctx, NULL, "image/png") == NULL);
    assert(mcp_content_image_new(ctx, "abc", NULL) == NULL);
    assert(mcp_content_audio_new(ctx, NULL, "audio/wav") == NULL);
    assert(mcp_content_audio_new(ctx, "abc", NULL) == NULL);
    assert(mcp_content_audio_new_annotated(ctx, NULL, "audio/wav", NULL) == NULL);
    assert(mcp_content_audio_new_annotated(ctx, "abc", NULL, NULL) == NULL);
    assert(mcp_tool_result_image_new(ctx, NULL, "image/png") == NULL);
    assert(mcp_tool_result_image_new(ctx, "abc", NULL) == NULL);
    assert(mcp_tool_result_audio_new(ctx, NULL, "audio/wav") == NULL);
    assert(mcp_tool_result_audio_new(ctx, "abc", NULL) == NULL);

    /* 2. Unannotated convenience builders */
    mcp_json_value_t *txt = mcp_content_text_new(ctx, "sample text");
    assert(txt != NULL);
    const char *s_val = NULL;
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, txt, "type"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "text") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, txt, "text"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "sample text") == 0);
    assert(mcp_json_object_get(ctx, txt, "annotations") == NULL);
    mcp_json_destroy(ctx, txt);

    mcp_json_value_t *img = mcp_content_image_new(ctx, "iVBORw0KGgo=", "image/png");
    assert(img != NULL);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, img, "type"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "image") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, img, "data"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "iVBORw0KGgo=") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, img, "mimeType"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "image/png") == 0);
    assert(mcp_json_object_get(ctx, img, "annotations") == NULL);
    mcp_json_destroy(ctx, img);

    /* 3. AudioContent unannotated and annotated */
    mcp_json_value_t *aud = mcp_content_audio_new(ctx, "UklGRg==", "audio/wav");
    assert(aud != NULL);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, aud, "type"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "audio") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, aud, "data"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "UklGRg==") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, aud, "mimeType"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "audio/wav") == 0);
    assert(mcp_json_object_get(ctx, aud, "annotations") == NULL);
    mcp_json_destroy(ctx, aud);

    mcp_content_annotations_t ann;
    memset(&ann, 0, sizeof(ann));
    ann.audience = MCP_AUDIENCE_USER;
    ann.priority = 0.95;
    strncpy(ann.last_modified, "2026-11-23T12:00:00Z", sizeof(ann.last_modified) - 1);

    mcp_json_value_t *aud_ann = mcp_content_audio_new_annotated(ctx, "UklGRg==", "audio/wav", &ann);
    assert(aud_ann != NULL);
    mcp_content_annotations_t extracted;
    memset(&extracted, 0, sizeof(extracted));
    assert(mcp_content_extract_annotations(ctx, aud_ann, &extracted) == MCP_OK);
    assert(extracted.audience == MCP_AUDIENCE_USER);
    assert(extracted.priority >= 0.949 && extracted.priority <= 0.951);
    assert(strcmp(extracted.last_modified, "2026-11-23T12:00:00Z") == 0);
    mcp_json_destroy(ctx, aud_ann);

    /* 4. Tool result with ImageContent */
    mcp_json_value_t *img_res = mcp_tool_result_image_new(ctx, "iVBORw0KGgo=", "image/png");
    assert(img_res != NULL);
    const mcp_json_value_t *is_err = mcp_json_object_get(ctx, img_res, "isError");
    bool b_err = true;
    assert(is_err != NULL && mcp_json_bool_value(ctx, is_err, &b_err) == MCP_OK && b_err == false);
    const mcp_json_value_t *c_arr = mcp_json_object_get(ctx, img_res, "content");
    assert(c_arr != NULL && mcp_json_array_size(ctx, c_arr) == 1);
    const mcp_json_value_t *item0 = mcp_json_array_get(ctx, c_arr, 0);
    assert(item0 != NULL);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, item0, "type"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "image") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, item0, "data"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "iVBORw0KGgo=") == 0);
    mcp_json_destroy(ctx, img_res);

    /* 5. Tool result with AudioContent */
    mcp_json_value_t *aud_res = mcp_tool_result_audio_new(ctx, "UklGRg==", "audio/wav");
    assert(aud_res != NULL);
    is_err = mcp_json_object_get(ctx, aud_res, "isError");
    b_err = true;
    assert(is_err != NULL && mcp_json_bool_value(ctx, is_err, &b_err) == MCP_OK && b_err == false);
    c_arr = mcp_json_object_get(ctx, aud_res, "content");
    assert(c_arr != NULL && mcp_json_array_size(ctx, c_arr) == 1);
    item0 = mcp_json_array_get(ctx, c_arr, 0);
    assert(item0 != NULL);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, item0, "type"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "audio") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, item0, "data"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "UklGRg==") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, item0, "mimeType"), &s_val) == MCP_OK);
    assert(strcmp(s_val, "audio/wav") == 0);
    mcp_json_destroy(ctx, aud_res);

    (void)b_err;
    (void)s_val;
    printf("PASS: test_item6_multimedia_content\n");
}

static mcp_status_t dummy_tool_no_schema(mcp_context_t *ctx, mcp_session_t *session,
                                        const mcp_json_value_t *args, void *user_data,
                                        mcp_json_value_t **result_out) {
    (void)session; (void)args; (void)user_data;
    *result_out = mcp_tool_result_text_new(ctx, "ok");
    return MCP_OK;
}

static void test_p2_item1_tool_default_input_schema(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-srv", "1.0.0");
    assert(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    /* Tool created with NULL input_schema */
    mcp_tool_t *t = mcp_tool_new(ctx, "no_schema_tool", "desc", NULL, dummy_tool_no_schema, NULL);
    assert(t != NULL);
    assert(mcp_server_add_tool(ctx, srv, t) == MCP_OK);

    /* Initialize session */
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "c", "1.0");
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "i1", "initialize", ip);
    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);

    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* Dispatch tools/list */
    mcp_message_t *treq = mcp_request_new_string_id(ctx, "t1", "tools/list", NULL);
    mcp_message_t *tresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, treq, &tresp) == MCP_OK);
    assert(tresp != NULL);

    const mcp_json_value_t *res = mcp_message_result(ctx, tresp);
    assert(res != NULL);
    const mcp_json_value_t *tools_arr = mcp_json_object_get(ctx, res, "tools");
    assert(tools_arr != NULL && mcp_json_array_size(ctx, tools_arr) == 1);
    const mcp_json_value_t *tool0 = mcp_json_array_get(ctx, tools_arr, 0);
    assert(tool0 != NULL);

    /* inputSchema MUST be present per official MCP spec */
    const mcp_json_value_t *in_schema = mcp_json_object_get(ctx, tool0, "inputSchema");
    assert(in_schema != NULL && mcp_json_type(ctx, in_schema) == MCP_JSON_OBJECT);
    const char *stype = NULL;
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, in_schema, "type"), &stype) == MCP_OK);
    assert(strcmp(stype, "object") == 0);

    mcp_message_destroy(ctx, treq);
    mcp_message_destroy(ctx, tresp);
    mcp_server_destroy(ctx, srv);

    /* Also verify sampling tool addition with NULL input_schema */
    mcp_json_value_t *sparams = mcp_sampling_params_new(ctx, 100);
    assert(sparams != NULL);
    mcp_sampling_tool_def_t sdef = { "sample_tool", "desc", NULL };
    assert(mcp_sampling_params_add_tool(ctx, sparams, &sdef) == MCP_OK);
    const mcp_json_value_t *st0 = mcp_sampling_params_get_tool_at(ctx, sparams, 0);
    assert(st0 != NULL);
    const mcp_json_value_t *sin_schema = mcp_json_object_get(ctx, st0, "inputSchema");
    assert(sin_schema != NULL && mcp_json_type(ctx, sin_schema) == MCP_JSON_OBJECT);
    stype = NULL;
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, sin_schema, "type"), &stype) == MCP_OK);
    assert(strcmp(stype, "object") == 0);
    mcp_json_destroy(ctx, sparams);

    mcp_context_destroy(ctx);
    printf("PASS: test_p2_item1_tool_default_input_schema\n");
}

static void test_p2_item2_prompt_arguments(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-prompt-srv", "1.0.0");
    assert(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    /* 1. Register a prompt with arguments */
    mcp_prompt_t *p = mcp_prompt_new(ctx, "search_prompt", "Searches things", dummy_prompt_get, NULL);
    assert(p != NULL);
    assert(mcp_prompt_add_argument(ctx, NULL, "q", "query", true) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_prompt_add_argument(ctx, p, NULL, "query", true) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_prompt_add_argument(ctx, p, "q", "query", true) == MCP_OK);
    assert(mcp_prompt_add_argument(ctx, p, "limit", "max results", false) == MCP_OK);
    assert(mcp_server_add_prompt(ctx, srv, p) == MCP_OK);

    /* Initialize session */
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "c", "1.0");
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "i2", "initialize", ip);
    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);

    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* Dispatch prompts/list */
    mcp_message_t *preq = mcp_request_new_string_id(ctx, "pl1", "prompts/list", NULL);
    mcp_message_t *presp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, preq, &presp) == MCP_OK);
    assert(presp != NULL);

    const mcp_json_value_t *res = mcp_message_result(ctx, presp);
    assert(res != NULL);
    const mcp_json_value_t *prompts_arr = mcp_json_object_get(ctx, res, "prompts");
    assert(prompts_arr != NULL && mcp_json_array_size(ctx, prompts_arr) == 1);
    const mcp_json_value_t *prompt0 = mcp_json_array_get(ctx, prompts_arr, 0);
    assert(prompt0 != NULL);

    /* Verify arguments array */
    const mcp_json_value_t *args_arr = mcp_json_object_get(ctx, prompt0, "arguments");
    assert(args_arr != NULL && mcp_json_array_size(ctx, args_arr) == 2);

    const mcp_json_value_t *arg0 = mcp_json_array_get(ctx, args_arr, 0);
    assert(arg0 != NULL);
    const char *str = NULL;
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, arg0, "name"), &str) == MCP_OK);
    assert(strcmp(str, "q") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, arg0, "description"), &str) == MCP_OK);
    assert(strcmp(str, "query") == 0);
    bool req_val = false;
    assert(mcp_json_bool_value(ctx, mcp_json_object_get(ctx, arg0, "required"), &req_val) == MCP_OK);
    assert(req_val == true);

    const mcp_json_value_t *arg1 = mcp_json_array_get(ctx, args_arr, 1);
    assert(arg1 != NULL);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, arg1, "name"), &str) == MCP_OK);
    assert(strcmp(str, "limit") == 0);
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, arg1, "description"), &str) == MCP_OK);
    assert(strcmp(str, "max results") == 0);

    mcp_message_destroy(ctx, preq);
    mcp_message_destroy(ctx, presp);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);

    (void)str;
    (void)req_val;
    printf("PASS: test_p2_item2_prompt_arguments\n");
}

static mcp_json_value_t *dummy_roots_fn(mcp_context_t *ctx, void *user_data) {
    (void)user_data;
    mcp_json_value_t *res = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, res, "roots", mcp_json_array_new(ctx));
    return res;
}

static void test_p2_item3_instructions_and_client_capabilities(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    /* 1. Server instructions and session capabilities storage */
    mcp_server_t *srv = mcp_server_create(ctx, "instr-srv", "1.0.0");
    assert(srv != NULL);
    assert(mcp_server_set_instructions(srv, "Guidance for LLM") == MCP_OK);
    assert(strcmp(mcp_server_get_instructions(srv), "Guidance for LLM") == 0);

    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    /* Client sends initialize with capabilities */
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "test-client", "1.0.0");
    mcp_json_value_t *caps = (mcp_json_value_t *)mcp_json_object_get(ctx, ip, "capabilities");
    mcp_json_value_t *rc = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, rc, "listChanged", mcp_json_bool_new(ctx, true));
    mcp_json_object_set_take(ctx, caps, "roots", rc);
    mcp_json_object_set_take(ctx, caps, "sampling", mcp_json_object_new(ctx));

    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "i-caps", "initialize", ip);
    mcp_message_t *iresp = NULL;
    assert(mcp_server_dispatch(ctx, srv, sess, ireq, &iresp) == MCP_OK);
    assert(iresp != NULL);

    /* Verify response contains instructions */
    const mcp_json_value_t *ires = mcp_message_result(ctx, iresp);
    assert(ires != NULL);
    const mcp_json_value_t *inst_v = mcp_json_object_get(ctx, ires, "instructions");
    assert(inst_v != NULL);
    const char *instr_str = NULL;
    assert(mcp_json_string_value(ctx, inst_v, &instr_str) == MCP_OK);
    assert(strcmp(instr_str, "Guidance for LLM") == 0);

    /* Verify server session stored client capabilities */
    assert(mcp_session_has_client_capability(sess, "roots") == true);
    assert(mcp_session_has_client_capability(sess, "sampling") == true);
    assert(mcp_session_has_client_capability(sess, "experimental") == false);
    assert(mcp_session_client_capabilities(sess) != NULL);

    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);
    mcp_server_destroy(ctx, srv);

    /* 2. Client-side automatic capabilities and instructions retrieval */
    fake_client_transport_t fake;
    memset(&fake, 0, sizeof(fake));
    const char *script[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\",\"serverInfo\":{\"name\":\"s\",\"version\":\"1.0\"},\"instructions\":\"LLM instructions from server\"}}"
    };
    fake.script = script;
    fake.nscript = 1;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFakeClientOps, &fake);
    assert(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    assert(c != NULL);

    /* Register roots provider on client -> should auto-advertise roots capability */
    mcp_client_set_roots_provider(ctx, c, dummy_roots_fn, NULL);

    /* Set custom capabilities */
    mcp_json_value_t *custom_caps = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, custom_caps, "custom_mod", mcp_json_object_new(ctx));
    assert(mcp_client_set_capabilities(ctx, c, custom_caps) == MCP_OK);
    mcp_json_destroy(ctx, custom_caps);

    assert(mcp_client_initialize(ctx, c, "my-client", "1.0", NULL) == MCP_OK);

    /* Check that client sent roots and custom_mod capabilities in request */
    assert(strstr(fake.all_sent, "\"roots\":{\"listChanged\":true}") != NULL);
    assert(strstr(fake.all_sent, "\"custom_mod\":{}") != NULL);

    /* Check that client extracted instructions from server */
    assert(mcp_client_get_server_instructions(c) != NULL);
    assert(strcmp(mcp_client_get_server_instructions(c), "LLM instructions from server") == 0);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);

    (void)instr_str;
    printf("PASS: test_p2_item3_instructions_and_client_capabilities\n");
}

/* =========================================================================
 * Phase 2 - Item 4: Bidirectional Ping Symmetry
 * ========================================================================= */
typedef struct {
    bool invoked;
    bool had_error;
} ping_test_cb_ctx_t;

static void test_ping_response_cb(mcp_context_t *ctx,
                                  mcp_server_t *server,
                                  mcp_session_t *session,
                                  uint64_t request_id,
                                  const mcp_json_value_t *result,
                                  const mcp_json_value_t *error,
                                  void *user_data) {
    (void)ctx;
    (void)server;
    (void)session;
    (void)request_id;
    (void)result;
    ping_test_cb_ctx_t *tc = (ping_test_cb_ctx_t *)user_data;
    tc->invoked = true;
    tc->had_error = (error != NULL);
}

static void test_p2_item4_bidirectional_ping(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    /* 1. Client handling incoming server ping */
    fake_client_transport_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFakeClientOps, &fake);
    assert(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    assert(c != NULL);

    mcp_message_t *srv_ping_req = mcp_request_new_string_id(ctx, "ping-req-42", "ping", NULL);
    assert(srv_ping_req != NULL);

    mcp_message_t *client_resp = NULL;
    assert(mcp_client_handle_server_request(ctx, c, srv_ping_req, &client_resp) == MCP_OK);
    assert(client_resp != NULL);
    assert(mcp_message_kind(ctx, client_resp) == MCP_MSG_RESPONSE);
    assert(mcp_message_error(ctx, client_resp) == NULL);

    const mcp_json_value_t *ping_res = mcp_message_result(ctx, client_resp);
    assert(ping_res != NULL);
    assert(mcp_json_type(ctx, ping_res) == MCP_JSON_OBJECT);

    mcp_message_destroy(ctx, srv_ping_req);
    mcp_message_destroy(ctx, client_resp);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);

    /* 2. Server sending ping to client via mcp_server_ping */
    mcp_server_t *srv = mcp_server_create(ctx, "test-ping-srv", "1.0.0");
    assert(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    mock_sink_t sink = {0};
    mcp_session_set_sink(sess, mock_session_sink, &sink);

    ping_test_cb_ctx_t cb_ctx = {0};
    uint64_t ping_req_id = 0;
    assert(mcp_server_ping(ctx, srv, sess, 5000, test_ping_response_cb, &cb_ctx, &ping_req_id) == MCP_OK);

    assert(sink.call_count == 1);
    assert(strstr(sink.last_payload, "\"method\":\"ping\"") != NULL);

    /* Now simulate client responding with {} */
    mcp_json_value_t *empty_res = mcp_json_object_new(ctx);
    mcp_message_t *dummy_req = mcp_request_new_number_id(ctx, ping_req_id, "ping", NULL);
    mcp_message_t *mock_resp = mcp_response_ok_new(ctx, dummy_req, empty_res);
    assert(mock_resp != NULL);

    assert(mcp_server_dispatch_response(ctx, srv, sess, mock_resp) == MCP_OK);
    assert(cb_ctx.invoked == true);
    assert(cb_ctx.had_error == false);

    mcp_message_destroy(ctx, dummy_req);
    mcp_message_destroy(ctx, mock_resp);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);

    printf("PASS: test_p2_item4_bidirectional_ping\n");
}

/* =========================================================================
 * Phase 2 - Item 5: Server Roots Changed Notification Callback
 * ========================================================================= */
typedef struct {
    bool invoked;
    mcp_session_t *session;
} roots_changed_test_ctx_t;

static void test_roots_changed_cb(mcp_context_t *ctx,
                                  mcp_server_t *server,
                                  mcp_session_t *session,
                                  void *user_data) {
    (void)ctx;
    (void)server;
    roots_changed_test_ctx_t *tc = (roots_changed_test_ctx_t *)user_data;
    tc->invoked = true;
    tc->session = session;
}

static void test_p2_item5_roots_changed_handler(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "test-roots-srv", "1.0.0");
    assert(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    roots_changed_test_ctx_t tc = {0};
    assert(mcp_server_set_roots_changed_handler(srv, test_roots_changed_cb, &tc) == MCP_OK);

    /* Send notifications/initialized first */
    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    assert(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* Send notifications/roots/list_changed */
    mcp_message_t *roots_notif = mcp_notification_new(ctx, "notifications/roots/list_changed", NULL);
    assert(roots_notif != NULL);
    assert(mcp_server_notify(ctx, srv, sess, roots_notif) == MCP_OK);
    mcp_message_destroy(ctx, roots_notif);

    assert(tc.invoked == true);
    assert(tc.session == sess);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);

    printf("PASS: test_p2_item5_roots_changed_handler\n");
}

int main(void) {
    printf("Starting Official MCP Spec Compliance Tests...\n");
    test_item1_capabilities_and_resource_notifications();
    test_item2_completion_schema_alignment();
    test_item3_cancellation_mechanism();
    test_item4_server_logging_notifications();
    test_item5_client_apis_and_notifications();
    test_item6_multimedia_content();
    test_p2_item1_tool_default_input_schema();
    test_p2_item2_prompt_arguments();
    test_p2_item3_instructions_and_client_capabilities();
    test_p2_item4_bidirectional_ping();
    test_p2_item5_roots_changed_handler();
    printf("All Official MCP Spec Compliance Tests Passed!\n");
    return 0;
}
