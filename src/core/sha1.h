/**
 * @file sha1.h
 * @brief Compact native RFC 3174 SHA-1 and WebSocket accept key generator.
 */

#ifndef MCPKIT_CORE_SHA1_H
#define MCPKIT_CORE_SHA1_H

#include <stddef.h>
#include <stdint.h>
#include "mcpkit/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Computes 20-byte SHA-1 digest for arbitrary input buffer.
 */
void mcp_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

/**
 * @brief Computes base64-encoded Sec-WebSocket-Accept string from client key.
 *
 * Concatenates client_key with "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
 * computes SHA-1, and formats as standard RFC 4648 Base64 (28 chars + NUL).
 *
 * @param client_key Input key string (from Sec-WebSocket-Key header).
 * @param accept_buf Output buffer; must be at least 32 bytes.
 * @return MCP_OK on success, MCP_ERR_INVALID_ARGUMENT on NULL input.
 */
mcp_status_t mcp_ws_compute_accept_key(const char *client_key, char accept_buf[32]);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_SHA1_H */
