#ifndef MCPKIT_TRANSPORT_STREAMABLE_HTTP_H
#define MCPKIT_TRANSPORT_STREAMABLE_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;

typedef struct mcp_http_io mcp_http_io_t;
struct mcp_http_io {
    void *user;
    mcp_status_t (*read)(mcp_context_t *ctx, void *user, char *buf, size_t cap, size_t *n_out);
    mcp_status_t (*write)(mcp_context_t *ctx, void *user, const char *data, size_t len);
};

#define MCP_HTTP_MAX_SESSIONS 16

mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_http_io_t *io);
char *mcp_sse_wrap(mcp_context_t *ctx, const char *json_text);

#endif
