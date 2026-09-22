/**
 * @file tls.h
 * @brief OpenSSL-backed TLS line transport.
 * @ingroup mcpkit-transport
 *
 * Wraps a TCP socket in TLS. The handshake completes inside
 * mcp_tls_transport_create(); mcp_transport_start() is a no-op.
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
 * In server mode the socket is bound and a TLS listener is set up;
 * the client's TLS handshake is performed inside start().  In client
 * mode the socket is connected and the TLS handshake is performed
 * immediately in create(); start() is a no-op.
 *
 * @param ctx          Context; may be NULL (default allocator).
 * @param host        Hostname or IP to connect to (client mode);
 *                    ignored in server mode.  May be NULL.
 * @param port        Port number.  0 in server mode = ephemeral.
 * @param server_mode true = server (bind+listen+SSL_accept);
 *                    false = client (connect+SSL_connect immediately).
 * @param cert        PEM certificate file path (server); NULL for client.
 * @param key         PEM private-key file path (server); NULL for client.
 * @return Owned mcp_transport_t, or NULL on failure (system call,
 *         handshake, or OOM).
 */
mcp_transport_t *mcp_tls_transport_create(mcp_context_t *ctx,
                                          const char *host,
                                          uint16_t port,
                                          bool server_mode,
                                          const char *cert,
                                          const char *key);

#endif /* MCPKIT_TRANSPORT_TLS_H */
