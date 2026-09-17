#ifndef MCPKIT_TRANSPORT_STDIO_H
#define MCPKIT_TRANSPORT_STDIO_H

#include <stdio.h>

#include "mcpkit/core/error.h"

/**
 * @file stdio.h
 * stdio-backed transport: reads lines from a FILE* stream.
 *
 * - in may be NULL (no reads); out may be NULL (no writes); at least
 *   one must be non-NULL or create returns NULL.
 * - Larger-than-4 MB lines are discarded to end-of-line and MCP_ERR_PROTOCOL
 *   is returned; the stream is realigned so subsequent recv() calls resume
 *   normal line framing.
 * - mcp_stdio_serve() runs a synchronous serve loop on stdin/stdout;
 *   it creates its own session internally, so a multi-session server
 *   should use the dispatch/queue pattern instead.
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_server mcp_server_t;

/**
 * Creates a stdio transport. in/out are borrowed FILE* handles; the
 * transport does not close them.
 */
mcp_transport_t *mcp_stdio_transport_create(mcp_context_t *ctx, FILE *in, FILE *out);

/**
 * Serves the server over the transport until EOF or an unrecoverable
 * error. Creates and destroys its own session; the caller must NOT call
 * mcp_transport_start/stop (the serve function manages the transport
 * lifecycle internally).
 */
mcp_status_t mcp_stdio_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t);

#endif
