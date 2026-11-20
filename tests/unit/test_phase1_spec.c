#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_check.h"
#include "mcpkit/mcpkit.h"

/* Captured notifications for testing progress reporting */
static char g_last_notification[1024] = {0};
static int g_notification_count = 0;

static mcp_status_t test_notify_sink(mcp_context_t *ctx, mcp_session_t *session,
                                     const char *json_text, size_t len, void *user_data) {
    (void)ctx;
    (void)session;
    (void)user_data;
    if (len < sizeof(g_last_notification)) {
        memcpy(g_last_notification, json_text, len);
        g_last_notification[len] = '\0';
    }
    g_notification_count++;
    return MCP_OK;
}

/* Helper to dispatch a JSON-RPC request message */
static mcp_message_t *dispatch_req(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *id_str, const char *method, mcp_json_value_t *params) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id_str, method, params);
    CHECK(req != NULL);
    mcp_message_t *resp = NULL;
    mcp_status_t st = mcp_server_dispatch(ctx, srv, s, req, &resp);
    mcp_message_destroy(ctx, req);
    CHECK(st == MCP_OK);
    return resp;
}

/* --- Resource Template Callbacks --- */
static mcp_status_t template_file_read(mcp_context_t *ctx, mcp_session_t *session,
                                      const char *uri, const mcp_json_value_t *vars,
                                      void *user_data, mcp_json_value_t **contents_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *pv = mcp_json_object_get(ctx, vars, "path");
    const char *path = NULL;
    CHECK(pv != NULL && mcp_json_string_value(ctx, pv, &path) == MCP_OK && path != NULL);

    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    CHECK(item != NULL && arr != NULL);

    char buf[256];
    snprintf(buf, sizeof(buf), "Content of path: %s", path);

    mcp_json_object_set_take(ctx, item, "uri", mcp_json_string_new(ctx, uri));
    mcp_json_object_set_take(ctx, item, "mimeType", mcp_json_string_new(ctx, "text/plain"));
    mcp_json_object_set_take(ctx, item, "text", mcp_json_string_new(ctx, buf));
    mcp_json_array_append(ctx, arr, item);

    *contents_out = arr;
    return MCP_OK;
}

/* --- Tool Progress Handler --- */
static mcp_status_t heavy_task_handler(mcp_context_t *ctx, mcp_session_t *session,
                                       const mcp_tool_call_ctx_t *call_ctx, void *user_data,
                                       mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    CHECK(call_ctx != NULL);
    CHECK(call_ctx->progress_token != NULL);

    /* Report 50% progress */
    mcp_status_t st = mcp_tool_report_progress(ctx, call_ctx, 50.0, 100.0, "processing step 1");
    CHECK(st == MCP_OK);

    /* Report 100% progress */
    st = mcp_tool_report_progress(ctx, call_ctx, 100.0, 100.0, "done");
    CHECK(st == MCP_OK);

    *result_out = mcp_tool_result_text_new(ctx, "heavy task completed successfully");
    return MCP_OK;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "phase1-test-server", "1.0.0");
    CHECK(server != NULL);

    mcp_session_t *session = mcp_server_create_session(ctx, server);
    CHECK(session != NULL);

    /* 1. Initialize Handshake */
    {
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, params, "protocolVersion", mcp_json_string_new(ctx, "2025-06-18"));
        mcp_json_value_t *ci = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, ci, "name", mcp_json_string_new(ctx, "test-client"));
        mcp_json_object_set_take(ctx, ci, "version", mcp_json_string_new(ctx, "1.0"));
        mcp_json_object_set_take(ctx, params, "clientInfo", ci);

        mcp_message_t *resp = dispatch_req(ctx, server, session, "init", "initialize", params);
        CHECK(resp != NULL);
        mcp_message_destroy(ctx, resp);

        mcp_message_t *notif = mcp_notification_new(ctx, "notifications/initialized", NULL);
        CHECK(mcp_server_notify(ctx, server, session, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);
    }

    /* 2. Test Resource Template Registration & API */
    {
        mcp_resource_template_t *tmpl1 = mcp_resource_template_new(ctx, "file:///{+path}", "Local Files",
                                                                  "Access files via path", "text/plain",
                                                                  template_file_read, NULL);
        CHECK(tmpl1 != NULL);
        CHECK(mcp_server_add_resource_template(ctx, server, tmpl1) == MCP_OK);

        /* Duplicate registration check */
        mcp_resource_template_t *tmpl1_dup = mcp_resource_template_new(ctx, "file:///{+path}", "Local Files 2",
                                                                      NULL, NULL, template_file_read, NULL);
        CHECK(mcp_server_add_resource_template(ctx, server, tmpl1_dup) == MCP_ERR_ALREADY_EXISTS);
        mcp_resource_template_destroy(ctx, tmpl1_dup);

        /* Second template */
        mcp_resource_template_t *tmpl2 = mcp_resource_template_new(ctx, "db://users/{id}", "User DB",
                                                                  "Database records", "application/json",
                                                                  template_file_read, NULL);
        CHECK(mcp_server_add_resource_template(ctx, server, tmpl2) == MCP_OK);

        /* Test resources/templates/list */
        mcp_message_t *resp = dispatch_req(ctx, server, session, "t-list", "resources/templates/list", NULL);
        CHECK(resp != NULL);
        const mcp_json_value_t *res = mcp_message_result(ctx, resp);
        CHECK(res != NULL);

        const mcp_json_value_t *r_tmpls = mcp_json_object_get(ctx, res, "resourceTemplates");
        CHECK(r_tmpls != NULL && mcp_json_type(ctx, r_tmpls) == MCP_JSON_ARRAY);
        CHECK(mcp_json_array_size(ctx, r_tmpls) == 2);

        /* Compatibility alias check */
        const mcp_json_value_t *tmpls_legacy = mcp_json_object_get(ctx, res, "templates");
        CHECK(tmpls_legacy != NULL && mcp_json_type(ctx, tmpls_legacy) == MCP_JSON_ARRAY);
        CHECK(mcp_json_array_size(ctx, tmpls_legacy) == 2);

        mcp_message_destroy(ctx, resp);

        /* Test resources/read via template match */
        mcp_json_value_t *read_params = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, read_params, "uri", mcp_json_string_new(ctx, "file:///docs/readme.txt"));
        resp = dispatch_req(ctx, server, session, "t-read", "resources/read", read_params);
        CHECK(resp != NULL);
        res = mcp_message_result(ctx, resp);
        CHECK(res != NULL);
        const mcp_json_value_t *contents = mcp_json_object_get(ctx, res, "contents");
        CHECK(contents != NULL && mcp_json_type(ctx, contents) == MCP_JSON_ARRAY);
        CHECK(mcp_json_array_size(ctx, contents) == 1);
        const mcp_json_value_t *first = mcp_json_array_get(ctx, contents, 0);
        const char *text = NULL;
        CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, first, "text"), &text) == MCP_OK);
        CHECK(strcmp(text, "Content of path: docs/readme.txt") == 0);
        mcp_message_destroy(ctx, resp);

        /* Test removing resource template */
        CHECK(mcp_server_remove_resource_template(ctx, server, "file:///{+path}") == MCP_OK);
        CHECK(mcp_server_remove_resource_template(ctx, server, "file:///{+path}") == MCP_ERR_NOT_FOUND);

        /* Verify it no longer matches */
        read_params = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, read_params, "uri", mcp_json_string_new(ctx, "file:///docs/readme.txt"));
        resp = dispatch_req(ctx, server, session, "t-read-fail", "resources/read", read_params);
        CHECK(resp != NULL);
        int err_code = 0;
        CHECK(mcp_message_error_code(ctx, resp, &err_code) == MCP_OK);
        CHECK(err_code == MCP_RPC_INVALID_PARAMS);
        mcp_message_destroy(ctx, resp);
    }

    /* 3. Test Tool Progress Reporting */
    {
        /* Register tool with V2 handler */
        mcp_tool_t *t = mcp_tool_new_v2(ctx, "heavy_worker", "Long running worker task",
                                        NULL, heavy_task_handler, NULL);
        CHECK(mcp_server_add_tool(ctx, server, t) == MCP_OK);

        /* Attach notify sink to capture progress notification */
        mcp_session_set_sink(session, test_notify_sink, NULL);

        mcp_json_value_t *call_params = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, call_params, "name", mcp_json_string_new(ctx, "heavy_worker"));
        mcp_json_value_t *meta = mcp_json_object_new(ctx);
        mcp_json_object_set_take(ctx, meta, "progressToken", mcp_json_string_new(ctx, "progress-abc-123"));
        mcp_json_object_set_take(ctx, call_params, "_meta", meta);

        g_notification_count = 0;
        memset(g_last_notification, 0, sizeof(g_last_notification));

        mcp_message_t *resp = dispatch_req(ctx, server, session, "call-heavy", "tools/call", call_params);
        CHECK(resp != NULL);
        const mcp_json_value_t *res = mcp_message_result(ctx, resp);
        CHECK(res != NULL);
        mcp_message_destroy(ctx, resp);

        /* Assert notifications/progress was emitted */
        CHECK(g_notification_count == 2);
        CHECK(strstr(g_last_notification, "notifications/progress") != NULL);
        CHECK(strstr(g_last_notification, "progress-abc-123") != NULL);
        CHECK(strstr(g_last_notification, "\"progress\":100") != NULL);
        CHECK(strstr(g_last_notification, "done") != NULL);
    }

    /* 4. Test EmbeddedResource Builders */
    {
        mcp_json_value_t *res_item = mcp_content_resource_new(ctx, "file:///sample.txt",
                                                              "text/plain", "Embedded file content", false);
        CHECK(res_item != NULL);
        const char *ty = NULL;
        CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, res_item, "type"), &ty) == MCP_OK);
        CHECK(strcmp(ty, "resource") == 0);
        const mcp_json_value_t *inner = mcp_json_object_get(ctx, res_item, "resource");
        CHECK(inner != NULL);
        const char *inner_txt = NULL;
        CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, inner, "text"), &inner_txt) == MCP_OK);
        CHECK(strcmp(inner_txt, "Embedded file content") == 0);
        mcp_json_destroy(ctx, res_item);

        /* Tool result helper */
        mcp_json_value_t *tool_res = mcp_tool_result_resource_new(ctx, "file:///binary.bin",
                                                                  "application/octet-stream", "AQIDBA==", true);
        CHECK(tool_res != NULL);
        const mcp_json_value_t *content = mcp_json_object_get(ctx, tool_res, "content");
        CHECK(content != NULL && mcp_json_array_size(ctx, content) == 1);
        mcp_json_destroy(ctx, tool_res);
    }

    /* Teardown */
    mcp_server_destroy_session(ctx, server, session);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);

    printf("PASS: test_phase1_spec completed successfully!\n");
    return 0;
}
