#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "mcpkit/core/error.h"
#include "mcpkit/transport/socket.h"
#include "mcpkit/transport/transport.h"

typedef struct mock_backend {
    int send_called;
    char last_send_data[256];
    size_t last_send_len;

    int sendv_called;
    size_t last_iovcnt;
    char combined_sendv_data[256];
    size_t combined_sendv_len;
} mock_backend_t;

static mcp_status_t mock_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t mock_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    (void)ctx;
    mock_backend_t *b = (mock_backend_t *)mcp_transport_backend(ctx, t);
    b->send_called++;
    if (len < sizeof(b->last_send_data)) {
        memcpy(b->last_send_data, data, len);
        b->last_send_data[len] = '\0';
    }
    b->last_send_len = len;
    return MCP_OK;
}

static mcp_status_t mock_sendv(mcp_context_t *ctx, mcp_transport_t *t, const mcp_iovec_t *iov,
                               size_t iovcnt) {
    (void)ctx;
    mock_backend_t *b = (mock_backend_t *)mcp_transport_backend(ctx, t);
    b->sendv_called++;
    b->last_iovcnt = iovcnt;
    b->combined_sendv_len = 0;
    for (size_t i = 0; i < iovcnt; i++) {
        if (b->combined_sendv_len + iov[i].iov_len < sizeof(b->combined_sendv_data)) {
            memcpy(b->combined_sendv_data + b->combined_sendv_len, iov[i].iov_base, iov[i].iov_len);
            b->combined_sendv_len += iov[i].iov_len;
        }
    }
    b->combined_sendv_data[b->combined_sendv_len] = '\0';
    return MCP_OK;
}

static mcp_status_t mock_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    (void)ctx;
    (void)t;
    *line_out = NULL;
    return MCP_OK;
}

static mcp_status_t mock_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static void test_sendv_ops_invoked(void) {
    mock_backend_t backend;
    memset(&backend, 0, sizeof(backend));

    mcp_transport_ops_t ops = {
        .start = mock_start,
        .send = mock_send,
        .sendv = mock_sendv,
        .recv = mock_recv,
        .stop = mock_stop,
    };

    mcp_transport_t *t = mcp_transport_create(NULL, &ops, &backend);
    assert(t != NULL);

    mcp_iovec_t iov[3] = {
        {.iov_base = "{\"jsonrpc\":", .iov_len = 11},
        {.iov_base = "\"2.0\"}", .iov_len = 6},
        {.iov_base = "", .iov_len = 0},
    };

    mcp_status_t st = mcp_transport_sendv(NULL, t, iov, 3);
    assert(st == MCP_OK);
    (void)st;
    assert(backend.sendv_called == 1);
    assert(backend.send_called == 0);
    assert(backend.last_iovcnt == 3);
    assert(strcmp(backend.combined_sendv_data, "{\"jsonrpc\":\"2.0\"}") == 0);

    mcp_transport_destroy(NULL, t);
}

static void test_sendv_fallback_concatenation(void) {
    mock_backend_t backend;
    memset(&backend, 0, sizeof(backend));

    mcp_transport_ops_t ops = {
        .start = mock_start,
        .send = mock_send,
        .sendv = NULL, /* Fallback should be triggered */
        .recv = mock_recv,
        .stop = mock_stop,
    };

    mcp_transport_t *t = mcp_transport_create(NULL, &ops, &backend);
    assert(t != NULL);

    mcp_iovec_t iov[3] = {
        {.iov_base = "hello", .iov_len = 5},
        {.iov_base = " ", .iov_len = 1},
        {.iov_base = "world", .iov_len = 5},
    };

    mcp_status_t st = mcp_transport_sendv(NULL, t, iov, 3);
    assert(st == MCP_OK);
    (void)st;
    assert(backend.sendv_called == 0);
    assert(backend.send_called == 1);
    assert(backend.last_send_len == 11);
    assert(strcmp(backend.last_send_data, "hello world") == 0);

    mcp_transport_destroy(NULL, t);
}

static void test_sendv_validation(void) {
    mock_backend_t backend;
    memset(&backend, 0, sizeof(backend));

    mcp_transport_ops_t ops = {
        .start = mock_start,
        .send = mock_send,
        .sendv = mock_sendv,
        .recv = mock_recv,
        .stop = mock_stop,
    };

    mcp_transport_t *t = mcp_transport_create(NULL, &ops, &backend);
    assert(t != NULL);

    mcp_iovec_t iov[1] = {
        {.iov_base = "test", .iov_len = 4},
    };

    assert(mcp_transport_sendv(NULL, NULL, iov, 1) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_transport_sendv(NULL, t, NULL, 1) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_transport_sendv(NULL, t, iov, 0) == MCP_ERR_INVALID_ARGUMENT);

    mcp_transport_destroy(NULL, t);
}

static void test_socket_sendv_roundtrip(void) {
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);
    (void)rc;

    mcp_transport_t *t1 = mcp_socket_transport_from_fd(NULL, sv[0]);
    mcp_transport_t *t2 = mcp_socket_transport_from_fd(NULL, sv[1]);
    assert(t1 != NULL);
    assert(t2 != NULL);

    mcp_iovec_t iov[3] = {
        {.iov_base = "{\"method\":", .iov_len = 10},
        {.iov_base = "\"ping\",", .iov_len = 7},
        {.iov_base = "\"id\":1}", .iov_len = 7},
    };

    mcp_status_t st = mcp_transport_sendv(NULL, t1, iov, 3);
    assert(st == MCP_OK);

    char *line = NULL;
    st = mcp_transport_recv(NULL, t2, &line);
    assert(st == MCP_OK);
    assert(line != NULL);
    assert(strcmp(line, "{\"method\":\"ping\",\"id\":1}") == 0);
    free(line);

    /* Also verify sock_send works with single vector */
    st = mcp_transport_send(NULL, t2, "{\"result\":\"pong\"}", 17);
    assert(st == MCP_OK);

    line = NULL;
    st = mcp_transport_recv(NULL, t1, &line);
    assert(st == MCP_OK);
    assert(line != NULL);
    assert(strcmp(line, "{\"result\":\"pong\"}") == 0);
    free(line);

    mcp_transport_destroy(NULL, t1);
    mcp_transport_destroy(NULL, t2);
}

int main(void) {
    test_sendv_ops_invoked();
    test_sendv_fallback_concatenation();
    test_sendv_validation();
    test_socket_sendv_roundtrip();
    printf("test_transport_sendv: all tests passed\n");
    return 0;
}
