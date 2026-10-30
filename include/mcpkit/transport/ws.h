/**
 * @file ws.h
 * @brief RFC 6455 WebSocket protocol frame codec and transport.
 * @ingroup mcpkit-transport
 */

#ifndef MCPKIT_TRANSPORT_WS_H
#define MCPKIT_TRANSPORT_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/transport/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RFC 6455 frame opcodes.
 */
typedef enum {
    MCP_WS_OP_CONTINUATION = 0x0,
    MCP_WS_OP_TEXT         = 0x1,
    MCP_WS_OP_BINARY       = 0x2,
    MCP_WS_OP_CLOSE        = 0x8,
    MCP_WS_OP_PING         = 0x9,
    MCP_WS_OP_PONG         = 0xA
} mcp_ws_opcode_t;

/**
 * @brief RFC 6455 parsed or unparsed WebSocket frame.
 */
typedef struct mcp_ws_frame {
    bool fin;
    mcp_ws_opcode_t opcode;
    bool masked;
    uint8_t mask_key[4];
    uint64_t payload_len;
    char *payload; /**< Heap-allocated on decode; caller frees or calls mcp_ws_frame_destroy */
} mcp_ws_frame_t;

/**
 * @brief Parses one WebSocket frame from buffer.
 *
 * If buffer contains a complete frame, *consumed_out is set to bytes consumed,
 * frame_out receives parsed frame with caller-owned frame_out->payload, and MCP_OK is returned.
 * If buffer is incomplete (needs more bytes), *consumed_out is set to 0, frame_out->payload = NULL,
 * and MCP_OK is returned.
 * If frame is malformed (invalid opcodes, RSV bits, protocol violations), MCP_ERR_PROTOCOL is returned.
 *
 * @param ctx Context; may be NULL.
 * @param buf Input byte buffer.
 * @param len Available bytes in buffer.
 * @param frame_out Receives parsed frame on success.
 * @param consumed_out Receives number of bytes consumed (0 if incomplete).
 * @return MCP_OK on success or incomplete, error code on protocol violation or OOM.
 */
mcp_status_t mcp_ws_parse_frame(mcp_context_t *ctx, const uint8_t *buf, size_t len,
                                mcp_ws_frame_t *frame_out, size_t *consumed_out);

/**
 * @brief Encodes a WebSocket frame into byte buffer.
 *
 * *bytes_out is allocated via ctx allocator; caller must free with mcp_json_free_string(ctx, (char*)*bytes_out).
 *
 * @param ctx Context; may be NULL.
 * @param frame Frame to serialize.
 * @param bytes_out Receives newly allocated byte buffer.
 * @param len_out Receives total bytes in encoded frame.
 * @return MCP_OK on success, error code otherwise.
 */
mcp_status_t mcp_ws_encode_frame(mcp_context_t *ctx, const mcp_ws_frame_t *frame,
                                 uint8_t **bytes_out, size_t *len_out);

/**
 * @brief Frees resources in parsed frame.
 *
 * @param ctx Context; may be NULL.
 * @param frame Frame whose payload is freed.
 */
void mcp_ws_frame_destroy(mcp_context_t *ctx, mcp_ws_frame_t *frame);

/**
 * @brief Computes base64-encoded Sec-WebSocket-Accept key from client key.
 *
 * @param client_key Input key string.
 * @param accept_buf Output buffer (must be at least 32 bytes).
 * @return MCP_OK on success, MCP_ERR_INVALID_ARGUMENT on NULL.
 */
mcp_status_t mcp_ws_compute_accept_key(const char *client_key, char accept_buf[32]);

/**
 * @brief Connects to a WebSocket endpoint and returns an mcp_transport_t.
 *
 * @param ctx Context; may be NULL.
 * @param url WebSocket URL (e.g., "ws://127.0.0.1:8080/mcp").
 * @return Owned mcp_transport_t or NULL on connection failure.
 */
mcp_transport_t *mcp_ws_client_connect(mcp_context_t *ctx, const char *url);

/**
 * @brief Wraps an already upgraded/connected socket fd into a synchronous mcp_transport_t.
 *
 * @param ctx Context; may be NULL.
 * @param fd Connected socket descriptor.
 * @param is_server True for server role (unmasks inbound, masks outbound if needed).
 * @return Owned mcp_transport_t or NULL on failure.
 */
mcp_transport_t *mcp_ws_transport_create(mcp_context_t *ctx, int fd, bool is_server);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_TRANSPORT_WS_H */
