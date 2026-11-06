#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/ws.h"

/*
 * Red-light gate for the WS secure_random_bytes fix (getrandom replaces
 * the old rand()-based key/mask derivation, which ran without srand()).
 * The public ws transport API (client-side frames must be masked)
 * exercises the secure_random_bytes path end-to-end in ws_send and the
 * PONG auto-reply in ws_recv: if the helper ever returned -1 those
 * calls would fail with MCP_ERR_IO instead of producing a working
 * frame. This test locks in that behavior on a real socketpair.
 */

static void test_ws_masked_roundtrip(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* sv[0] is the server side (unmasked outbound), sv[1] is the client
     * side (masked outbound, exercises secure_random_bytes per frame). */
    mcp_transport_t *srv_t = mcp_ws_transport_create(ctx, sv[0], true);
    mcp_transport_t *cli_t = mcp_ws_transport_create(ctx, sv[1], false);
    CHECK(srv_t != NULL);
    CHECK(cli_t != NULL);

    CHECK(mcp_transport_start(ctx, srv_t) == MCP_OK);
    CHECK(mcp_transport_start(ctx, cli_t) == MCP_OK);

    /* 1. Client -> server: masked TEXT frame, two back-to-back sends so
     *    two distinct random mask keys are generated. */
    const char *req_a = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
    const char *req_b = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";
    CHECK(mcp_transport_send(ctx, cli_t, req_a, strlen(req_a)) == MCP_OK);
    CHECK(mcp_transport_send(ctx, cli_t, req_b, strlen(req_b)) == MCP_OK);

    char *line = NULL;
    CHECK(mcp_transport_recv(ctx, srv_t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, req_a) == 0);
    mcp_json_free_string(ctx, line);

    line = NULL;
    CHECK(mcp_transport_recv(ctx, srv_t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, req_b) == 0);
    mcp_json_free_string(ctx, line);

    /* 2. Server -> client: unmasked TEXT frame, client unmask path. */
    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[]}}";
    CHECK(mcp_transport_send(ctx, srv_t, resp, strlen(resp)) == MCP_OK);

    line = NULL;
    CHECK(mcp_transport_recv(ctx, cli_t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, resp) == 0);
    mcp_json_free_string(ctx, line);

    /* 3. Server PING -> client auto-answers with a masked PONG: this
     *    forces the secure_random_bytes call inside ws_recv for the PONG
     *    mask key. If that call fails the client recv must not hang or
     *    crash; the PONG is transparently swallowed by ws_recv. */
    mcp_ws_frame_t ping = {
        .fin = true,
        .opcode = MCP_WS_OP_PING,
        .masked = false,
        .payload_len = 4,
        .payload = "ping",
    };
    uint8_t *ping_bytes = NULL;
    size_t ping_len = 0;
    CHECK(mcp_ws_encode_frame(ctx, &ping, &ping_bytes, &ping_len) == MCP_OK);
    CHECK(ping_bytes != NULL && ping_len > 0);
    ssize_t sent = 0;
    while (sent < (ssize_t)ping_len) {
        ssize_t n = send(sv[0], ping_bytes + sent, ping_len - (size_t)sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += n;
    }
    CHECK(sent == (ssize_t)ping_len);
    mcp_json_free_string(ctx, (char *)ping_bytes);

    /* Next TEXT frame from the server still arrives intact on the client
     * even though the PONG round-trip was in between. */
    const char *resp2 = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[]}}";
    CHECK(mcp_transport_send(ctx, srv_t, resp2, strlen(resp2)) == MCP_OK);

    line = NULL;
    CHECK(mcp_transport_recv(ctx, cli_t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, resp2) == 0);
    mcp_json_free_string(ctx, line);

    CHECK(mcp_transport_stop(ctx, srv_t) == MCP_OK);
    CHECK(mcp_transport_stop(ctx, cli_t) == MCP_OK);

    mcp_transport_destroy(ctx, srv_t);
    mcp_transport_destroy(ctx, cli_t);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_ws_masked_roundtrip();
    printf("test_ws_randomness: OK\n");
    return 0;
}
