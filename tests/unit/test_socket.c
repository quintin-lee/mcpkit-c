#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    uint16_t port = 45678;

    // create server first (bind+listen); its accept is deferred to start().
    mcp_transport_t *server_t = mcp_socket_transport_create(ctx, NULL, port, true);
    CHECK(server_t != NULL);

    // create client now: connect succeeds because the server is already
    // listening; the accepted connection queues until the server's start()
    // calls accept(), so no deadlock.
    mcp_transport_t *client_t = mcp_socket_transport_create(ctx, "127.0.0.1", port, false);
    CHECK(client_t != NULL);
    CHECK(mcp_transport_start(ctx, client_t) == MCP_OK);

    CHECK(mcp_transport_start(ctx, server_t) == MCP_OK);

    const char *msg = "hello socket";
    mcp_status_t st = mcp_transport_send(ctx, client_t, msg, strlen(msg));
    CHECK(st == MCP_OK);

    char *line = NULL;
    st = mcp_transport_recv(ctx, server_t, &line);
    CHECK(st == MCP_OK);
    CHECK(line != NULL);
    CHECK(strcmp(line, "hello socket") == 0);
    mcp_json_free_string(ctx, line);

    st = mcp_transport_send(ctx, server_t, "reply", 5);
    CHECK(st == MCP_OK);
    line = NULL;
    st = mcp_transport_recv(ctx, client_t, &line);
    CHECK(st == MCP_OK);
    CHECK(strcmp(line, "reply") == 0);
    mcp_json_free_string(ctx, line);

    CHECK(mcp_transport_stop(ctx, client_t) == MCP_OK);
    CHECK(mcp_transport_stop(ctx, server_t) == MCP_OK);

    mcp_transport_destroy(ctx, client_t);
    mcp_transport_destroy(ctx, server_t);

    mcp_context_destroy(ctx);
    printf("OK\n");
    return 0;
}
