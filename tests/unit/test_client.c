#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    char sent[4096];
    size_t sent_len;
    int started;
    int stopped;
    const char **script;
    size_t nscript;
    size_t cursor;
    int nsent;
} fake_t;

static mcp_status_t fake_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    ((fake_t *)mcp_transport_backend(ctx, t))->started++;
    return MCP_OK;
}

static mcp_status_t fake_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    (void)ctx;
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    CHECK(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->sent_len = len;
    f->nsent++;
    return MCP_OK;
}

static mcp_status_t fake_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    CHECK(f->cursor < f->nscript);
    const char *line = f->script[f->cursor++];
    size_t n = strlen(line) + 1;
    char *buf = mcp_context_allocator(ctx)->malloc_fn(n, mcp_context_allocator(ctx)->userdata);
    CHECK(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t fake_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    ((fake_t *)mcp_transport_backend(ctx, t))->stopped++;
    return MCP_OK;
}

static const mcp_transport_ops_t kFake = { fake_start, fake_send, fake_recv, fake_stop };

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    CHECK(mcp_client_create(ctx, NULL) == NULL);
    CHECK(mcp_client_request(ctx, NULL, "ping", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_protocol_version(ctx, c) == NULL);
    CHECK(mcp_client_request(ctx, c, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK && fake.started == 1);

    static const char *kOk[] = { "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":true}}" };
    fake.script = kOk;
    fake.nscript = 1;
    mcp_json_value_t *res = NULL;
    CHECK(mcp_client_request(ctx, c, "ping", NULL, &res) == MCP_OK);
    CHECK(res != NULL);
    bool ok = false;
    CHECK(mcp_json_bool_value(ctx, mcp_json_object_get(ctx, res, "ok"), &ok) == MCP_OK && ok);
    mcp_json_destroy(ctx, res);
    CHECK(strstr(fake.sent, "\"method\":\"ping\"") != NULL);
    CHECK(strstr(fake.sent, "\"id\":1") != NULL);

    static const char *kMismatch[] = { "{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":{}}" };
    fake.script = kMismatch;
    fake.nscript = 1;
    fake.cursor = 0;
    res = (mcp_json_value_t *)0x1;
    CHECK(mcp_client_request(ctx, c, "ping", NULL, &res) == MCP_ERR_PROTOCOL);
    CHECK(res == NULL);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK && fake.stopped == 1);
    mcp_client_destroy(ctx, c);

    CHECK(mcp_client_initialize(ctx, NULL, "n", "v", NULL) == MCP_ERR_INVALID_ARGUMENT);

    fake_t fake2;
    memset(&fake2, 0, sizeof(fake2));
    static const char *kShake[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\","
        "\"serverInfo\":{\"name\":\"s\",\"version\":\"1\"},\"capabilities\":{}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32602,\"message\":\"Unknown tool\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"error\":{\"code\":-32600}}",
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"tools\":[{\"name\":\"echo\"}]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"contents\":[]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"messages\":[]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"completions\":[]}}",
    };
    fake2.script = kShake;
    fake2.nscript = 9;
    mcp_transport_t *t2 = mcp_transport_create(ctx, &kFake, &fake2);
    CHECK(t2 != NULL);
    mcp_client_t *c2 = mcp_client_create(ctx, t2);
    CHECK(c2 != NULL);
    CHECK(mcp_client_protocol_version(ctx, c2) == NULL);
    mcp_json_value_t *info = NULL;
    CHECK(mcp_client_initialize(ctx, c2, "n", "v", &info) == MCP_OK);
    CHECK(fake2.nsent == 2);
    CHECK(strstr(fake2.sent, "\"method\":\"notifications/initialized\"") != NULL);
    CHECK(strstr(fake2.sent, "\"id\"") == NULL);
    const char *ver = mcp_client_protocol_version(ctx, c2);
    CHECK(ver != NULL && strcmp(ver, "2025-06-18") == 0);
    CHECK(info != NULL);
    const char *sname = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, info, "name"), &sname) == MCP_OK);
    CHECK(strcmp(sname, "s") == 0);
    mcp_json_destroy(ctx, info);
    CHECK(mcp_client_call_tool(ctx, c2, "nope", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_call_tool(ctx, c2, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_ping(ctx, c2) == MCP_OK);
    CHECK(mcp_client_request(ctx, c2, "ping", NULL, NULL) == MCP_ERR_PROTOCOL);
    mcp_json_value_t *tools = NULL;
    CHECK(mcp_client_list_tools(ctx, c2, &tools) == MCP_OK && tools != NULL);
    const mcp_json_value_t *tarr = mcp_json_object_get(ctx, tools, "tools");
    CHECK(mcp_json_array_size(ctx, tarr) == 1);
    const char *tname = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, mcp_json_array_get(ctx, tarr, 0),
                                                          "name"),
                                 &tname) == MCP_OK);
    CHECK(strcmp(tname, "echo") == 0);
    mcp_json_destroy(ctx, tools);
    CHECK(mcp_client_list_tools(ctx, c2, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *args = mcp_json_object_new(ctx);
    CHECK(args != NULL);
    CHECK(mcp_json_object_set(ctx, args, "text", mcp_json_string_new(ctx, "hi")) == MCP_OK);
    mcp_json_value_t *cres = NULL;
    CHECK(mcp_client_call_tool(ctx, c2, "echo", args, &cres) == MCP_OK && cres != NULL);
    const char *etext = NULL;
    CHECK(mcp_json_string_value(ctx,
                                 mcp_json_object_get(ctx,
                                                     mcp_json_array_get(ctx,
                                                                        mcp_json_object_get(ctx,
                                                                                            cres,
                                                                                            "content"),
                                                                        0),
                                                     "text"),
                                 &etext) == MCP_OK);
    CHECK(strcmp(etext, "hi") == 0);
    mcp_json_destroy(ctx, cres);
    CHECK(mcp_client_read_resource(ctx, c2, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *rres = NULL;
    CHECK(mcp_client_read_resource(ctx, c2, "test://doc", &rres) == MCP_OK && rres != NULL);
    mcp_json_destroy(ctx, rres);
    mcp_json_value_t *pres = NULL;
    CHECK(mcp_client_get_prompt(ctx, c2, "p", NULL, &pres) == MCP_OK && pres != NULL);
    mcp_json_destroy(ctx, pres);
    CHECK(mcp_client_get_prompt(ctx, c2, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    // completion/complete via mcp_client_complete
    CHECK(mcp_client_complete(ctx, c2, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *crow = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, crow, "text", mcp_json_string_new(ctx, "x")) == MCP_OK);
    mcp_json_value_t *cre = NULL;
    CHECK(mcp_client_complete(ctx, c2, "prompt/greet", crow, &cre) == MCP_OK && cre != NULL);
    const mcp_json_value_t *carr = mcp_json_object_get(ctx, cre, "completions");
    CHECK(carr != NULL && mcp_json_array_size(ctx, carr) == 0);
    mcp_json_destroy(ctx, cre);

    // Test subscriptions/listen, recv_message, and cancel_subscription
    CHECK(mcp_client_subscriptions_listen(ctx, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_cancel_subscription(ctx, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_recv_message(ctx, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    static const char *kSubAck[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/subscriptions/acknowledged\",\"params\":{\"notifications\":{\"toolsListChanged\":true},\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":\"sub-test-1\"}}}"
    };
    fake2.script = kSubAck;
    fake2.nscript = 1;
    fake2.cursor = 0;
    mcp_json_value_t *sub_filter = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, sub_filter, "toolsListChanged", mcp_json_bool_new(ctx, true)) == MCP_OK);
    mcp_message_t *ack_msg = NULL;
    CHECK(mcp_client_subscriptions_listen(ctx, c2, sub_filter, &ack_msg) == MCP_OK);
    CHECK(ack_msg != NULL);
    CHECK(mcp_message_kind(ctx, ack_msg) == MCP_MSG_NOTIFICATION);
    CHECK(strcmp(mcp_message_method(ctx, ack_msg), "notifications/subscriptions/acknowledged") == 0);
    const mcp_json_value_t *ack_p = mcp_message_params(ctx, ack_msg);
    CHECK(ack_p != NULL);
    const mcp_json_value_t *meta = mcp_json_object_get(ctx, ack_p, "_meta");
    CHECK(meta != NULL);
    const mcp_json_value_t *sub_id_v = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId");
    CHECK(sub_id_v != NULL);
    const char *sub_id_str = NULL;
    CHECK(mcp_json_string_value(ctx, sub_id_v, &sub_id_str) == MCP_OK && strcmp(sub_id_str, "sub-test-1") == 0);
    mcp_message_destroy(ctx, ack_msg);
    CHECK(strstr(fake2.sent, "\"method\":\"subscriptions/listen\"") != NULL);

    // Test receiving notification on subscription stream
    static const char *kNotifStream[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":\"sub-test-1\"}}}"
    };
    fake2.script = kNotifStream;
    fake2.nscript = 1;
    fake2.cursor = 0;
    mcp_message_t *stream_msg = NULL;
    CHECK(mcp_client_recv_message(ctx, c2, &stream_msg) == MCP_OK);
    CHECK(stream_msg != NULL);
    CHECK(strcmp(mcp_message_method(ctx, stream_msg), "notifications/tools/list_changed") == 0);
    mcp_message_destroy(ctx, stream_msg);

    // Test cancelling subscription
    CHECK(mcp_client_cancel_subscription(ctx, c2, "sub-test-1") == MCP_OK);
    CHECK(strstr(fake2.sent, "\"method\":\"notifications/cancelled\"") != NULL);
    CHECK(strstr(fake2.sent, "sub-test-1") != NULL);

    // Test Tasks & Skills client helper invalid arguments
    CHECK(mcp_client_tasks_get(ctx, NULL, "task-abc", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_tasks_get(ctx, c2, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_tasks_update(ctx, NULL, "task-abc", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_tasks_update(ctx, c2, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_tasks_cancel(ctx, NULL, "task-abc", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_tasks_cancel(ctx, c2, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_skills_list(ctx, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_skills_get(ctx, NULL, "skill", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_skills_get(ctx, c2, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    static const char *kTasksSkillsResponses[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{\"taskId\":\"task-abc\",\"status\":\"working\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"result\":{\"taskId\":\"task-abc\",\"status\":\"completed\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"result\":{\"taskId\":\"task-abc\",\"status\":\"cancelled\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"result\":{\"skills\":[{\"name\":\"code-search\",\"uri\":\"skills://code-search\",\"description\":\"search code\"}]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"result\":{\"name\":\"code-search\",\"uri\":\"skills://code-search\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":16,\"result\":{\"name\":\"code-search\",\"uri\":\"skills://code-search\"}}"
    };
    fake2.script = kTasksSkillsResponses;
    fake2.nscript = 6;
    fake2.cursor = 0;

    // 1. tasks/get
    mcp_json_value_t *tget_res = NULL;
    CHECK(mcp_client_tasks_get(ctx, c2, "task-abc", &tget_res) == MCP_OK);
    CHECK(tget_res != NULL);
    const char *task_id_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, tget_res, "taskId"), &task_id_val) == MCP_OK);
    CHECK(strcmp(task_id_val, "task-abc") == 0);
    mcp_json_destroy(ctx, tget_res);
    CHECK(strstr(fake2.sent, "\"method\":\"tasks/get\"") != NULL);
    CHECK(strstr(fake2.sent, "\"taskId\":\"task-abc\"") != NULL);

    // 2. tasks/update
    mcp_json_value_t *in_resp = mcp_json_array_new(ctx);
    CHECK(in_resp != NULL);
    mcp_json_value_t *tupd_res = NULL;
    CHECK(mcp_client_tasks_update(ctx, c2, "task-abc", in_resp, &tupd_res) == MCP_OK);
    CHECK(tupd_res != NULL);
    const char *status_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, tupd_res, "status"), &status_val) == MCP_OK);
    CHECK(strcmp(status_val, "completed") == 0);
    mcp_json_destroy(ctx, tupd_res);
    CHECK(strstr(fake2.sent, "\"method\":\"tasks/update\"") != NULL);
    CHECK(strstr(fake2.sent, "\"inputResponses\":[]") != NULL);

    // 3. tasks/cancel
    mcp_json_value_t *tcan_res = NULL;
    CHECK(mcp_client_tasks_cancel(ctx, c2, "task-abc", &tcan_res) == MCP_OK);
    CHECK(tcan_res != NULL);
    status_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, tcan_res, "status"), &status_val) == MCP_OK);
    CHECK(strcmp(status_val, "cancelled") == 0);
    mcp_json_destroy(ctx, tcan_res);
    CHECK(strstr(fake2.sent, "\"method\":\"tasks/cancel\"") != NULL);

    // 4. skills/list (with cursor)
    mcp_json_value_t *slist_res = NULL;
    CHECK(mcp_client_skills_list(ctx, c2, "cursor-1", &slist_res) == MCP_OK);
    CHECK(slist_res != NULL);
    const mcp_json_value_t *skills_arr = mcp_json_object_get(ctx, slist_res, "skills");
    CHECK(skills_arr != NULL && mcp_json_array_size(ctx, skills_arr) == 1);
    mcp_json_destroy(ctx, slist_res);
    CHECK(strstr(fake2.sent, "\"method\":\"skills/list\"") != NULL);
    CHECK(strstr(fake2.sent, "\"cursor\":\"cursor-1\"") != NULL);

    // 5. skills/get (by name)
    mcp_json_value_t *sget_res1 = NULL;
    CHECK(mcp_client_skills_get(ctx, c2, "code-search", &sget_res1) == MCP_OK);
    CHECK(sget_res1 != NULL);
    mcp_json_destroy(ctx, sget_res1);
    CHECK(strstr(fake2.sent, "\"method\":\"skills/get\"") != NULL);
    CHECK(strstr(fake2.sent, "\"name\":\"code-search\"") != NULL);

    // 6. skills/get (by URI)
    mcp_json_value_t *sget_res2 = NULL;
    CHECK(mcp_client_skills_get(ctx, c2, "skills://code-search", &sget_res2) == MCP_OK);
    CHECK(sget_res2 != NULL);
    mcp_json_destroy(ctx, sget_res2);
    CHECK(strstr(fake2.sent, "\"method\":\"skills/get\"") != NULL);
    CHECK(strstr(fake2.sent, "\"uri\":\"skills://code-search\"") != NULL);

    // 7. notifications/roots/list_changed
    CHECK(mcp_client_notify_roots_list_changed(ctx, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_notify_roots_list_changed(ctx, c2) == MCP_OK);
    CHECK(strstr(fake2.sent, "\"method\":\"notifications/roots/list_changed\"") != NULL);
    CHECK(strstr(fake2.sent, "\"id\":") == NULL);

    mcp_client_destroy(ctx, c2);
    mcp_transport_destroy(ctx, t2);
    mcp_client_destroy(ctx, NULL);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    printf("test_client OK\n");
    return 0;
}
