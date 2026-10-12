#define _DEFAULT_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

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

/* Fire-and-observe: push sampling + roots requests into the server outbox
 * (flushed by the serve loop) and return a queued receipt. The host's
 * answers come back over the wire where nobody matches them to these
 * requests — peer-side markers are the acceptance signal. */
static mcp_status_t ask_model_handler(mcp_context_t *ctx, mcp_session_t *session,
                                      const mcp_json_value_t *args, void *user_data,
                                      mcp_json_value_t **result_out) {
    (void)session;
    (void)args;
    mcp_server_t *srv = (mcp_server_t *)user_data;
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *roots_params = mcp_json_object_new(ctx);
    if (roots_params == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_server_request_client(ctx, srv, "roots/list", roots_params);
    if (st != MCP_OK) {
        mcp_json_destroy(ctx, roots_params);
        return st;
    }
    st = mcp_server_request_client(ctx, srv, "sampling/createMessage", NULL);
    if (st != MCP_OK) {
        return st;
    }

    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_status_t stt = MCP_ERR_NOMEM;
    int item_linked = 0;
    int content_linked = 0;
    if (item != NULL && content != NULL && result != NULL &&
        set_new_string(ctx, item, "type", "text") == MCP_OK &&
        set_new_string(ctx, item, "text",
                       "queued sampling/createMessage + roots/list; "
                       "see peer log for answers.") == MCP_OK &&
        mcp_json_array_append(ctx, content, item) == MCP_OK) {
        item_linked = 1;
        if (mcp_json_object_set(ctx, result, "content", content) == MCP_OK) {
            content_linked = 1;
            stt = MCP_OK;
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
    mcp_server_t *srv = mcp_server_create(ctx, "host-caps", "0.1.0");
    if (srv == NULL) {
        fprintf(stderr, "server create failed\n");
        goto done;
    }
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    if (schema == NULL) {
        fprintf(stderr, "schema build failed\n");
        goto done;
    }
    mcp_tool_t *tool =
        mcp_tool_new(ctx, "ask_model",
                     "Ask the calling host for sampling + roots; returns a queued receipt.",
                     schema, ask_model_handler, srv);
    if (tool == NULL) {
        fprintf(stderr, "tool create failed\n");
        mcp_json_destroy(ctx, schema);
        goto done;
    }
    if (mcp_server_add_tool(ctx, srv, tool) != MCP_OK) {
        fprintf(stderr, "tool register failed\n");
        mcp_tool_destroy(ctx, tool);
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
