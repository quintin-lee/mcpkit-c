/**
 * @file test_tls_transport.c
 *
 * Loopback TLS roundtrip test.  Requires a self-signed cert generated
 * at CMake build time (passed as argv[1]=cert, argv[2]=key).
 * Server and client are in the same process on port 45700.
 * The TLS handshake is deferred to mcp_transport_start() for both
 * sides; the server's start() runs in a thread so the handshake can
 * complete concurrently with the client's start().
 */
#include "test_check.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/tls.h"

static const uint16_t kPort = 45700;

static const char *kInit =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"tls-test\",\"version\":\"0.1.0\"}}}";

typedef struct {
    mcp_context_t   *ctx;
    mcp_transport_t *srv;
    mcp_status_t     st;
} server_args_t;

/* Server thread: run the TLS handshake (accept + SSL_accept). */
static void *server_start_fn(void *arg) {
    server_args_t *a = (server_args_t *)arg;
    a->st = mcp_transport_start(a->ctx, a->srv);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 1;
    }
    const char *cert = argv[1];
    const char *key  = argv[2];

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* --- Server: bind + TLS listener; handshake deferred to start() --- */
    mcp_transport_t *srv = mcp_tls_transport_create(ctx, NULL, kPort, true, cert, key);
    CHECK(srv != NULL);

    /* --- Client: connect; handshake deferred to start() --- */
    mcp_transport_t *cli = mcp_tls_transport_create(ctx, "127.0.0.1", kPort,
                                                     false, NULL, NULL);
    CHECK(cli != NULL);

    /* Run server-side handshake in a thread so it can proceed
     * concurrently with the client's SSL_connect. */
    server_args_t args = { .ctx = ctx, .srv = srv, .st = MCP_OK };
    pthread_t thr;
    CHECK(pthread_create(&thr, NULL, server_start_fn, &args) == 0);

    /* Client-side handshake (SSL_connect). */
    CHECK(mcp_transport_start(ctx, cli) == MCP_OK);

    /* Wait for server's SSL_accept to complete. */
    CHECK(pthread_join(thr, NULL) == 0);
    CHECK(args.st == MCP_OK);

    /* Send init request from client -> server. */
    CHECK(mcp_transport_send(ctx, cli, kInit, strlen(kInit)) == MCP_OK);

    char *line = NULL;
    CHECK(mcp_transport_recv(ctx, srv, &line) == MCP_OK);
    CHECK(line != NULL);
    CHECK(strstr(line, "initialize") != NULL);
    mcp_json_free_string(ctx, line);

    /* Send a canned response from server -> client. */
    const char *kResp =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
        "\"protocolVersion\":\"2025-06-18\","
        "\"serverInfo\":{\"name\":\"tls-test\",\"version\":\"0.1.0\"}}}";
    CHECK(mcp_transport_send(ctx, srv, kResp, strlen(kResp)) == MCP_OK);

    CHECK(mcp_transport_recv(ctx, cli, &line) == MCP_OK);
    CHECK(line != NULL);
    CHECK(strstr(line, "2025-06-18") != NULL);
    mcp_json_free_string(ctx, line);

    /* Clean teardown. */
    CHECK(mcp_transport_stop(ctx, srv) == MCP_OK);
    mcp_transport_destroy(ctx, srv);
    CHECK(mcp_transport_stop(ctx, cli) == MCP_OK);
    mcp_transport_destroy(ctx, cli);

    mcp_context_destroy(ctx);
    printf("test_tls_transport OK\n");
    return 0;
}
