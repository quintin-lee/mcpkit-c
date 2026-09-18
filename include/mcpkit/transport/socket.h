/**
 * @file socket.h
 * @brief POSIX TCP socket transport.
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

#ifndef MCPKIT_TRANSPORT_SOCKET_H
#define MCPKIT_TRANSPORT_SOCKET_H

#include <stdint.h>
#include <stdbool.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

/**
 * @brief Creates a TCP socket transport.
 *
 * In client mode the socket is connected immediately. In server mode
 * the socket is bound and listening; call mcp_transport_start() to
 * accept a single connection.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param host Hostname or IP to connect to (client mode); ignored in
 *            server mode (binds to 0.0.0.0). May be NULL.
 * @param port Port number. 0 in server mode = ephemeral port (use
 *             getsockname() to retrieve it).
 * @param server_mode true = server (bind+listen+accept); false = client
 *                   (connect immediately).
 * @return Owned mcp_transport_t, or NULL on system-call failure or OOM.
 */
mcp_transport_t *mcp_socket_transport_create(mcp_context_t *ctx, const char *host,
                                             uint16_t port, bool server_mode);

#endif
