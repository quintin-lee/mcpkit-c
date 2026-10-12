/* Socket acceptance probe: drive tools/call over TCP, check echo bytes.
 * argv: <host> <port>. Exit 0 on "sock-probe" round-trip, 1 otherwise.
 * Test-only helper; built with the test suite, never installed. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

#define PROBE_TEXT "sock-probe"

static int fail(mcp_context_t *ctx, mcp_client_t *c, mcp_transport_t *t,
                const char *what, mcp_status_t st) {
    fprintf(stderr, "probe_sock: %s: %s\n", what, mcp_status_string(st));
    if (c != NULL) {
        mcp_client_destroy(ctx, c);
    }
    if (t != NULL) {
        mcp_transport_destroy(ctx, t);
    }
    mcp_context_destroy(ctx);
    return 1;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) {
        fprintf(stderr, "usage: probe_sock <host> <port>\n");
        return 2;
    }
    unsigned long port = strtoul(argv[2], NULL, 10);
    if (port == 0 || port > 65535) {
        fprintf(stderr, "probe_sock: bad port '%s'\n", argv[2]);
        return 2;
    }
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_transport_t *t =
        mcp_socket_transport_create(ctx, argv[1], (uint16_t)port, false);
    if (t == NULL) {
        fprintf(stderr, "probe_sock: connect failed\n");
        mcp_context_destroy(ctx);
        return 1;
    }
    mcp_client_t *c = mcp_client_create(ctx, t);
    if (c == NULL) {
        return fail(ctx, NULL, t, "client create", MCP_ERR_NOMEM);
    }
    mcp_status_t st = mcp_client_connect(ctx, c);
    if (st != MCP_OK) {
        return fail(ctx, c, t, "connect", st);
    }
    st = mcp_client_initialize(ctx, c, "probe-sock", "0.1.0", NULL);
    if (st != MCP_OK) {
        return fail(ctx, c, t, "initialize", st);
    }
    mcp_json_value_t *args = mcp_json_object_new(ctx);
    if (args == NULL) {
        return fail(ctx, c, t, "args", MCP_ERR_NOMEM);
    }
    if (mcp_json_object_set(ctx, args, "text", mcp_json_string_new(ctx, PROBE_TEXT)) !=
        MCP_OK) {
        mcp_json_destroy(ctx, args);
        return fail(ctx, c, t, "args", MCP_ERR_NOMEM);
    }
    mcp_json_value_t *res = NULL;
    st = mcp_client_call_tool(ctx, c, "echo", args, &res);
    if (st != MCP_OK) {
        return fail(ctx, c, t, "call", st);
    }
    char *s = mcp_json_serialize(ctx, res);
    int rc = 1;
    if (s != NULL) {
        printf("%s\n", s);
        rc = strstr(s, PROBE_TEXT) != NULL ? 0 : 1;
        if (rc != 0) {
            fprintf(stderr, "probe_sock: echo text missing in result\n");
        }
        mcp_json_free_string(ctx, s);
    }
    mcp_json_destroy(ctx, res);
    mcp_status_t dc = mcp_client_disconnect(ctx, c);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    return dc == MCP_OK ? rc : 1;
}
