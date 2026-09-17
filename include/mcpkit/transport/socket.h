#ifndef MCPKIT_TRANSPORT_SOCKET_H
#define MCPKIT_TRANSPORT_SOCKET_H

#include <stdint.h>
#include <stdbool.h>

#include "mcpkit/core/error.h"

/**
 * @file socket.h
 * POSIX TCP socket transport.
 *
 * - Client mode (server_mode == false): connect() to host:port is
 *   performed immediately in mcp_socket_transport_create(); start() is
 *   a no-op.
 * - Server mode (server_mode == true): bind+listen on 0.0.0.0:port is
 *   performed in create(); start() calls accept() on a single
 *   connection and stores the accepted fd.
 * - Both modes use newline-framed lines with the same 4 MB cap as
 *   mcp_stdio_transport_create().
 * - Returns NULL on system-call failure or OOM.
 * - Port 0 in server mode binds to an ephemeral port; retrieve it via
 *   getsockname() on the listening socket before calling start().
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

mcp_transport_t *mcp_socket_transport_create(mcp_context_t *ctx, const char *host,
                                              uint16_t port, bool server_mode);

#endif
