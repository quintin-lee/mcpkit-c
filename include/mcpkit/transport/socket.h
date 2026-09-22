/**
 * @file socket.h
 * @brief POSIX TCP socket transport.
 * @ingroup mcpkit-transport
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
typedef struct mcp_server mcp_server_t;
typedef struct mcp_executor mcp_executor_t;

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

/**
 * @brief Accepts and serves multiple TCP connections concurrently.
 *
 * Binds and listens on 0.0.0.0:port (port 0 = ephemeral port; use
 * mcp_socket_transport_create with a separate call to retrieve the
 * actual port via getsockname() if needed).  The accept loop runs on
 * the calling thread and uses poll() with a 500 ms timeout so that
 * mcp_shutdown_requested() is observed within that window.  Each
 * accepted connection is submitted to pool as a task that runs a
 * full single-connection serve loop (create session, drain outbox,
 * recv/parse/dispatch/send, destroy session, stop/destroy transport).
 *
 * When mcp_shutdown_requested() returns true the accept loop stops
 * accepting new connections and mcp_executor_wait drains all
 * in-flight connections before the function returns.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param server Target server; must outlive all connections.
 * @param port  Port to listen on (0 = ephemeral).
 * @param pool  Caller-owned mcp_executor_t; NOT destroyed by this
 *              function.  The caller must call mcp_executor_destroy()
 *              after mcp_socket_serve returns.
 * @return MCP_OK after clean shutdown; MCP_ERR_NOMEM on accept/socket
 *         failure; MCP_ERR_INVALID_ARGUMENT on NULL server or pool.
 *
 * Thread-safety: multiple worker threads call mcp_server_dispatch
 * concurrently with independent sessions.  The library guards
 * log_floor (atomic) and subscribed_uris (mutex) inside the server;
 * the host is responsible for thread-safe tool handlers and for any
 * shared state they access.
 */
mcp_status_t mcp_socket_serve(mcp_context_t *ctx, mcp_server_t *server,
                              uint16_t port, mcp_executor_t *pool);

#endif
