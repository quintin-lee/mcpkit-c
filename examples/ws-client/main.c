#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/ws.h"

static int fail(mcp_context_t *ctx, mcp_client_t *c, mcp_transport_t *t, mcp_status_t st) {
    fprintf(stderr, "ws-client error: %s\n", mcp_status_string(st));
    if (c != NULL) {
        mcp_client_destroy(ctx, c);
    }
    if (t != NULL) {
        mcp_transport_destroy(ctx, t);
    }
    mcp_context_destroy(ctx);
    return 1;
}

static void print_result(mcp_context_t *ctx, const mcp_json_value_t *v) {
    char *s = mcp_json_serialize(ctx, v);
    if (s != NULL) {
        printf("%s\n", s);
        mcp_json_free_string(ctx, s);
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) {
        fprintf(stderr, "usage: ws-client <ws-url> <list|call|ping> [name] [text]\n");
        fprintf(stderr, "example: ws-client ws://127.0.0.1:8080/mcp list\n");
        fprintf(stderr, "example: ws-client ws://127.0.0.1:8080/mcp call echo 'hello ws'\n");
        return 2;
    }

    const char *url = argv[1];
    const char *cmd = argv[2];

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }

    mcp_transport_t *t = mcp_ws_client_connect(ctx, url);
    if (t == NULL) {
        fprintf(stderr, "ws-client: failed to connect to %s\n", url);
        mcp_context_destroy(ctx);
        return 1;
    }

    mcp_client_t *c = mcp_client_create(ctx, t);
    if (c == NULL) {
        mcp_transport_destroy(ctx, t);
        mcp_context_destroy(ctx);
        return 1;
    }

    mcp_status_t st = mcp_client_connect(ctx, c);
    if (st != MCP_OK) {
        return fail(ctx, c, t, st);
    }

    st = mcp_client_initialize(ctx, c, "ws-client-demo", "1.0.0", NULL);
    if (st != MCP_OK) {
        return fail(ctx, c, t, st);
    }

    if (strcmp(cmd, "ping") == 0) {
        st = mcp_client_ping(ctx, c);
        if (st != MCP_OK) {
            return fail(ctx, c, t, st);
        }
        printf("pong\n");
    } else if (strcmp(cmd, "list") == 0) {
        mcp_json_value_t *tools = NULL;
        st = mcp_client_list_tools(ctx, c, &tools);
        if (st != MCP_OK) {
            return fail(ctx, c, t, st);
        }
        print_result(ctx, tools);
        mcp_json_destroy(ctx, tools);
    } else if (strcmp(cmd, "call") == 0) {
        const char *name = argc > 3 ? argv[3] : "echo";
        const char *text = argc > 4 ? argv[4] : "hello ws";

        mcp_json_value_t *args = mcp_json_object_new(ctx);
        mcp_json_value_t *v = mcp_json_string_new(ctx, text);
        mcp_json_object_set(ctx, args, "text", v);

        mcp_json_value_t *result = NULL;
        st = mcp_client_call_tool(ctx, c, name, args, &result);
        if (st != MCP_OK) {
            return fail(ctx, c, t, st);
        }
        print_result(ctx, result);
        mcp_json_destroy(ctx, result);
    } else {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        mcp_client_destroy(ctx, c);
        mcp_transport_destroy(ctx, t);
        mcp_context_destroy(ctx);
        return 2;
    }

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    return 0;
}
