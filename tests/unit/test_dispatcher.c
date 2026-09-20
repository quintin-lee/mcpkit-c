#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "test_check.h"

static mcp_status_t echo_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)s;
    (void)u;
    const mcp_json_value_t *t = mcp_json_object_get(c, a, "text");
    const char *str = NULL;
    if (t == NULL || mcp_json_string_value(c, t, &str) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *out = mcp_json_object_new(c);
    mcp_json_value_t *sv = mcp_json_string_new(c, str);
    if (out == NULL || sv == NULL || mcp_json_object_set(c, out, "text", sv) != MCP_OK) {
        mcp_json_destroy(c, sv);
        mcp_json_destroy(c, out);
        return MCP_ERR_NOMEM;
    }
    *o = out;
    return MCP_OK;
}

static mcp_status_t fail_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)c;
    (void)s;
    (void)a;
    (void)u;
    (void)o;
    return MCP_ERR_IO;
}

static mcp_status_t slow_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    volatile unsigned long spin = 0;
    for (unsigned long i = 0; i < 50000000ul; i++) {
        spin += i;
    }
    (void)spin;
    *o = mcp_json_object_new(c);
    return *o ? MCP_OK : MCP_ERR_NOMEM;
}

static mcp_status_t static_read(mcp_context_t *c, mcp_session_t *s, const char *uri, void *u,
                                mcp_json_value_t **o) {
    (void)s;
    (void)u;
    mcp_json_value_t *arr = mcp_json_array_new(c);
    mcp_json_value_t *item = mcp_json_object_new(c);
    mcp_json_value_t *uval = mcp_json_string_new(c, uri);
    mcp_json_value_t *tval = mcp_json_string_new(c, "hello");
    if (!arr || !item || !uval || !tval || mcp_json_object_set(c, item, "uri", uval) != MCP_OK ||
        mcp_json_object_set(c, item, "text", tval) != MCP_OK ||
        mcp_json_array_append(c, arr, item) != MCP_OK) {
        mcp_json_destroy(c, uval);
        mcp_json_destroy(c, tval);
        mcp_json_destroy(c, item);
        mcp_json_destroy(c, arr);
        return MCP_ERR_NOMEM;
    }
    *o = arr;
    return MCP_OK;
}

static mcp_status_t static_prompt(mcp_context_t *c, mcp_session_t *s,
                                  const mcp_json_value_t *args, void *u, mcp_json_value_t **o) {
    (void)s;
    (void)args;
    (void)u;
    mcp_json_value_t *arr = mcp_json_array_new(c);
    mcp_json_value_t *msg = mcp_json_object_new(c);
    mcp_json_value_t *role = mcp_json_string_new(c, "user");
    mcp_json_value_t *content = mcp_json_object_new(c);
    mcp_json_value_t *type = mcp_json_string_new(c, "text");
    mcp_json_value_t *text = mcp_json_string_new(c, "hi");
    if (!arr || !msg || !role || !content || !type || !text ||
        mcp_json_object_set(c, msg, "role", role) != MCP_OK ||
        mcp_json_object_set(c, content, "type", type) != MCP_OK ||
        mcp_json_object_set(c, content, "text", text) != MCP_OK ||
        mcp_json_object_set(c, msg, "content", content) != MCP_OK ||
        mcp_json_array_append(c, arr, msg) != MCP_OK) {
        mcp_json_destroy(c, role);
        mcp_json_destroy(c, type);
        mcp_json_destroy(c, text);
        mcp_json_destroy(c, content);
        mcp_json_destroy(c, msg);
        mcp_json_destroy(c, arr);
        return MCP_ERR_NOMEM;
    }
    *o = arr;
    return MCP_OK;
}

static mcp_json_value_t *text_schema(mcp_context_t *ctx) {
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_json_value_t *prop = mcp_schema_string_new(ctx);
    if (!schema || !prop || mcp_schema_add_property(ctx, schema, "text", prop) != MCP_OK ||
        mcp_schema_add_required(ctx, schema, "text") != MCP_OK) {
        mcp_json_destroy(ctx, prop);
        mcp_json_destroy(ctx, schema);
        return NULL;
    }
    return schema;
}

// Completion provider: returns an array of suggestion strings.
static mcp_json_value_t *compl_provider(mcp_context_t *ctx, mcp_session_t *s,
                                        const mcp_json_value_t *ref, void *ud) {
    (void)s;
    (void)ud;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (arr == NULL) {
        return NULL;
    }
    // ref is {"type":"ref","value":"prompt/greet"} — we return a couple of hints.
    const char *val = "hint";
    if (ref != NULL) {
        const mcp_json_value_t *v = mcp_json_object_get(ctx, ref, "value");
        if (v != NULL) {
            const char *s = NULL;
            if (mcp_json_string_value(ctx, v, &s) == MCP_OK && s != NULL) {
                val = s;
            }
        }
    }
    mcp_json_value_t *s1 = mcp_json_string_new(ctx, val);
    mcp_json_value_t *s2 = mcp_json_string_new(ctx, "other");
    if (s1 == NULL || s2 == NULL || mcp_json_array_append(ctx, arr, s1) != MCP_OK ||
        mcp_json_array_append(ctx, arr, s2) != MCP_OK) {
        mcp_json_destroy(ctx, s1);
        mcp_json_destroy(ctx, s2);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    return arr;
}

// Returns NULL (simulating provider failure).
static mcp_json_value_t *compl_fail(mcp_context_t *ctx, mcp_session_t *s,
                                    const mcp_json_value_t *ref, void *ud) {
    (void)ctx;
    (void)s;
    (void)ref;
    (void)ud;
    return NULL;
}

static mcp_json_value_t *params1(mcp_context_t *ctx, const char *text) {
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    mcp_json_value_t *v = mcp_json_string_new(ctx, text);
    if (!p || !v || mcp_json_object_set(ctx, p, "text", v) != MCP_OK) {
        mcp_json_destroy(ctx, v);
        mcp_json_destroy(ctx, p);
        return NULL;
    }
    return p;
}

static int error_code(mcp_context_t *ctx, const mcp_message_t *resp) {
    int code = 0;
    CHECK(mcp_message_error_code(ctx, resp, &code) == MCP_OK);
    return code;
}

static mcp_json_value_t *call_params(mcp_context_t *ctx, const char *name,
                                     mcp_json_value_t *args_or_null) {
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    mcp_json_value_t *n = mcp_json_string_new(ctx, name);
    if (!p || !n || mcp_json_object_set(ctx, p, "name", n) != MCP_OK) {
        mcp_json_destroy(ctx, n);
        mcp_json_destroy(ctx, p);
        return NULL;
    }
    if (args_or_null != NULL &&
        mcp_json_object_set(ctx, p, "arguments", args_or_null) != MCP_OK) {
        mcp_json_destroy(ctx, args_or_null);
        mcp_json_destroy(ctx, p);
        return NULL;
    }
    return p;
}

static mcp_message_t *dispatch_new(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *id, const char *method, mcp_json_value_t *params) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, params);
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp);
    return resp;
}

static void setup(mcp_context_t **ctx, mcp_server_t **srv, mcp_session_t **s) {
    *ctx = mcp_context_create(NULL);
    CHECK(*ctx);
    *srv = mcp_server_create(*ctx, "srv", "1");
    CHECK(*srv);
    CHECK(mcp_server_add_tool(*ctx, *srv,
                               mcp_tool_new(*ctx, "echo", "echo", text_schema(*ctx),
                                            echo_handler, NULL)) == MCP_OK);
    CHECK(mcp_server_add_tool(*ctx, *srv, mcp_tool_new(*ctx, "fail", NULL, NULL, fail_handler,
                                                       NULL)) == MCP_OK);
    CHECK(mcp_server_add_tool(*ctx, *srv, mcp_tool_new(*ctx, "slow", NULL, NULL, slow_handler,
                                                       NULL)) == MCP_OK);
    CHECK(mcp_server_add_resource(*ctx, *srv, mcp_resource_new(*ctx, "file:///a", "a",
                                                               "text/plain", static_read,
                                                               NULL)) == MCP_OK);
    CHECK(mcp_server_add_prompt(*ctx, *srv, mcp_prompt_new(*ctx, "greet", "say hi", static_prompt,
                                                            NULL)) == MCP_OK);
    CHECK(mcp_server_register_completion_provider(*ctx, *srv, "prompt/", compl_provider,
                                                    NULL) == MCP_OK);
    *s = mcp_server_create_session(*ctx, *srv);
    CHECK(*s);
}

static void teardown(mcp_context_t *ctx, mcp_server_t *srv) {
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

static void init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    mcp_json_value_t *params = mcp_initialize_params_new(ctx, "cli", "1");
    CHECK(params);
    mcp_message_t *resp = dispatch_new(ctx, srv, s, "init", "initialize", params);
    CHECK(mcp_message_result(ctx, resp) != NULL);
    mcp_message_destroy(ctx, resp);
    CHECK(!mcp_session_is_initialized(ctx, s));
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);
    CHECK(mcp_session_is_initialized(ctx, s));
}

int main(void) {
    mcp_context_t *ctx;
    mcp_server_t *srv;
    mcp_session_t *s;
    setup(&ctx, &srv, &s);

    // pre-init gate
    mcp_message_t *r = dispatch_new(ctx, srv, s, "g0", "tools/list", NULL);
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_REQUEST);
    mcp_message_destroy(ctx, r);

    init_session(ctx, srv, s);

    // tools/list contains echo with schema
    r = dispatch_new(ctx, srv, s, "g1", "tools/list", NULL);
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *tools = mcp_json_object_get(ctx, res, "tools");
    CHECK(tools && mcp_json_array_size(ctx, tools) == 3);
    bool found_schema = false;
    for (size_t i = 0; i < 3; i++) {
        const mcp_json_value_t *t = mcp_json_array_get(ctx, tools, i);
        const mcp_json_value_t *n = mcp_json_object_get(ctx, t, "name");
        const char *ns = NULL;
        CHECK(mcp_json_string_value(ctx, n, &ns) == MCP_OK);
        if (strcmp(ns, "echo") == 0) {
            found_schema = mcp_json_object_has(ctx, t, "inputSchema");
        }
    }
    CHECK(found_schema);
    mcp_message_destroy(ctx, r);

    // tools/call echo roundtrip
    r = dispatch_new(ctx, srv, s, "c1", "tools/call", call_params(ctx, "echo", params1(ctx, "hey")));
    res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *back = mcp_json_object_get(ctx, res, "text");
    const char *bs = NULL;
    CHECK(mcp_json_string_value(ctx, back, &bs) == MCP_OK && strcmp(bs, "hey") == 0);
    mcp_message_destroy(ctx, r);

    // schema violation (no arguments against required-text schema) -> -32602
    r = dispatch_new(ctx, srv, s, "c2", "tools/call", call_params(ctx, "echo", NULL));
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // unknown tool -> -32602
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "name", mcp_json_string_new(ctx, "nope")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "c3", "tools/call", p);
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // handler failure -> isError result, not RPC error
    p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "name", mcp_json_string_new(ctx, "fail")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "c4", "tools/call", p);
    res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *iserr = mcp_json_object_get(ctx, res, "isError");
    bool b = false;
    CHECK(mcp_json_bool_value(ctx, iserr, &b) == MCP_OK && b);
    mcp_message_destroy(ctx, r);

    // duplicate id -> -32600
    r = dispatch_new(ctx, srv, s, "c1", "ping", NULL);
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_REQUEST);
    mcp_message_destroy(ctx, r);

    // ping
    r = dispatch_new(ctx, srv, s, "p1", "ping", NULL);
    CHECK(mcp_message_result(ctx, r) != NULL);
    mcp_message_destroy(ctx, r);

    // resources
    r = dispatch_new(ctx, srv, s, "r1", "resources/list", NULL);
    res = mcp_message_result(ctx, r);
    CHECK(res && mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "resources")) == 1);
    mcp_message_destroy(ctx, r);
    p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "uri", mcp_json_string_new(ctx, "file:///a")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "r2", "resources/read", p);
    res = mcp_message_result(ctx, r);
    CHECK(res && mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "contents")) == 1);
    mcp_message_destroy(ctx, r);
    p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "uri", mcp_json_string_new(ctx, "file:///missing")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "r3", "resources/read", p);
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // prompts
    r = dispatch_new(ctx, srv, s, "m1", "prompts/list", NULL);
    res = mcp_message_result(ctx, r);
    CHECK(res && mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "prompts")) == 1);
    mcp_message_destroy(ctx, r);
    p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "name", mcp_json_string_new(ctx, "greet")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "m2", "prompts/get", p);
    res = mcp_message_result(ctx, r);
    CHECK(res && mcp_json_array_size(ctx, mcp_json_object_get(ctx, res, "messages")) == 1);
    mcp_message_destroy(ctx, r);
    p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "name", mcp_json_string_new(ctx, "nope")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "m3", "prompts/get", p);
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // completion/list returns empty array
    r = dispatch_new(ctx, srv, s, "cl1", "completion/list", NULL);
    res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *comps = mcp_json_object_get(ctx, res, "completions");
    CHECK(comps && mcp_json_array_size(ctx, comps) == 0);
    mcp_message_destroy(ctx, r);

    // completion/complete: build params with ref object + argument
    p = mcp_json_object_new(ctx);
    mcp_json_value_t *ref_obj = mcp_json_object_new(ctx);
    mcp_json_value_t *type_v = mcp_json_string_new(ctx, "ref");
    mcp_json_value_t *val_v = mcp_json_string_new(ctx, "prompt/greet");
    CHECK(ref_obj && type_v && val_v);
    CHECK(mcp_json_object_set(ctx, ref_obj, "type", type_v) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, ref_obj, "value", val_v) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "ref", ref_obj) == MCP_OK);
    mcp_json_value_t *arg_obj = mcp_json_object_new(ctx);
    CHECK(arg_obj && mcp_json_object_set(ctx, arg_obj, "name", mcp_json_string_new(ctx, "greet")) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "argument", arg_obj) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "cc1", "completion/complete", p);
    res = mcp_message_result(ctx, r);
    CHECK(res);
    comps = mcp_json_object_get(ctx, res, "completions");
    CHECK(comps && mcp_json_array_size(ctx, comps) == 2);
    const mcp_json_value_t *c0 = mcp_json_array_get(ctx, comps, 0);
    const char *c0s = NULL;
    CHECK(mcp_json_string_value(ctx, c0, &c0s) == MCP_OK && strcmp(c0s, "hint") == 0);
    mcp_message_destroy(ctx, r);

    // completion/complete: unknown reference -> empty completions (no provider match)
    p = mcp_json_object_new(ctx);
    ref_obj = mcp_json_object_new(ctx);
    type_v = mcp_json_string_new(ctx, "ref");
    val_v = mcp_json_string_new(ctx, "tool/echo");
    arg_obj = mcp_json_object_new(ctx);
    CHECK(ref_obj && type_v && val_v && arg_obj);
    CHECK(mcp_json_object_set(ctx, ref_obj, "type", type_v) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, ref_obj, "value", val_v) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "ref", ref_obj) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "argument", arg_obj) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "cc2", "completion/complete", p);
    res = mcp_message_result(ctx, r);
    CHECK(res);
    comps = mcp_json_object_get(ctx, res, "completions");
    CHECK(comps && mcp_json_array_size(ctx, comps) == 0);
    mcp_message_destroy(ctx, r);

    // completion/complete: missing ref param -> -32602
    r = dispatch_new(ctx, srv, s, "cc3", "completion/complete", NULL);
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // queue: order + notification + empty
    mcp_queue_t *q = mcp_queue_create(ctx);
    CHECK(q && mcp_queue_size(ctx, q) == 0);
    CHECK(mcp_queue_push(ctx, q, s, mcp_request_new_string_id(ctx, "q1", "ping", NULL)) == MCP_OK);
    CHECK(mcp_queue_push(ctx, q, s, mcp_initialized_notification_new(ctx)) == MCP_OK);
    CHECK(mcp_queue_size(ctx, q) == 2);
    mcp_message_t *qr = NULL;
    CHECK(mcp_server_process_one(ctx, srv, q, &qr) == MCP_OK && qr != NULL);
    CHECK(mcp_message_result(ctx, qr) != NULL);
    mcp_message_destroy(ctx, qr);
    qr = (void *)0x1;
    CHECK(mcp_server_process_one(ctx, srv, q, &qr) == MCP_OK && qr == NULL);
    CHECK(mcp_server_process_one(ctx, srv, q, &qr) == MCP_ERR_NOT_FOUND && qr == NULL);
    mcp_queue_destroy(ctx, q);

    // slow-tool isolation: session A slow, session B fast, both correct
    mcp_session_t *sb = mcp_server_create_session(ctx, srv);
    CHECK(sb);
    init_session(ctx, srv, sb);
    p = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set(ctx, p, "name", mcp_json_string_new(ctx, "slow")) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "slow1", "tools/call", p);
    CHECK(mcp_message_result(ctx, r) != NULL);
    mcp_message_destroy(ctx, r);
    r = dispatch_new(ctx, srv, sb, "fast1", "ping", NULL);
    CHECK(mcp_message_result(ctx, r) != NULL);
    mcp_message_destroy(ctx, r);

    teardown(ctx, srv);
    return 0;
}
