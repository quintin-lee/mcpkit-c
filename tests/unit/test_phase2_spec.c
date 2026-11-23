#define _DEFAULT_SOURCE

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    bool called;
    uint64_t req_id;
    mcp_json_value_t *result_clone;
    mcp_json_value_t *error_clone;
    int error_code;
    char *error_message;
    mcp_session_t *session;
} test_cb_state_t;

static void reset_cb_state(mcp_context_t *ctx, test_cb_state_t *st) {
    if (st->result_clone != NULL) {
        mcp_json_destroy(ctx, st->result_clone);
        st->result_clone = NULL;
    }
    if (st->error_clone != NULL) {
        mcp_json_destroy(ctx, st->error_clone);
        st->error_clone = NULL;
    }
    if (st->error_message != NULL) {
        free(st->error_message);
        st->error_message = NULL;
    }
    st->called = false;
    st->req_id = 0;
    st->error_code = 0;
    st->session = NULL;
}

static void on_test_response(mcp_context_t *ctx,
                             mcp_server_t *server,
                             mcp_session_t *session,
                             uint64_t req_id,
                             const mcp_json_value_t *result,
                             const mcp_json_value_t *error,
                             void *user_data) {
    (void)server;
    test_cb_state_t *st = (test_cb_state_t *)user_data;
    if (st == NULL) {
        return;
    }
    st->called = true;
    st->req_id = req_id;
    st->session = session;
    if (result != NULL) {
        st->result_clone = mcp_json_clone(ctx, result);
    }
    if (error != NULL) {
        st->error_clone = mcp_json_clone(ctx, error);
        const mcp_json_value_t *code_val = mcp_json_object_get(ctx, error, "code");
        if (code_val != NULL) {
            double cd = 0;
            if (mcp_json_number_value(ctx, code_val, &cd) == MCP_OK) {
                st->error_code = (int)cd;
            }
        }
        const mcp_json_value_t *msg_val = mcp_json_object_get(ctx, error, "message");
        if (msg_val != NULL) {
            const char *ms = NULL;
            if (mcp_json_string_value(ctx, msg_val, &ms) == MCP_OK && ms != NULL) {
                st->error_message = strdup(ms);
            }
        }
    }
}

static void test_server_request_sampling_success(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);

    test_cb_state_t state;
    memset(&state, 0, sizeof(state));

    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_object_set(ctx, params, "maxTokens", mcp_json_number_new(ctx, 256));

    uint64_t assigned_id = 0;
    mcp_status_t st = mcp_server_request_sampling(ctx, srv, NULL, params, 5000,
                                                   on_test_response, &state, &assigned_id);
    assert(st == MCP_OK);
    assert(assigned_id >= 1000);
    (void)st;

    /* Verify request is in server outbox */
    mcp_message_t *out_req = NULL;
    assert(mcp_server_outbox_pop(ctx, srv, &out_req) == MCP_OK);
    assert(out_req != NULL);
    assert(mcp_message_kind(ctx, out_req) == MCP_MSG_REQUEST);
    assert(strcmp(mcp_message_method(ctx, out_req), "sampling/createMessage") == 0);

    double req_num = 0;
    assert(mcp_message_id_number(ctx, out_req, &req_num) == MCP_OK);
    assert((uint64_t)req_num == assigned_id);
    (void)req_num;

    /* Construct client response */
    mcp_json_value_t *res_obj = mcp_json_object_new(ctx);
    mcp_json_object_set(ctx, res_obj, "role", mcp_json_string_new(ctx, "assistant"));
    mcp_message_t *resp = mcp_response_ok_new(ctx, out_req, res_obj);
    assert(resp != NULL);

    /* Dispatch response to server */
    st = mcp_server_dispatch_response(ctx, srv, NULL, resp);
    assert(st == MCP_OK);

    assert(state.called == true);
    assert(state.req_id == assigned_id);
    assert(state.result_clone != NULL);
    assert(state.error_clone == NULL);

    const mcp_json_value_t *role_val = mcp_json_object_get(ctx, state.result_clone, "role");
    const char *role_str = NULL;
    assert(mcp_json_string_value(ctx, role_val, &role_str) == MCP_OK);
    assert(strcmp(role_str, "assistant") == 0);
    (void)role_val;
    (void)role_str;

    /* Calling again with the same response should return NOT_FOUND (already consumed) */
    st = mcp_server_dispatch_response(ctx, srv, NULL, resp);
    assert(st == MCP_ERR_NOT_FOUND);

    mcp_message_destroy(ctx, resp);
    mcp_message_destroy(ctx, out_req);
    reset_cb_state(ctx, &state);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_request_sampling_success\n");
}

static void test_server_request_roots_error(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);

    test_cb_state_t state;
    memset(&state, 0, sizeof(state));

    uint64_t assigned_id = 0;
    mcp_status_t st = mcp_server_request_roots(ctx, srv, NULL, 5000,
                                               on_test_response, &state, &assigned_id);
    assert(st == MCP_OK);
    (void)st;

    mcp_message_t *out_req = NULL;
    assert(mcp_server_outbox_pop(ctx, srv, &out_req) == MCP_OK);
    assert(strcmp(mcp_message_method(ctx, out_req), "roots/list") == 0);

    /* Construct client error response: roots not supported (-32601) */
    mcp_message_t *resp = mcp_response_err_new(ctx, out_req, -32601, "roots not supported", NULL);
    assert(resp != NULL);

    st = mcp_server_dispatch_response(ctx, srv, NULL, resp);
    assert(st == MCP_OK);

    assert(state.called == true);
    assert(state.req_id == assigned_id);
    assert(state.result_clone == NULL);
    assert(state.error_clone != NULL);
    assert(state.error_code == -32601);
    assert(state.error_message != NULL);
    assert(strcmp(state.error_message, "roots not supported") == 0);

    mcp_message_destroy(ctx, resp);
    mcp_message_destroy(ctx, out_req);
    reset_cb_state(ctx, &state);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_request_roots_error\n");
}

static void test_server_dispatch_string_id_match(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);

    test_cb_state_t state;
    memset(&state, 0, sizeof(state));

    uint64_t assigned_id = 0;
    mcp_status_t st = mcp_server_request_elicitation(ctx, srv, NULL, NULL, 5000,
                                                     on_test_response, &state, &assigned_id);
    assert(st == MCP_OK);
    (void)st;

    mcp_message_t *out_req = NULL;
    assert(mcp_server_outbox_pop(ctx, srv, &out_req) == MCP_OK);

    /* Manually craft a JSON response where id is serialized as a string: {"jsonrpc":"2.0","id":"<assigned_id>","result":{"ok":true}} */
    char resp_json[256];
    snprintf(resp_json, sizeof(resp_json),
             "{\"jsonrpc\":\"2.0\",\"id\":\"%" PRIu64 "\",\"result\":{\"ok\":true}}", assigned_id);

    mcp_message_t *resp = mcp_message_parse(ctx, resp_json, strlen(resp_json));
    assert(resp != NULL);
    assert(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    assert(mcp_message_id_type(ctx, resp) == MCP_ID_STRING);

    st = mcp_server_dispatch_response(ctx, srv, NULL, resp);
    assert(st == MCP_OK);

    assert(state.called == true);
    assert(state.req_id == assigned_id);
    assert(state.result_clone != NULL);

    mcp_message_destroy(ctx, resp);
    mcp_message_destroy(ctx, out_req);
    reset_cb_state(ctx, &state);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_dispatch_string_id_match\n");
}

static void test_server_unmatched_response(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);

    const char *resp_json = "{\"jsonrpc\":\"2.0\",\"id\":99999,\"result\":{\"status\":\"ok\"}}";
    mcp_message_t *resp = mcp_message_parse(ctx, resp_json, strlen(resp_json));
    assert(resp != NULL);

    mcp_status_t st = mcp_server_dispatch_response(ctx, srv, NULL, resp);
    assert(st == MCP_ERR_NOT_FOUND);
    (void)st;

    mcp_message_destroy(ctx, resp);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_unmatched_response\n");
}

static void test_server_sweep_timeout(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);

    test_cb_state_t state;
    memset(&state, 0, sizeof(state));

    /* Register a request with a short 30ms timeout */
    uint64_t assigned_id = 0;
    mcp_status_t st = mcp_server_request_sampling(ctx, srv, NULL, NULL, 30,
                                                   on_test_response, &state, &assigned_id);
    assert(st == MCP_OK);
    (void)st;

    /* Immediate sweep before deadline should NOT expire anything */
    size_t swept = mcp_server_sweep_pending_requests(ctx, srv, 0);
    assert(swept == 0);
    assert(state.called == false);

    /* Sleep 60ms to let deadline expire */
    struct timespec sleep_time = {0, 60000000}; /* 60 ms */
    nanosleep(&sleep_time, NULL);

    swept = mcp_server_sweep_pending_requests(ctx, srv, 0);
    assert(swept == 1);
    assert(state.called == true);
    assert(state.req_id == assigned_id);
    assert(state.result_clone == NULL);
    assert(state.error_clone != NULL);
    assert(state.error_code == MCP_RPC_REQUEST_TIMEOUT);
    assert(state.error_message != NULL);
    assert(strcmp(state.error_message, "Request timed out") == 0);

    /* Second sweep returns 0 */
    swept = mcp_server_sweep_pending_requests(ctx, srv, 0);
    assert(swept == 0);
    (void)swept;

    /* Drain outbox to clean up */
    mcp_message_t *out_req = NULL;
    mcp_server_outbox_pop(ctx, srv, &out_req);
    if (out_req != NULL) {
        mcp_message_destroy(ctx, out_req);
    }

    reset_cb_state(ctx, &state);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_sweep_timeout\n");
}

static void test_server_dispatch_passthrough(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    test_cb_state_t state;
    memset(&state, 0, sizeof(state));

    uint64_t assigned_id = 0;
    mcp_status_t st = mcp_server_request_roots(ctx, srv, sess, 5000,
                                               on_test_response, &state, &assigned_id);
    assert(st == MCP_OK);
    (void)st;

    mcp_message_t *out_req = NULL;
    assert(mcp_server_outbox_pop(ctx, srv, &out_req) == MCP_OK);

    mcp_json_value_t *res_obj = mcp_json_object_new(ctx);
    mcp_json_object_set(ctx, res_obj, "status", mcp_json_string_new(ctx, "roots_ready"));
    mcp_message_t *resp = mcp_response_ok_new(ctx, out_req, res_obj);
    assert(resp != NULL);

    /* Pass response directly to mcp_server_dispatch */
    mcp_message_t *resp_out = (mcp_message_t *)0x1234;
    st = mcp_server_dispatch(ctx, srv, sess, resp, &resp_out);
    assert(st == MCP_OK);
    assert(resp_out == NULL);

    assert(state.called == true);
    assert(state.req_id == assigned_id);
    assert(state.result_clone != NULL);
    assert(state.session == sess);

    mcp_message_destroy(ctx, resp);
    mcp_message_destroy(ctx, out_req);
    reset_cb_state(ctx, &state);
    mcp_server_destroy_session(ctx, srv, sess);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_dispatch_passthrough\n");
}

static void test_session_free_clears_pending_ref(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess != NULL);

    test_cb_state_t state;
    memset(&state, 0, sizeof(state));

    uint64_t assigned_id = 0;
    mcp_status_t st = mcp_server_request_roots(ctx, srv, sess, 5000,
                                               on_test_response, &state, &assigned_id);
    assert(st == MCP_OK);
    (void)st;

    mcp_message_t *out_req = NULL;
    assert(mcp_server_outbox_pop(ctx, srv, &out_req) == MCP_OK);

    /* Destroy session BEFORE response arrives */
    mcp_server_destroy_session(ctx, srv, sess);

    /* Now response arrives without session */
    mcp_json_value_t *res_obj = mcp_json_object_new(ctx);
    mcp_message_t *resp = mcp_response_ok_new(ctx, out_req, res_obj);
    assert(resp != NULL);

    st = mcp_server_dispatch_response(ctx, srv, NULL, resp);
    assert(st == MCP_OK);
    assert(state.called == true);
    /* Verified that session was cleared to NULL, avoiding dangling reference */
    assert(state.session == NULL);

    mcp_message_destroy(ctx, resp);
    mcp_message_destroy(ctx, out_req);
    reset_cb_state(ctx, &state);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_session_free_clears_pending_ref\n");
}

static void test_server_destroy_with_pending(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test-server", "1.0.0");
    assert(srv != NULL);

    for (int i = 0; i < 10; i++) {
        uint64_t id = 0;
        mcp_status_t st = mcp_server_request_sampling(ctx, srv, NULL, NULL, 10000,
                                                       on_test_response, NULL, &id);
        assert(st == MCP_OK);
        (void)st;
    }

    /* Destroy server with 10 pending requests still in table */
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("PASS: test_server_destroy_with_pending\n");
}

int main(void) {
    printf("Starting Phase 2 spec compliance tests...\n");
    test_server_request_sampling_success();
    test_server_request_roots_error();
    test_server_dispatch_string_id_match();
    test_server_unmatched_response();
    test_server_sweep_timeout();
    test_server_dispatch_passthrough();
    test_session_free_clears_pending_ref();
    test_server_destroy_with_pending();
    printf("PASS: All Phase 2 spec compliance tests passed successfully!\n");
    return 0;
}
