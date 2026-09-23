/**
 * @file test_mrtr.c
 *
 * Unit tests for the Multi Round-Trip Requests (MRTR) implementation:
 *
 *   Part 1 — Protocol helpers (mrtr.h / mrtr.c):
 *      - mcp_mrtr_elicit_request_new
 *      - mcp_mrtr_result_input_required_new
 *      - mcp_mrtr_is_input_required
 *      - mcp_mrtr_get_request_state
 *      - mcp_mrtr_get_input_requests
 *      - mcp_mrtr_input_response_new
 *
 *   Part 2 — Server-side dispatcher:
 *      - V1 tools still receive resultType=complete decoration
 *      - V2 tool first call → InputRequiredResult forwarded as-is
 *      - V2 tool retry (with inputResponses + requestState) → complete result
 *
 *   Part 3 — Client-side MRTR loop (pump transport, single-threaded):
 *      - mcp_client_call_tool_mrtr completes after one elicitation
 *      - User-cancel returns MCP_ERR_CANCELLED
 */

#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/mrtr.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/server/tool.h"

/* -------------------------------------------------------------------------
 * Part 1: Protocol helper tests
 * ---------------------------------------------------------------------- */

static void test_elicit_request_new(void) {
    printf("  test_elicit_request_new\n");

    mcp_json_value_t *req = mcp_mrtr_elicit_request_new(NULL, "Enter your PIN", "default", NULL);
    CHECK(req != NULL);
    if (req == NULL) return;

    const mcp_json_value_t *msg = mcp_json_object_get(NULL, req, "message");
    CHECK(msg != NULL);
    CHECK(mcp_json_type(NULL, msg) == MCP_JSON_STRING);
    const char *msgs = NULL;
    CHECK(mcp_json_string_value(NULL, msg, &msgs) == MCP_OK);
    CHECK(msgs != NULL && strcmp(msgs, "Enter your PIN") == 0);

    const mcp_json_value_t *mode = mcp_json_object_get(NULL, req, "mode");
    CHECK(mode != NULL);
    const char *modes = NULL;
    CHECK(mcp_json_string_value(NULL, mode, &modes) == MCP_OK);
    CHECK(modes != NULL && strcmp(modes, "default") == 0);

    /* requestedSchema should be absent when NULL was passed */
    CHECK(mcp_json_object_get(NULL, req, "requestedSchema") == NULL);

    mcp_json_destroy(NULL, req);
}

static void test_elicit_request_with_schema(void) {
    printf("  test_elicit_request_with_schema\n");

    mcp_json_value_t *schema = mcp_json_object_new(NULL);
    CHECK(schema != NULL);

    mcp_json_value_t *req =
        mcp_mrtr_elicit_request_new(NULL, "Confirm action", NULL, schema);
    /* schema is taken by the call */

    CHECK(req != NULL);
    if (req == NULL) return;

    /* mode field should be absent */
    CHECK(mcp_json_object_get(NULL, req, "mode") == NULL);

    const mcp_json_value_t *s = mcp_json_object_get(NULL, req, "requestedSchema");
    CHECK(s != NULL);

    mcp_json_destroy(NULL, req);
}

static void test_input_required_new(void) {
    printf("  test_input_required_new\n");

    mcp_json_value_t *reqs = mcp_json_array_new(NULL);
    CHECK(reqs != NULL);

    mcp_json_value_t *item = mcp_mrtr_elicit_request_new(NULL, "Q1", NULL, NULL);
    CHECK(item != NULL);
    CHECK(mcp_json_array_append(NULL, reqs, item) == MCP_OK);

    mcp_json_value_t *result =
        mcp_mrtr_result_input_required_new(NULL, reqs, "state_token_abc");
    /* reqs taken */

    CHECK(result != NULL);
    if (result == NULL) return;

    /* Verify resultType */
    CHECK(mcp_mrtr_is_input_required(NULL, result));

    /* Verify requestState */
    const char *rs = mcp_mrtr_get_request_state(NULL, result);
    CHECK(rs != NULL && strcmp(rs, "state_token_abc") == 0);

    /* Verify inputRequests */
    const mcp_json_value_t *arr = mcp_mrtr_get_input_requests(NULL, result);
    CHECK(arr != NULL);
    CHECK(mcp_json_array_size(NULL, arr) == 1);

    mcp_json_destroy(NULL, result);
}

static void test_input_required_no_state(void) {
    printf("  test_input_required_no_state\n");

    mcp_json_value_t *reqs = mcp_json_array_new(NULL);
    CHECK(reqs != NULL);

    mcp_json_value_t *result =
        mcp_mrtr_result_input_required_new(NULL, reqs, NULL);
    CHECK(result != NULL);
    if (result == NULL) return;

    CHECK(mcp_mrtr_is_input_required(NULL, result));
    CHECK(mcp_mrtr_get_request_state(NULL, result) == NULL);

    mcp_json_destroy(NULL, result);
}

static void test_is_input_required_negative(void) {
    printf("  test_is_input_required_negative\n");

    /* NULL */
    CHECK(!mcp_mrtr_is_input_required(NULL, NULL));

    /* Object with resultType=complete */
    mcp_json_value_t *obj = mcp_json_object_new(NULL);
    CHECK(obj != NULL);
    mcp_json_value_t *sv = mcp_json_string_new(NULL, "complete");
    CHECK(sv != NULL);
    CHECK(mcp_json_object_set_take(NULL, obj, "resultType", sv) == MCP_OK);
    CHECK(!mcp_mrtr_is_input_required(NULL, obj));
    mcp_json_destroy(NULL, obj);

    /* Plain string (not an object) */
    mcp_json_value_t *s = mcp_json_string_new(NULL, "input_required");
    CHECK(s != NULL);
    CHECK(!mcp_mrtr_is_input_required(NULL, s));
    mcp_json_destroy(NULL, s);
}

static void test_input_response_new(void) {
    printf("  test_input_response_new\n");

    /* Accept with data */
    mcp_json_value_t *data = mcp_json_object_new(NULL);
    CHECK(data != NULL);
    mcp_json_value_t *pin = mcp_json_string_new(NULL, "1234");
    CHECK(pin != NULL);
    CHECK(mcp_json_object_set_take(NULL, data, "pin", pin) == MCP_OK);

    mcp_json_value_t *resp = mcp_mrtr_input_response_new(NULL, MCP_ELICIT_ACCEPT, data);
    /* data taken */
    CHECK(resp != NULL);
    if (resp == NULL) return;

    const mcp_json_value_t *action = mcp_json_object_get(NULL, resp, "action");
    CHECK(action != NULL);
    const char *as = NULL;
    CHECK(mcp_json_string_value(NULL, action, &as) == MCP_OK);
    CHECK(as != NULL && strcmp(as, "accept") == 0);
    CHECK(mcp_json_object_get(NULL, resp, "content") != NULL);

    mcp_json_destroy(NULL, resp);

    /* Reject (no data) */
    mcp_json_value_t *reject = mcp_mrtr_input_response_new(NULL, MCP_ELICIT_REJECT, NULL);
    CHECK(reject != NULL);
    const mcp_json_value_t *ra = mcp_json_object_get(NULL, reject, "action");
    CHECK(ra != NULL);
    const char *ras = NULL;
    CHECK(mcp_json_string_value(NULL, ra, &ras) == MCP_OK);
    CHECK(ras != NULL && strcmp(ras, "reject") == 0);
    CHECK(mcp_json_object_get(NULL, reject, "content") == NULL);
    mcp_json_destroy(NULL, reject);

    /* Cancel */
    mcp_json_value_t *cancel = mcp_mrtr_input_response_new(NULL, MCP_ELICIT_CANCEL, NULL);
    CHECK(cancel != NULL);
    const mcp_json_value_t *ca = mcp_json_object_get(NULL, cancel, "action");
    CHECK(ca != NULL);
    const char *cas = NULL;
    CHECK(mcp_json_string_value(NULL, ca, &cas) == MCP_OK);
    CHECK(cas != NULL && strcmp(cas, "cancel") == 0);
    mcp_json_destroy(NULL, cancel);
}

/* -------------------------------------------------------------------------
 * Part 2: Server dispatcher — V1 and V2 dispatch
 * ---------------------------------------------------------------------- */

/* V1 handler: returns a simple object */
static mcp_status_t v1_handler(mcp_context_t *ctx, mcp_session_t *session,
                                const mcp_json_value_t *args, void *user_data,
                                mcp_json_value_t **result_out) {
    (void)session; (void)args; (void)user_data;
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    mcp_json_value_t *sv  = obj ? mcp_json_string_new(ctx, "done") : NULL;
    if (obj == NULL || sv == NULL) { mcp_json_destroy(ctx, obj); return MCP_ERR_NOMEM; }
    if (mcp_json_object_set_take(ctx, obj, "value", sv) != MCP_OK) {
        mcp_json_destroy(ctx, obj); return MCP_ERR_NOMEM;
    }
    *result_out = obj;
    return MCP_OK;
}

/* V2 handler state machine: no requestState → InputRequired; has requestState → complete */
static mcp_status_t v2_handler(mcp_context_t *ctx, mcp_session_t *session,
                                const mcp_tool_call_ctx_t *call_ctx, void *user_data,
                                mcp_json_value_t **result_out) {
    (void)session; (void)user_data;

    if (call_ctx->request_state == NULL) {
        /* First call: request user input */
        mcp_json_value_t *reqs = mcp_json_array_new(ctx);
        if (reqs == NULL) return MCP_ERR_NOMEM;
        mcp_json_value_t *item =
            mcp_mrtr_elicit_request_new(ctx, "Enter confirmation", "default", NULL);
        if (item == NULL) { mcp_json_destroy(ctx, reqs); return MCP_ERR_NOMEM; }
        if (mcp_json_array_append(ctx, reqs, item) != MCP_OK) {
            mcp_json_destroy(ctx, item);
            mcp_json_destroy(ctx, reqs);
            return MCP_ERR_NOMEM;
        }
        mcp_json_value_t *ir = mcp_mrtr_result_input_required_new(ctx, reqs, "state_v2");
        if (ir == NULL) return MCP_ERR_NOMEM;
        *result_out = ir;
        return MCP_OK;
    }

    /* Retry call: produce final result */
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    mcp_json_value_t *sv  = obj ? mcp_json_string_new(ctx, "confirmed") : NULL;
    if (obj == NULL || sv == NULL) { mcp_json_destroy(ctx, obj); return MCP_ERR_NOMEM; }
    if (mcp_json_object_set_take(ctx, obj, "status", sv) != MCP_OK) {
        mcp_json_destroy(ctx, obj); return MCP_ERR_NOMEM;
    }
    *result_out = obj;
    return MCP_OK;
}

/* Build a synthetic request, dispatch with an initialized session, return response */
static mcp_message_t *do_dispatch(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *method, mcp_json_value_t *params) {
    static int id_counter = 0;
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "req%d", ++id_counter);
    mcp_message_t *req = mcp_request_new_string_id(ctx, id_buf, method, params);
    CHECK(req != NULL);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    return resp;
}

static mcp_session_t *make_initialized_session(mcp_context_t *ctx, mcp_server_t *srv) {
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    if (s == NULL) return NULL;
    /* Dispatch initialize */
    mcp_json_value_t *iparams = mcp_initialize_params_new(ctx, "test-client", "1.0");
    if (iparams == NULL) { mcp_server_destroy_session(ctx, srv, s); return NULL; }
    mcp_message_t *resp = do_dispatch(ctx, srv, s, "initialize", iparams);
    if (resp != NULL) mcp_message_destroy(ctx, resp);
    /* Send initialized notification */
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    if (notif != NULL) {
        mcp_server_notify(ctx, srv, s, notif);
        mcp_message_destroy(ctx, notif);
    }
    return s;
}

static void test_v1_tool_dispatch(void) {
    printf("  test_v1_tool_dispatch\n");

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0");
    CHECK(srv != NULL);

    mcp_tool_t *tool = mcp_tool_new(ctx, "v1tool", "A V1 tool", NULL, v1_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    mcp_session_t *s = make_initialized_session(ctx, srv);
    CHECK(s != NULL);

    /* Build tools/call params */
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    CHECK(params != NULL);
    mcp_json_value_t *nv = mcp_json_string_new(ctx, "v1tool");
    CHECK(nv != NULL);
    CHECK(mcp_json_object_set_take(ctx, params, "name", nv) == MCP_OK);

    mcp_message_t *resp = do_dispatch(ctx, srv, s, "tools/call", params);
    CHECK(resp != NULL);
    if (resp != NULL) {
        /* Should have resultType=complete */
        const mcp_json_value_t *result = mcp_message_result(ctx, resp);
        CHECK(result != NULL);
        const mcp_json_value_t *rt = mcp_json_object_get(ctx, result, "resultType");
        CHECK(rt != NULL);
        const char *rts = NULL;
        CHECK(mcp_json_string_value(ctx, rt, &rts) == MCP_OK);
        CHECK(rts != NULL && strcmp(rts, "complete") == 0);
        mcp_message_destroy(ctx, resp);
    }

    mcp_server_destroy_session(ctx, srv, s);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void test_v2_tool_first_call(void) {
    printf("  test_v2_tool_first_call\n");

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0");
    CHECK(srv != NULL);

    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "v2tool", "A V2 tool", NULL, v2_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    mcp_session_t *s = make_initialized_session(ctx, srv);
    CHECK(s != NULL);

    mcp_json_value_t *params = mcp_json_object_new(ctx);
    CHECK(params != NULL);
    mcp_json_value_t *nv = mcp_json_string_new(ctx, "v2tool");
    CHECK(nv != NULL);
    CHECK(mcp_json_object_set_take(ctx, params, "name", nv) == MCP_OK);

    /* First call: should get InputRequiredResult */
    mcp_message_t *resp = do_dispatch(ctx, srv, s, "tools/call", params);
    CHECK(resp != NULL);
    if (resp != NULL) {
        const mcp_json_value_t *result = mcp_message_result(ctx, resp);
        CHECK(result != NULL);
        CHECK(mcp_mrtr_is_input_required(ctx, result));
        const char *rs = mcp_mrtr_get_request_state(ctx, result);
        CHECK(rs != NULL && strcmp(rs, "state_v2") == 0);
        mcp_message_destroy(ctx, resp);
    }

    mcp_server_destroy_session(ctx, srv, s);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void test_v2_tool_retry(void) {
    printf("  test_v2_tool_retry\n");

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0");
    CHECK(srv != NULL);

    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "v2tool", "A V2 tool", NULL, v2_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    mcp_session_t *s = make_initialized_session(ctx, srv);
    CHECK(s != NULL);

    /* Retry call: supply requestState and inputResponses */
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    CHECK(params != NULL);
    mcp_json_value_t *nv = mcp_json_string_new(ctx, "v2tool");
    mcp_json_value_t *rs = mcp_json_string_new(ctx, "state_v2");
    mcp_json_value_t *irsp_arr = mcp_json_array_new(ctx);
    mcp_json_value_t *irsp_item = mcp_json_object_new(ctx);
    mcp_json_value_t *act = mcp_json_string_new(ctx, "accept");
    if (params == NULL || nv == NULL || rs == NULL || irsp_arr == NULL ||
        irsp_item == NULL || act == NULL) {
        printf("    SKIP: OOM in test setup\n");
        mcp_json_destroy(ctx, params);
        mcp_json_destroy(ctx, nv);
        mcp_json_destroy(ctx, rs);
        mcp_json_destroy(ctx, irsp_arr);
        mcp_json_destroy(ctx, irsp_item);
        mcp_json_destroy(ctx, act);
        mcp_server_destroy_session(ctx, srv, s);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
        return;
    }
    CHECK(mcp_json_object_set_take(ctx, irsp_item, "action", act) == MCP_OK);
    CHECK(mcp_json_array_append(ctx, irsp_arr, irsp_item) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, params, "name", nv) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, params, "requestState", rs) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, params, "inputResponses", irsp_arr) == MCP_OK);

    mcp_message_t *resp = do_dispatch(ctx, srv, s, "tools/call", params);
    CHECK(resp != NULL);
    if (resp != NULL) {
        const mcp_json_value_t *result = mcp_message_result(ctx, resp);
        CHECK(result != NULL);
        /* Should be complete now */
        CHECK(!mcp_mrtr_is_input_required(ctx, result));
        const mcp_json_value_t *rt = mcp_json_object_get(ctx, result, "resultType");
        CHECK(rt != NULL);
        const char *rts = NULL;
        CHECK(mcp_json_string_value(ctx, rt, &rts) == MCP_OK);
        CHECK(rts != NULL && strcmp(rts, "complete") == 0);
        const mcp_json_value_t *status = mcp_json_object_get(ctx, result, "status");
        CHECK(status != NULL);
        const char *sts = NULL;
        CHECK(mcp_json_string_value(ctx, status, &sts) == MCP_OK);
        CHECK(sts != NULL && strcmp(sts, "confirmed") == 0);
        mcp_message_destroy(ctx, resp);
    }

    mcp_server_destroy_session(ctx, srv, s);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

/* -------------------------------------------------------------------------
 * Part 3: Client MRTR loop — pump (single-threaded) transport
 * ---------------------------------------------------------------------- */

/* pump_t: single-threaded server-in-send pattern from test_client_e2e.c */
typedef struct {
    FILE *c2s;
    FILE *s2c;
    long cpos;
    long rpos;
    mcp_server_t *srv;
    mcp_session_t *sess;
    mcp_context_t *ctx;
} pump_t;

static void server_step(pump_t *p) {
    mcp_context_t *ctx = p->ctx;
    CHECK(fseek(p->c2s, p->cpos, SEEK_SET) == 0);
    static char buf[16384];
    if (fgets(buf, (int)sizeof(buf), p->c2s) == NULL) return;
    p->cpos = ftell(p->c2s);
    mcp_message_t *msg = mcp_message_parse(ctx, buf, strlen(buf));
    if (msg == NULL) return;
    if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
        mcp_server_notify(ctx, p->srv, p->sess, msg);
        mcp_message_destroy(ctx, msg);
        return;
    }
    mcp_message_t *resp = NULL;
    mcp_server_dispatch(ctx, p->srv, p->sess, msg, &resp);
    mcp_message_destroy(ctx, msg);
    if (resp == NULL) return;
    char *out = mcp_message_serialize(ctx, resp);
    mcp_message_destroy(ctx, resp);
    if (out == NULL) return;
    CHECK(fseek(p->s2c, 0, SEEK_END) == 0);
    CHECK(fputs(out, p->s2c) != EOF && fputc('\n', p->s2c) != EOF);
    CHECK(fflush(p->s2c) == 0);
    mcp_json_free_string(ctx, out);
}

static mcp_status_t pump_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t;
    return MCP_OK;
}

static mcp_status_t pump_send(mcp_context_t *ctx, mcp_transport_t *t,
                               const char *data, size_t len) {
    pump_t *p = (pump_t *)mcp_transport_backend(ctx, t);
    CHECK(fseek(p->c2s, 0, SEEK_END) == 0);
    CHECK(fwrite(data, 1, len, p->c2s) == len);
    CHECK(fputc('\n', p->c2s) != EOF);
    CHECK(fflush(p->c2s) == 0);
    server_step(p);
    return MCP_OK;
}

static mcp_status_t pump_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    pump_t *p = (pump_t *)mcp_transport_backend(ctx, t);
    CHECK(fseek(p->s2c, p->rpos, SEEK_SET) == 0);
    static char buf[16384];
    if (fgets(buf, (int)sizeof(buf), p->s2c) == NULL) {
        *line_out = NULL;
        return MCP_ERR_IO;
    }
    p->rpos = ftell(p->s2c);
    size_t n = strlen(buf) + 1;
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    char *line = (char *)a->malloc_fn(n, a->userdata);
    if (line == NULL) { *line_out = NULL; return MCP_ERR_NOMEM; }
    memcpy(line, buf, n);
    *line_out = line;
    return MCP_OK;
}

static mcp_status_t pump_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t;
    return MCP_OK;
}

static const mcp_transport_ops_t kPump = { pump_start, pump_send, pump_recv, pump_stop };

/* Elicitation callback: accepts the first request with a fixed payload */
static int g_elicit_calls = 0;

static mcp_json_value_t *test_elicit(mcp_context_t *ctx,
                                      const mcp_json_value_t *input_requests,
                                      const char *request_state,
                                      void *user_data) {
    (void)user_data;
    g_elicit_calls++;

    CHECK(request_state != NULL && strcmp(request_state, "state_v2") == 0);
    CHECK(input_requests != NULL && mcp_json_array_size(ctx, input_requests) >= 1);

    /* Build accept response */
    mcp_json_value_t *responses = mcp_json_array_new(ctx);
    if (responses == NULL) return NULL;

    mcp_json_value_t *data = mcp_json_object_new(ctx);
    if (data == NULL) { mcp_json_destroy(ctx, responses); return NULL; }

    mcp_json_value_t *pv = mcp_json_string_new(ctx, "9999");
    if (pv == NULL) { mcp_json_destroy(ctx, data); mcp_json_destroy(ctx, responses); return NULL; }
    if (mcp_json_object_set_take(ctx, data, "pin", pv) != MCP_OK) {
        mcp_json_destroy(ctx, data); mcp_json_destroy(ctx, responses); return NULL;
    }

    mcp_json_value_t *resp_item = mcp_mrtr_input_response_new(ctx, MCP_ELICIT_ACCEPT, data);
    if (resp_item == NULL) { mcp_json_destroy(ctx, responses); return NULL; }

    if (mcp_json_array_append(ctx, responses, resp_item) != MCP_OK) {
        mcp_json_destroy(ctx, resp_item);
        mcp_json_destroy(ctx, responses);
        return NULL;
    }
    return responses;
}

static mcp_json_value_t *test_elicit_cancel(mcp_context_t *ctx,
                                              const mcp_json_value_t *input_requests,
                                              const char *request_state,
                                              void *user_data) {
    (void)ctx; (void)input_requests; (void)request_state; (void)user_data;
    return NULL; /* signals cancel */
}

static void setup_pump(pump_t *p, mcp_context_t *ctx, mcp_server_t *srv) {
    memset(p, 0, sizeof(*p));
    p->c2s = tmpfile();
    p->s2c = tmpfile();
    p->srv = srv;
    p->sess = mcp_server_create_session(ctx, srv);
    p->ctx = ctx;
}

static void teardown_pump(pump_t *p, mcp_context_t *ctx, mcp_server_t *srv) {
    if (p->c2s) fclose(p->c2s);
    if (p->s2c) fclose(p->s2c);
    if (p->sess) mcp_server_destroy_session(ctx, srv, p->sess);
}

static void test_mrtr_client_loop(void) {
    printf("  test_mrtr_client_loop\n");

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0");
    CHECK(srv != NULL);
    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "v2tool", NULL, NULL, v2_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    pump_t pump;
    setup_pump(&pump, ctx, srv);
    CHECK(pump.c2s != NULL && pump.s2c != NULL && pump.sess != NULL);

    mcp_transport_t *t = mcp_transport_create(ctx, &kPump, &pump);
    CHECK(t != NULL);
    mcp_client_t *client = mcp_client_create(ctx, t);
    CHECK(client != NULL);
    CHECK(mcp_client_connect(ctx, client) == MCP_OK);

    mcp_status_t st = mcp_client_initialize(ctx, client, "test-client", "1.0", NULL);
    CHECK(st == MCP_OK);

    g_elicit_calls = 0;
    mcp_client_set_mrtr_elicit_handler(ctx, client, test_elicit, NULL);

    mcp_json_value_t *result = NULL;
    st = mcp_client_call_tool_mrtr(ctx, client, "v2tool", NULL, &result);
    CHECK(st == MCP_OK);
    CHECK(result != NULL);
    CHECK(!mcp_mrtr_is_input_required(ctx, result));
    CHECK(g_elicit_calls == 1);

    if (result != NULL) {
        const mcp_json_value_t *status = mcp_json_object_get(ctx, result, "status");
        CHECK(status != NULL);
        const char *sts = NULL;
        CHECK(mcp_json_string_value(ctx, status, &sts) == MCP_OK);
        CHECK(sts != NULL && strcmp(sts, "confirmed") == 0);
        mcp_json_destroy(ctx, result);
    }

    CHECK(mcp_client_disconnect(ctx, client) == MCP_OK);
    mcp_client_destroy(ctx, client);
    mcp_transport_destroy(ctx, t);
    teardown_pump(&pump, ctx, srv);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void test_mrtr_client_cancel(void) {
    printf("  test_mrtr_client_cancel\n");

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0");
    CHECK(srv != NULL);
    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "v2tool", NULL, NULL, v2_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    pump_t pump;
    setup_pump(&pump, ctx, srv);
    CHECK(pump.c2s != NULL && pump.s2c != NULL && pump.sess != NULL);

    mcp_transport_t *t = mcp_transport_create(ctx, &kPump, &pump);
    CHECK(t != NULL);
    mcp_client_t *client = mcp_client_create(ctx, t);
    CHECK(client != NULL);
    CHECK(mcp_client_connect(ctx, client) == MCP_OK);

    mcp_status_t st = mcp_client_initialize(ctx, client, "test-client", "1.0", NULL);
    CHECK(st == MCP_OK);

    mcp_client_set_mrtr_elicit_handler(ctx, client, test_elicit_cancel, NULL);

    mcp_json_value_t *result = NULL;
    st = mcp_client_call_tool_mrtr(ctx, client, "v2tool", NULL, &result);
    CHECK(st == MCP_ERR_CANCELLED);
    CHECK(result == NULL);

    CHECK(mcp_client_disconnect(ctx, client) == MCP_OK);
    mcp_client_destroy(ctx, client);
    mcp_transport_destroy(ctx, t);
    teardown_pump(&pump, ctx, srv);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void test_mrtr_no_handler_passthrough(void) {
    printf("  test_mrtr_no_handler_passthrough\n");

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0");
    CHECK(srv != NULL);
    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "v2tool", NULL, NULL, v2_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    pump_t pump;
    setup_pump(&pump, ctx, srv);
    CHECK(pump.c2s != NULL && pump.s2c != NULL && pump.sess != NULL);

    mcp_transport_t *t = mcp_transport_create(ctx, &kPump, &pump);
    CHECK(t != NULL);
    mcp_client_t *client = mcp_client_create(ctx, t);
    CHECK(client != NULL);
    CHECK(mcp_client_connect(ctx, client) == MCP_OK);

    mcp_status_t st = mcp_client_initialize(ctx, client, "test-client", "1.0", NULL);
    CHECK(st == MCP_OK);

    /* No elicitation handler: InputRequiredResult should be returned as-is */
    mcp_json_value_t *result = NULL;
    st = mcp_client_call_tool_mrtr(ctx, client, "v2tool", NULL, &result);
    CHECK(st == MCP_OK);
    CHECK(result != NULL);
    CHECK(mcp_mrtr_is_input_required(ctx, result));  /* raw InputRequiredResult */
    mcp_json_destroy(ctx, result);

    CHECK(mcp_client_disconnect(ctx, client) == MCP_OK);
    mcp_client_destroy(ctx, client);
    mcp_transport_destroy(ctx, t);
    teardown_pump(&pump, ctx, srv);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== test_mrtr: Part 1 — Protocol helpers ===\n");
    test_elicit_request_new();
    test_elicit_request_with_schema();
    test_input_required_new();
    test_input_required_no_state();
    test_is_input_required_negative();
    test_input_response_new();

    printf("=== test_mrtr: Part 2 — Dispatcher ===\n");
    test_v1_tool_dispatch();
    test_v2_tool_first_call();
    test_v2_tool_retry();

    printf("=== test_mrtr: Part 3 — Client MRTR loop ===\n");
    test_mrtr_client_loop();
    test_mrtr_client_cancel();
    test_mrtr_no_handler_passthrough();

    printf("test_mrtr OK\n");
    return 0;
}
