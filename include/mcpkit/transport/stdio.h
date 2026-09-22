/**
 * @file stdio.h
 * @brief stdio-backed transport: reads lines from a FILE* stream.
 * @ingroup mcpkit-transport
 *
 * - in may be NULL (no reads); out may be NULL (no writes); at least
 *   one must be non-NULL or create returns NULL.
 * - Larger-than-4 MB lines are discarded to end-of-line and MCP_ERR_PROTOCOL
 *   is returned; the stream is realigned so subsequent recv() calls resume
 *   normal line framing.
 * - mcp_stdio_serve() runs a synchronous serve loop on stdin/stdout;
 *   it creates its own session internally, so a multi-session server
 *   should use the dispatch/queue pattern instead.
 * - Signal safety: EINTR on reads is retried internally, but SIGPIPE
 *   disposition is host policy — the library never installs handlers.
 *   A host that writes to a pipe/socket-backed FILE* must ignore
 *   SIGPIPE itself (signal(SIGPIPE, SIG_IGN)) so a closed peer
 *   surfaces as EPIPE/MCP_ERR_IO instead of killing the process.
 */

#ifndef MCPKIT_TRANSPORT_STDIO_H
#define MCPKIT_TRANSPORT_STDIO_H

#include <stdio.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_client mcp_client_t;

/**
 * @brief Creates a stdio transport over two FILE* handles.
 *
 * The FILE* handles are borrowed; the transport does not close them.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param in FILE* to read from; may be NULL for write-only.
 * @param out FILE* to write to; may be NULL for read-only.
 * @return Owned mcp_transport_t, or NULL if both in and out are NULL,
 *         or on OOM.
 */
mcp_transport_t *mcp_stdio_transport_create(mcp_context_t *ctx, FILE *in, FILE *out);

/**
 * @brief Runs a synchronous serve loop until EOF or an unrecoverable
 *        error.
 *
 * Creates and destroys its own session; the caller must NOT call
 * mcp_transport_start/stop (the serve function manages the transport
 * lifecycle internally).
 *
 * Shutdown: if mcp_request_shutdown() was called (e.g. from the
 * host's own SIGTERM/SIGINT handler — the library installs none),
 * the in-flight request runs to completion and the loop returns
 * MCP_ERR_CANCELLED. A finite recv timeout lets an idle loop wake
 * up promptly to observe the flag.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server to dispatch to.
 * @param t Transport created by mcp_stdio_transport_create.
 * @return MCP_OK on clean EOF; MCP_ERR_IO on I/O failure;
 *         MCP_ERR_CANCELLED on requested shutdown.
 */
mcp_status_t mcp_stdio_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t);

/**
 * @brief Runs a synchronous serve loop that also routes server-originated
 *        requests (roots/list, sampling/createMessage, elicitation/create)
 *        to an optional client.
 *
 * Behaves identically to mcp_stdio_serve when client is NULL.  When
 * client is non-NULL, incoming REQUEST messages whose method is one of
 * the three server-to-client methods are routed to
 * mcp_client_handle_server_request instead of mcp_server_dispatch; the
 * response is serialized and sent back over the transport.  All other
 * methods still go to mcp_server_dispatch.
 *
 * The client's provider callbacks (mcp_client_set_roots_provider, etc.)
 * determine what is returned; if no provider is registered the client
 * replies -32601.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server to dispatch to.
 * @param client Optional client for server-originated requests; NULL
 *               disables that routing (same as mcp_stdio_serve).
 * @param t Transport created by mcp_stdio_transport_create.
 * @return MCP_OK on clean EOF; MCP_ERR_IO on I/O failure;
 *         MCP_ERR_CANCELLED on requested shutdown.
 */
mcp_status_t mcp_stdio_serve_with_client(mcp_context_t *ctx, mcp_server_t *server,
                                         mcp_client_t *client, mcp_transport_t *t);

#endif
