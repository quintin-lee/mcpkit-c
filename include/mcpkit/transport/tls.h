/**
 * @file tls.h
 * @brief OpenSSL-backed TLS line transport.
 * @ingroup mcpkit-transport
 *
 * Wraps a TCP socket in TLS.  The TLS handshake is deferred to
 * mcp_transport_start() for both client and server modes, so a
 * single-threaded host can create both sides and then start each
 * (the server's start() typically runs in a thread).  After a
 * successful start(), send/recv use the TLS channel; a second
 * start() call is a no-op.
 *
 * Line framing, 4 MB cap, and deadline semantics match socket.c.
 *
 * Server: supply cert + key PEM file paths; port 0 = ephemeral.
 * Client: host = "127.0.0.1" or IP; cert/key = NULL (no peer
 * certificate verification by default — suitable for loopback tests,
 * NOT for production).
 *
 * Built only when MCPKIT_BUILD_TLS=ON; links OpenSSL PRIVATE to
 * mcpkit_core so the default zero-dep build is unchanged.
 */

#ifndef MCPKIT_TRANSPORT_TLS_H
#define MCPKIT_TRANSPORT_TLS_H

#include <stdint.h>
#include <stdbool.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

/**
 * @brief Creates a TLS-encrypted transport.
 *
 * In both modes the TCP socket is opened immediately and the TLS
 * handshake is deferred to mcp_transport_start(): the server runs
 * SSL_accept on the accepted connection; the client runs SSL_connect.
 * This allows a single-threaded host to create both sides before
 * starting either (typically the server's start() runs in a thread).
 *
 * @param ctx          Context; may be NULL (default allocator).
 * @param host        Hostname or IP to connect to (client mode);
 *                    ignored in server mode.  May be NULL.
 * @param port        Port number.  0 in server mode = ephemeral.
 * @param server_mode true = server (bind+listen+SSL_accept in start);
 *                    false = client (connect+SSL_connect in start).
 * @param cert        PEM certificate file path (server); NULL for client.
 * @param key         PEM private-key file path (server); NULL for client.
 * @return Owned mcp_transport_t, or NULL on failure (system call,
 *         cert load, or OOM).  Call mcp_transport_start() before
 *         using send/recv.
 */
mcp_transport_t *mcp_tls_transport_create(mcp_context_t *ctx,
                                          const char *host,
                                          uint16_t port,
                                          bool server_mode,
                                          const char *cert,
                                          const char *key);

#endif /* MCPKIT_TRANSPORT_TLS_H */
