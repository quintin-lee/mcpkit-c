#include <signal.h>
#include <stdio.h>

#include "mcpkit/mcpkit.h"

#define UI_URI "ui://status/panel"

static void on_mount(mcp_context_t *ctx, mcp_session_t *session, void *user_data) {
    (void)ctx;
    (void)session;
    printf("mounted: %s\n", (const char *)user_data);
}

static void on_unmount(mcp_context_t *ctx, mcp_session_t *session, void *user_data) {
    (void)ctx;
    (void)session;
    printf("unmounted: %s\n", (const char *)user_data);
}

static mcp_status_t status_handler(mcp_context_t *ctx, mcp_session_t *session,
                                   const mcp_json_value_t *args, void *user_data,
                                   mcp_json_value_t **result_out) {
    (void)session;
    (void)args;
    (void)user_data;
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *v = (result == NULL) ? NULL : mcp_json_string_new(ctx, "ok");
    if (v == NULL || mcp_json_object_set(ctx, result, "status", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        if (result != NULL) {
            mcp_json_destroy(ctx, result);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_apps_result_with_ui(ctx, result, UI_URI) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

static int run_request(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *sess,
                       const char *tag, mcp_message_t *req) {
    int rc = -1;
    mcp_message_t *resp = NULL;
    char *out = NULL;
    if (req == NULL || mcp_server_dispatch(ctx, srv, sess, req, &resp) != MCP_OK ||
        resp == NULL || (out = mcp_message_serialize(ctx, resp)) == NULL) {
        goto done;
    }
    printf("%s: %s\n", tag, out);
    rc = 0;
done:
    if (out != NULL) {
        mcp_json_free_string(ctx, out);
    }
    if (resp != NULL) {
        mcp_message_destroy(ctx, resp);
    }
    if (req != NULL) {
        mcp_message_destroy(ctx, req);
    }
    return rc;
}

static mcp_json_value_t *call_params(mcp_context_t *ctx) {
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *v = (params == NULL) ? NULL : mcp_json_string_new(ctx, "get_system_status");
    if (v == NULL || mcp_json_object_set(ctx, params, "name", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return NULL;
    }
    v = mcp_json_object_new(ctx);
    if (v == NULL || mcp_json_object_set(ctx, params, "arguments", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        mcp_json_destroy(ctx, params);
        return NULL;
    }
    return params;
}

static mcp_json_value_t *read_params(mcp_context_t *ctx) {
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *v = (params == NULL) ? NULL : mcp_json_string_new(ctx, UI_URI);
    if (v == NULL || mcp_json_object_set(ctx, params, "uri", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return NULL;
    }
    return params;
}

int main(void) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "apps-host", "0.1.0");
    mcp_session_t *sess = NULL;
    mcp_apps_mount_t *mount = NULL;
    mcp_csp_t *csp = NULL;
    mcp_resource_t *ui = NULL;
    mcp_message_t *notif = NULL;
    if (srv == NULL) {
        fprintf(stderr, "server create failed\n");
        goto done;
    }
    csp = mcp_csp_default_deny_new(ctx);
    if (csp == NULL) {
        fprintf(stderr, "csp create failed\n");
        goto done;
    }
    ui = mcp_apps_ui_resource_new(ctx, UI_URI, "Status Panel", "<h1>ok</h1>", csp);
    mcp_csp_destroy(ctx, csp);
    csp = NULL;
    if (ui == NULL || mcp_server_add_resource(ctx, srv, ui) != MCP_OK) {
        fprintf(stderr, "ui resource register failed\n");
        if (ui != NULL) {
            mcp_resource_destroy(ctx, ui);
            ui = NULL;
        }
        goto done;
    }
    ui = NULL;
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_tool_t *tool = (schema == NULL)
                           ? NULL
                           : mcp_tool_new(ctx, "get_system_status", "System status", schema,
                                          status_handler, NULL);
    if (tool == NULL ||
        mcp_tool_set_visibility(ctx, tool, MCP_TOOL_VIS_BOTH) != MCP_OK ||
        mcp_tool_require_perms(ctx, tool, MCP_APPS_PERM_CALL_TOOL) != MCP_OK ||
        mcp_server_add_tool(ctx, srv, tool) != MCP_OK) {
        fprintf(stderr, "tool register failed\n");
        if (tool != NULL) {
            mcp_tool_destroy(ctx, tool);
        } else if (schema != NULL) {
            mcp_json_destroy(ctx, schema);
        }
        goto done;
    }
    sess = mcp_server_create_session(ctx, srv);
    if (sess == NULL || mcp_session_set_apps_host(ctx, sess, true) != MCP_OK ||
        mcp_session_grant(ctx, sess, MCP_APPS_PERM_CALL_TOOL) != MCP_OK) {
        fprintf(stderr, "session setup failed\n");
        goto done;
    }
    if (run_request(ctx, srv, sess, "init",
                    mcp_request_new_number_id(ctx, 1, "initialize",
                                              mcp_initialize_params_new(ctx, "apps-host",
                                                                        "0.1.0"))) != 0) {
        fprintf(stderr, "initialize failed\n");
        goto done;
    }
    notif = mcp_initialized_notification_new(ctx);
    if (notif == NULL || mcp_server_notify(ctx, srv, sess, notif) != MCP_OK) {
        fprintf(stderr, "initialized notify failed\n");
        goto done;
    }
    mcp_message_destroy(ctx, notif);
    notif = NULL;
    if (mcp_apps_mount(ctx, sess, on_mount, on_unmount, (void *)UI_URI, &mount) != MCP_OK) {
        fprintf(stderr, "mount failed\n");
        goto done;
    }
    if (run_request(ctx, srv, sess, "call",
                    mcp_request_new_number_id(ctx, 2, "tools/call",
                                              call_params(ctx))) != 0 ||
        run_request(ctx, srv, sess, "read",
                    mcp_request_new_number_id(ctx, 3, "resources/read",
                                              read_params(ctx))) != 0) {
        fprintf(stderr, "call/read failed\n");
        goto done;
    }
    if (mcp_apps_unmount(ctx, mount) != MCP_OK) {
        fprintf(stderr, "unmount failed\n");
        goto done;
    }
    mount = NULL;
    rc = 0;
done:
    if (mount != NULL) {
        mcp_apps_unmount(ctx, mount);
    }
    if (notif != NULL) {
        mcp_message_destroy(ctx, notif);
    }
    if (sess != NULL) {
        mcp_server_destroy_session(ctx, srv, sess);
    }
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
