#ifndef MCPKIT_TRANSPORT_STREAMABLE_HTTP_H
#define MCPKIT_TRANSPORT_STREAMABLE_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"

/**
 * @file streamable_http.h
 * Streamable HTTP transport layer over a pluggable I/O backend.
 *
 * - mcp_http_io_t is an opaque I/O vtable; the implementation is
 *   responsible for fulfilling read/write callbacks against a real
 *   network socket or file.
 * - mcp_http_serve(): runs a synchronous HTTP serve loop. Sessions are
 *   loop-local (destroyed when the loop exits); at most
 *   MCP_HTTP_MAX_SESSIONS concurrent sessions are supported.
 * - mcp_sse_wrap(): returns an owned SSE-framed string; caller frees
 *   with mcp_json_free_string(ctx, s).
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;

typedef struct mcp_http_io mcp_http_io_t;
struct mcp_http_io {
    void *user;
    mcp_status_t (*read)(mcp_context_t *ctx, void *user, char *buf, size_t cap,
                           size_t *n_out);
    mcp_status_t (*write)(mcp_context_t *ctx, void *user, const char *data, size_t len);
};

#define MCP_HTTP_MAX_SESSIONS 16

/**
 * Runs a synchronous HTTP serve loop until the I/O backend signals EOF.
 * Sessions are destroyed when the loop exits; the caller must not use
 * them afterwards.
 */
mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server,
                             mcp_http_io_t *io);

/**
 * Wraps a JSON string in an SSE `data:` frame.
 * Returns an owned heap string; caller frees with mcp_json_free_string().
 */
char *mcp_sse_wrap(mcp_context_t *ctx, const char *json_text);

#endif
