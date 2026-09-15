#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static int fail(mcp_context_t *ctx, mcp_client_t *c, mcp_transport_t *t, mcp_status_t st) {
    fprintf(stderr, "client: %s\n", mcp_status_string(st));
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
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
    if (argc < 2 || (strcmp(argv[1], "list") != 0 && strcmp(argv[1], "call") != 0 &&
                     strcmp(argv[1], "ping") != 0)) {
        fprintf(stderr, "usage: client <list|call|ping> [name] [text]\n");
        return 2;
    }
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    if (t == NULL) {
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
    st = mcp_client_initialize(ctx, c, "example-client", "0.1.0", NULL);
    if (st != MCP_OK) {
        return fail(ctx, c, t, st);
    }
    if (strcmp(argv[1], "ping") == 0) {
        st = mcp_client_ping(ctx, c);
        if (st != MCP_OK) {
            return fail(ctx, c, t, st);
        }
        printf("pong\n");
    } else if (strcmp(argv[1], "list") == 0) {
        mcp_json_value_t *tools = NULL;
        st = mcp_client_list_tools(ctx, c, &tools);
        if (st != MCP_OK) {
            return fail(ctx, c, t, st);
        }
        print_result(ctx, tools);
        mcp_json_destroy(ctx, tools);
    } else {
        const char *name = argc > 2 ? argv[2] : "echo";
        const char *text = argc > 3 ? argv[3] : "hello";
        mcp_json_value_t *args = mcp_json_object_new(ctx);
        if (args == NULL) {
            return fail(ctx, c, t, MCP_ERR_NOMEM);
        }
        if (mcp_json_object_set(ctx, args, "text", mcp_json_string_new(ctx, text)) != MCP_OK) {
            mcp_json_destroy(ctx, args);
            return fail(ctx, c, t, MCP_ERR_NOMEM);
        }
        mcp_json_value_t *res = NULL;
        st = mcp_client_call_tool(ctx, c, name, args, &res);
        if (st != MCP_OK) {
            return fail(ctx, c, t, st);
        }
        print_result(ctx, res);
        mcp_json_destroy(ctx, res);
    }
    mcp_status_t dc = mcp_client_disconnect(ctx, c);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    return dc == MCP_OK ? 0 : 1;
}
