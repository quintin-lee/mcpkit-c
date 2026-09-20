#define _POSIX_C_SOURCE 200809L

#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    CHECK(mcp_transport_set_timeout(ctx, NULL, 100, 100) == MCP_ERR_INVALID_ARGUMENT);
    uint64_t r = 0, w = 0;
    CHECK(mcp_transport_get_timeout(ctx, NULL, &r, &w) == MCP_ERR_INVALID_ARGUMENT);

    uint16_t port = 45701;
    mcp_transport_t *server_t = mcp_socket_transport_create(ctx, NULL, port, true);
    CHECK(server_t != NULL);

    CHECK(mcp_transport_get_timeout(ctx, server_t, &r, &w) == MCP_OK);
    CHECK(r == 0 && w == 0);

    CHECK(mcp_transport_set_timeout(ctx, server_t, 100, 100) == MCP_OK);
    CHECK(mcp_transport_get_timeout(ctx, server_t, &r, &w) == MCP_OK);
    CHECK(r == 100 && w == 100);

    mcp_transport_t *client_t = mcp_socket_transport_create(ctx, "127.0.0.1", port, false);
    CHECK(client_t != NULL);
    CHECK(mcp_transport_start(ctx, client_t) == MCP_OK);

    CHECK(mcp_transport_set_timeout(ctx, client_t, 100, 100) == MCP_OK);

    char *line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, client_t, &line) == MCP_ERR_TIMEOUT);
    CHECK(line == NULL);

    CHECK(mcp_transport_start(ctx, server_t) == MCP_OK);
    line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, server_t, &line) == MCP_ERR_TIMEOUT);
    CHECK(line == NULL);

    CHECK(mcp_transport_send(ctx, client_t, "hi", 2) == MCP_OK);
    line = NULL;
    CHECK(mcp_transport_recv(ctx, server_t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, "hi") == 0);
    mcp_json_free_string(ctx, line);

    CHECK(mcp_transport_stop(ctx, client_t) == MCP_OK);
    CHECK(mcp_transport_stop(ctx, server_t) == MCP_OK);
    mcp_transport_destroy(ctx, client_t);
    mcp_transport_destroy(ctx, server_t);

    int fds[2];
    CHECK(pipe(fds) == 0);
    FILE *in = fdopen(fds[0], "r");
    CHECK(in != NULL);
    FILE *out = tmpfile();
    CHECK(out != NULL);
    mcp_transport_t *st = mcp_stdio_transport_create(ctx, in, out);
    CHECK(st != NULL);
    CHECK(mcp_transport_set_timeout(ctx, st, 100, 0) == MCP_OK);
    line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, st, &line) == MCP_ERR_TIMEOUT);
    CHECK(line == NULL);
    CHECK(write(fds[1], "hello\n", 6) == 6);
    line = NULL;
    CHECK(mcp_transport_recv(ctx, st, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, "hello") == 0);
    mcp_json_free_string(ctx, line);
    CHECK(mcp_transport_stop(ctx, st) == MCP_OK);
    mcp_transport_destroy(ctx, st);
    fclose(in);
    fclose(out);
    close(fds[1]);

    mcp_context_destroy(ctx);
    printf("OK\n");
    return 0;
}
