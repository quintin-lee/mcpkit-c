#include "test_check.h"
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/transport.h"

/* ── provider callbacks ─────────────────────────────────────────────────── */

static int g_roots_calls = 0;

static mcp_json_value_t *roots_provider(mcp_context_t *ctx, void *user_data) {
    (void)user_data;
    g_roots_calls++;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (arr == NULL) return NULL;
    mcp_json_value_t *root = mcp_json_object_new(ctx);
    if (root == NULL) { mcp_json_destroy(ctx, arr); return NULL; }
    mcp_json_value_t *uri = mcp_json_string_new(ctx, "file:///tmp");
    if (uri == NULL || mcp_json_object_set_take(ctx, root, "uri", uri) != MCP_OK) {
        if (uri != NULL) mcp_json_destroy(ctx, uri);
        mcp_json_destroy(ctx, root);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    if (mcp_json_array_append(ctx, arr, root) != MCP_OK) {
        mcp_json_destroy(ctx, root);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    return arr;
}

static int g_sample_calls = 0;

static mcp_json_value_t *sample_provider(mcp_context_t *ctx,
                                         const mcp_json_value_t *params,
                                         void *user_data) {
    (void)user_data; (void)params;
    g_sample_calls++;
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) return NULL;
    mcp_json_value_t *role = mcp_json_string_new(ctx, "assistant");
    if (role == NULL || mcp_json_object_set_take(ctx, result, "role", role) != MCP_OK) {
        if (role != NULL) mcp_json_destroy(ctx, role);
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_json_value_t *model = mcp_json_string_new(ctx, "test-model");
    if (model == NULL || mcp_json_object_set_take(ctx, result, "model", model) != MCP_OK) {
        if (model != NULL) mcp_json_destroy(ctx, model);
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    return result;
}

static int g_elicit_calls = 0;

static mcp_json_value_t *elicit_provider(mcp_context_t *ctx,
                                         const mcp_json_value_t *params,
                                         void *user_data) {
    (void)user_data; (void)params;
    g_elicit_calls++;
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) return NULL;
    return result;
}

/* ── helpers ────────────────────────────────────────────────────────────── */

static mcp_message_t *make_request(mcp_context_t *ctx, const char *method,
                                   const char *id) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, NULL);
    CHECK(req != NULL);
    return req;
}

/* ── tests ─────────────────────────────────────────────────────────────── */

static void test_null_guards(mcp_context_t *ctx, mcp_client_t *c) {
    mcp_message_t *resp;

    /* NULL client */
    CHECK(mcp_client_handle_server_request(ctx, NULL, NULL, &resp) ==
          MCP_ERR_INVALID_ARGUMENT);
    /* NULL req */
    CHECK(mcp_client_handle_server_request(ctx, c, NULL, &resp) ==
          MCP_ERR_INVALID_ARGUMENT);
}

static void test_unknown_method(mcp_context_t *ctx, mcp_client_t *c) {
    mcp_message_t *req = make_request(ctx, "unknown/method", "1");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    CHECK(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    int ecode = 0;
    CHECK(mcp_message_error_code(ctx, resp, &ecode) == MCP_OK);
    CHECK(ecode == -32601);
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

static void test_roots_no_provider(mcp_context_t *ctx, mcp_client_t *c) {
    /* No provider registered → -32601 */
    mcp_message_t *req = make_request(ctx, "roots/list", "10");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    CHECK(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

static void test_roots_with_provider(mcp_context_t *ctx, mcp_client_t *c) {
    g_roots_calls = 0;
    mcp_client_set_roots_provider(ctx, c, roots_provider, NULL);

    mcp_message_t *req = make_request(ctx, "roots/list", "11");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    CHECK(g_roots_calls == 1);

    /* Result should be an array with one root */
    const mcp_json_value_t *result = mcp_message_result(ctx, resp);
    CHECK(result != NULL);
    CHECK(mcp_json_type(ctx, result) == MCP_JSON_ARRAY);
    CHECK(mcp_json_array_size(ctx, result) == 1);

    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);

    /* Clear provider */
    mcp_client_set_roots_provider(ctx, c, NULL, NULL);
    g_roots_calls = 0;
    mcp_message_t *req2 = make_request(ctx, "roots/list", "12");
    mcp_message_t *resp2 = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req2, &resp2) == MCP_OK);
    CHECK(g_roots_calls == 0); /* provider not called */
    mcp_message_destroy(ctx, req2);
    mcp_message_destroy(ctx, resp2);
}

static void test_sample_no_provider(mcp_context_t *ctx, mcp_client_t *c) {
    mcp_message_t *req = make_request(ctx, "sampling/createMessage", "20");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

static void test_sample_with_provider(mcp_context_t *ctx, mcp_client_t *c) {
    g_sample_calls = 0;
    mcp_client_set_sample_provider(ctx, c, sample_provider, NULL);

    mcp_message_t *req = make_request(ctx, "sampling/createMessage", "21");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    CHECK(g_sample_calls == 1);

    const mcp_json_value_t *result = mcp_message_result(ctx, resp);
    CHECK(result != NULL);
    CHECK(mcp_json_type(ctx, result) == MCP_JSON_OBJECT);

    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

static void test_elicitation_no_provider(mcp_context_t *ctx, mcp_client_t *c) {
    mcp_message_t *req = make_request(ctx, "elicitation/create", "30");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

static void test_elicitation_with_provider(mcp_context_t *ctx, mcp_client_t *c) {
    g_elicit_calls = 0;
    mcp_client_set_elicitation_provider(ctx, c, elicit_provider, NULL);

    mcp_message_t *req = make_request(ctx, "elicitation/create", "31");
    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, c, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    CHECK(g_elicit_calls == 1);

    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

static void test_set_provider_null_client(void) {
    /* NULL client is a no-op, must not crash */
    mcp_client_set_roots_provider(NULL, NULL, roots_provider, NULL);
    mcp_client_set_sample_provider(NULL, NULL, sample_provider, NULL);
    mcp_client_set_elicitation_provider(NULL, NULL, elicit_provider, NULL);
}

/* Minimal no-op transport ops so mcp_client_create accepts a non-NULL transport. */
static mcp_status_t noop_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t; return MCP_OK;
}
static mcp_status_t noop_send(mcp_context_t *ctx, mcp_transport_t *t, const char *d,
                              size_t n) {
    (void)ctx; (void)t; (void)d; (void)n; return MCP_OK;
}
static mcp_status_t noop_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    (void)ctx; (void)t; *line_out = NULL; return MCP_ERR_IO;
}
static mcp_status_t noop_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t; return MCP_OK;
}
static const mcp_transport_ops_t kNoop = { noop_start, noop_send, noop_recv, noop_stop };

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_transport_t *t = mcp_transport_create(ctx, &kNoop, NULL);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);

    test_null_guards(ctx, c);
    test_unknown_method(ctx, c);
    test_roots_no_provider(ctx, c);
    test_roots_with_provider(ctx, c);
    test_sample_no_provider(ctx, c);
    test_sample_with_provider(ctx, c);
    test_elicitation_no_provider(ctx, c);
    test_elicitation_with_provider(ctx, c);
    test_set_provider_null_client();

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    return 0;
}
