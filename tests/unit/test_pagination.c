#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t null_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    *o = mcp_json_object_new(c);
    return *o != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

typedef struct {
    char sent[4096];
    const char **script;
    size_t nscript;
    size_t cursor;
} page_fake_t;

static mcp_status_t pf_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t pf_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                            size_t len) {
    (void)ctx;
    page_fake_t *f = (page_fake_t *)mcp_transport_backend(ctx, t);
    CHECK(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    return MCP_OK;
}

static mcp_status_t pf_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    page_fake_t *f = (page_fake_t *)mcp_transport_backend(ctx, t);
    CHECK(f->cursor < f->nscript);
    const char *line = f->script[f->cursor++];
    size_t n = strlen(line) + 1;
    char *buf = mcp_context_allocator(ctx)->malloc_fn(n, mcp_context_allocator(ctx)->userdata);
    CHECK(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t pf_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static const mcp_transport_ops_t kPageFake = { pf_start, pf_send, pf_recv, pf_stop };

static mcp_message_t *list_req(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                               const char *id, const char *cursor) {
    mcp_json_value_t *params = NULL;
    if (cursor != NULL) {
        params = mcp_json_object_new(ctx);
        CHECK(params != NULL);
        mcp_json_value_t *cv = mcp_json_string_new(ctx, cursor);
        CHECK(cv != NULL);
        CHECK(mcp_json_object_set_take(ctx, params, "cursor", cv) == MCP_OK);
    }
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, "tools/list", params);
    CHECK(req != NULL);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp != NULL);
    return resp;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    CHECK(srv != NULL);
    char name[16];
    for (int i = 0; i < 250; i++) {
        snprintf(name, sizeof(name), "t%03d", i);
        char *copy = mcp_context_allocator(ctx)->malloc_fn(strlen(name) + 1,
                                                            mcp_context_allocator(ctx)->userdata);
        CHECK(copy != NULL);
        memcpy(copy, name, strlen(name) + 1);
        mcp_tool_t *tool = mcp_tool_new(ctx, copy, NULL, NULL, null_handler, NULL);
        mcp_context_allocator(ctx)->free_fn(copy, mcp_context_allocator(ctx)->userdata);
        CHECK(tool != NULL);
        CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);
    }
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);

    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "cli", "1");
    CHECK(ip != NULL);
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "init", "initialize", ip);
    CHECK(ireq != NULL);
    mcp_message_t *iresp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, ireq, &iresp) == MCP_OK);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);

    mcp_message_t *r = list_req(ctx, srv, s, "p0", NULL);
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res != NULL);
    CHECK(mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "tools")) == 100);
    const char *nc = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, res, "nextCursor"), &nc) == MCP_OK);
    CHECK(strcmp(nc, "100") == 0);
    mcp_message_destroy(ctx, r);

    r = list_req(ctx, srv, s, "p1", "100");
    res = mcp_message_result(ctx, r);
    CHECK(res != NULL);
    CHECK(mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "tools")) == 100);
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, res, "nextCursor"), &nc) == MCP_OK);
    CHECK(strcmp(nc, "200") == 0);
    const mcp_json_value_t *first =
        mcp_json_array_get(ctx, mcp_json_object_get(ctx, res, "tools"), 0);
    const char *fname = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, first, "name"), &fname) == MCP_OK);
    CHECK(strcmp(fname, "t100") == 0);
    mcp_message_destroy(ctx, r);

    r = list_req(ctx, srv, s, "p2", "200");
    res = mcp_message_result(ctx, r);
    CHECK(res != NULL);
    CHECK(mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "tools")) == 50);
    CHECK(mcp_json_object_get(ctx, res, "nextCursor") == NULL);
    mcp_message_destroy(ctx, r);

    r = list_req(ctx, srv, s, "p3", "abc");
    int code = 0;
    CHECK(mcp_message_error_code(ctx, r, &code) == MCP_OK && code == -32602);
    mcp_message_destroy(ctx, r);

    mcp_queue_t *q = mcp_queue_create(ctx);
    CHECK(q != NULL);
    for (size_t i = 0; i < MCP_QUEUE_MAX_LEN; i++) {
        mcp_message_t *m = mcp_request_new_number_id(ctx, (double)i, "ping", NULL);
        CHECK(m != NULL);
        CHECK(mcp_queue_push(ctx, q, s, m) == MCP_OK);
    }
    mcp_message_t *extra = mcp_request_new_number_id(ctx, 99999.0, "ping", NULL);
    CHECK(extra != NULL);
    CHECK(mcp_queue_push(ctx, q, s, extra) == MCP_ERR_NOMEM);
    mcp_message_destroy(ctx, extra);
    mcp_queue_destroy(ctx, q);

    page_fake_t pf;
    memset(&pf, 0, sizeof(pf));
    static const char *kPages[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[{\"name\":\"a\"}],\"nextCursor\":\"1\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"b\"}]}}",
    };
    pf.script = kPages;
    pf.nscript = 2;
    mcp_transport_t *t = mcp_transport_create(ctx, &kPageFake, &pf);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    mcp_json_value_t *tools = NULL;
    CHECK(mcp_client_list_tools(ctx, c, &tools) == MCP_OK && tools != NULL);
    const mcp_json_value_t *tarr = mcp_json_object_get(ctx, tools, "tools");
    CHECK(tarr != NULL && mcp_json_array_size(ctx, tarr) == 2);
    CHECK(strstr(pf.sent, "\"cursor\":\"1\"") != NULL);
    mcp_json_destroy(ctx, tools);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("OK\n");
    return 0;
}
