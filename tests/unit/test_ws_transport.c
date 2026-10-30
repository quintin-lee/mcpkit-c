#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/ws.h"

static void test_ws_transport_loopback(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* sv[0] is server, sv[1] is client */
    mcp_transport_t *srv_t = mcp_ws_transport_create(ctx, sv[0], true);
    mcp_transport_t *cli_t = mcp_ws_transport_create(ctx, sv[1], false);
    CHECK(srv_t != NULL);
    CHECK(cli_t != NULL);

    CHECK(mcp_transport_start(ctx, srv_t) == MCP_OK);
    CHECK(mcp_transport_start(ctx, cli_t) == MCP_OK);

    /* Client sends request to Server */
    const char *req_msg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
    CHECK(mcp_transport_send(ctx, cli_t, req_msg, strlen(req_msg)) == MCP_OK);

    /* Server receives request */
    char *recv_line = NULL;
    CHECK(mcp_transport_recv(ctx, srv_t, &recv_line) == MCP_OK);
    CHECK(recv_line != NULL);
    CHECK(strcmp(recv_line, req_msg) == 0);
    mcp_json_free_string(ctx, recv_line);

    /* Server sends response to Client */
    const char *resp_msg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[]}}";
    CHECK(mcp_transport_send(ctx, srv_t, resp_msg, strlen(resp_msg)) == MCP_OK);

    /* Client receives response */
    recv_line = NULL;
    CHECK(mcp_transport_recv(ctx, cli_t, &recv_line) == MCP_OK);
    CHECK(recv_line != NULL);
    CHECK(strcmp(recv_line, resp_msg) == 0);
    mcp_json_free_string(ctx, recv_line);

    CHECK(mcp_transport_stop(ctx, srv_t) == MCP_OK);
    CHECK(mcp_transport_stop(ctx, cli_t) == MCP_OK);

    mcp_transport_destroy(ctx, srv_t);
    mcp_transport_destroy(ctx, cli_t);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_ws_transport_loopback();
    printf("test_ws_transport: OK\n");
    return 0;
}
