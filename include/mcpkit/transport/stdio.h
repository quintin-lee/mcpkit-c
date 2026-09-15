#ifndef MCPKIT_TRANSPORT_STDIO_H
#define MCPKIT_TRANSPORT_STDIO_H

#include <stdio.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_server mcp_server_t;

mcp_transport_t *mcp_stdio_transport_create(mcp_context_t *ctx, FILE *in, FILE *out);
mcp_status_t mcp_stdio_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t);

#endif
