#define _DEFAULT_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"

#define APPS_SERVER_UI_URI "ui://app/greet"

static mcp_status_t set_new_string(mcp_context_t *ctx, mcp_json_value_t *obj,
                                   const char *key, const char *val) {
    mcp_json_value_t *v = mcp_json_string_new(ctx, val);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_json_object_set(ctx, obj, key, v);
    if (st != MCP_OK) {
        mcp_json_destroy(ctx, v);
    }
    return st;
}

static mcp_status_t greet_handler(mcp_context_t *ctx, mcp_session_t *session,
                                  const mcp_json_value_t *args, void *user_data,
                                  mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;

    const mcp_json_value_t *namev = mcp_json_object_get(ctx, args, "name");
    const char *name = NULL;
    if (namev == NULL || mcp_json_string_value(ctx, namev, &name) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    char greeting[256];
    snprintf(greeting, sizeof(greeting), "hello, %s", name);

    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_status_t stt = MCP_ERR_NOMEM;
    int item_linked = 0;
    int content_linked = 0;
    if (item != NULL && content != NULL && result != NULL &&
        set_new_string(ctx, item, "type", "text") == MCP_OK &&
        set_new_string(ctx, item, "text", greeting) == MCP_OK &&
        mcp_json_array_append(ctx, content, item) == MCP_OK) {
        item_linked = 1;
        if (mcp_json_object_set(ctx, result, "content", content) == MCP_OK) {
            content_linked = 1;
            stt = mcp_apps_result_with_ui(ctx, result, APPS_SERVER_UI_URI);
        }
    }
    if (stt != MCP_OK) {
        if (!item_linked) {
            mcp_json_destroy(ctx, item);
        }
        if (!content_linked) {
            mcp_json_destroy(ctx, content);
        }
        mcp_json_destroy(ctx, result);
        return stt;
    }
    *result_out = result;
    return MCP_OK;
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s\n", prog);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        usage(argv[0]);
        return 2;
    }

    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "apps-server", "0.1.0");
    if (srv == NULL) {
        fprintf(stderr, "server create failed\n");
        goto done;
    }
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_tool_t *tool = NULL;
    if (schema == NULL ||
        mcp_schema_add_property(ctx, schema, "name", mcp_schema_string_new(ctx)) !=
            MCP_OK ||
        mcp_schema_add_required(ctx, schema, "name") != MCP_OK) {
        fprintf(stderr, "schema build failed\n");
        goto done;
    }
    tool = mcp_tool_new(ctx, "greet", "Greet a name; result carries _meta.ui.", schema,
                        greet_handler, NULL);
    if (tool == NULL) {
        fprintf(stderr, "tool create failed\n");
        mcp_json_destroy(ctx, schema);
        goto done;
    }
    if (mcp_apps_tool_set_ui(ctx, tool, APPS_SERVER_UI_URI) != MCP_OK ||
        mcp_server_add_tool(ctx, srv, tool) != MCP_OK) {
        fprintf(stderr, "tool register failed\n");
        mcp_tool_destroy(ctx, tool);
        goto done;
    }
    mcp_csp_t *csp = mcp_csp_default_deny_new(ctx);
    mcp_resource_t *ui =
        (csp != NULL) ? mcp_apps_ui_resource_new(ctx, APPS_SERVER_UI_URI, "greet",
                                                 "<div><p>apps-server</p></div>", csp)
                      : NULL;
    mcp_csp_destroy(ctx, csp);
    if (ui == NULL || mcp_server_add_resource(ctx, srv, ui) != MCP_OK) {
        fprintf(stderr, "ui resource register failed\n");
        if (ui != NULL) {
            mcp_resource_destroy(ctx, ui);
        }
        goto done;
    }
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    if (t == NULL) {
        fprintf(stderr, "transport create failed\n");
        goto done;
    }
    if (mcp_transport_start(ctx, t) != MCP_OK) {
        fprintf(stderr, "transport start failed\n");
        goto done_transport;
    }
    if (mcp_stdio_serve(ctx, srv, t) != MCP_OK) {
        fprintf(stderr, "serve failed\n");
        goto done_transport;
    }
    rc = 0;

done_transport:
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
done:
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
