/**
 * @file ws.c
 * @brief RFC 6455 WebSocket protocol frame codec and transport.
 */

#include "mcpkit/transport/ws.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"
#include "../core/sha1.h"

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static void *ws_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void ws_free(mcp_context_t *ctx, void *p) {
    if (p == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

mcp_status_t mcp_ws_parse_frame(mcp_context_t *ctx, const uint8_t *buf, size_t len,
                                mcp_ws_frame_t *frame_out, size_t *consumed_out) {
    if (buf == NULL || frame_out == NULL || consumed_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *consumed_out = 0;
    memset(frame_out, 0, sizeof(*frame_out));

    if (len < 2) {
        return MCP_OK; /* Need more data */
    }

    uint8_t b0 = buf[0];
    uint8_t b1 = buf[1];

    bool fin = (b0 & 0x80) != 0;
    uint8_t rsv = (b0 & 0x70);
    if (rsv != 0) {
        return MCP_ERR_PROTOCOL;
    }

    uint8_t opcode_raw = (b0 & 0x0F);
    mcp_ws_opcode_t opcode;
    switch (opcode_raw) {
        case MCP_WS_OP_CONTINUATION:
        case MCP_WS_OP_TEXT:
        case MCP_WS_OP_BINARY:
        case MCP_WS_OP_CLOSE:
        case MCP_WS_OP_PING:
        case MCP_WS_OP_PONG:
            opcode = (mcp_ws_opcode_t)opcode_raw;
            break;
        default:
            return MCP_ERR_PROTOCOL;
    }

    bool is_control = (opcode_raw & 0x08) != 0;
    if (is_control && !fin) {
        /* Control frames cannot be fragmented */
        return MCP_ERR_PROTOCOL;
    }

    bool masked = (b1 & 0x80) != 0;
    uint64_t payload_len = (b1 & 0x7F);

    if (is_control && payload_len > 125) {
        /* Control frames cannot have payload > 125 */
        return MCP_ERR_PROTOCOL;
    }

    size_t header_len = 2;
    if (payload_len == 126) {
        header_len += 2;
        if (len < header_len) {
            return MCP_OK;
        }
        payload_len = ((uint64_t)buf[2] << 8) | buf[3];
    } else if (payload_len == 127) {
        header_len += 8;
        if (len < header_len) {
            return MCP_OK;
        }
        if ((buf[2] & 0x80) != 0) {
            /* RFC 6455 5.2: Most significant bit must be zero */
            return MCP_ERR_PROTOCOL;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | buf[2 + i];
        }
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (len < header_len + 4) {
            return MCP_OK;
        }
        memcpy(mask_key, buf + header_len, 4);
        header_len += 4;
    }

    if (len < header_len + payload_len) {
        return MCP_OK; /* Payload incomplete */
    }

    char *payload_copy = NULL;
    if (payload_len > 0) {
        payload_copy = ws_malloc(ctx, (size_t)payload_len + 1);
        if (payload_copy == NULL) {
            return MCP_ERR_NOMEM;
        }
        const uint8_t *src = buf + header_len;
        if (masked) {
            for (size_t i = 0; i < (size_t)payload_len; i++) {
                payload_copy[i] = (char)(src[i] ^ mask_key[i % 4]);
            }
        } else {
            memcpy(payload_copy, src, (size_t)payload_len);
        }
        payload_copy[payload_len] = '\0';
    }

    frame_out->fin = fin;
    frame_out->opcode = opcode;
    frame_out->masked = masked;
    memcpy(frame_out->mask_key, mask_key, 4);
    frame_out->payload_len = payload_len;
    frame_out->payload = payload_copy;

    *consumed_out = header_len + (size_t)payload_len;
    return MCP_OK;
}

mcp_status_t mcp_ws_encode_frame(mcp_context_t *ctx, const mcp_ws_frame_t *frame,
                                 uint8_t **bytes_out, size_t *len_out) {
    if (frame == NULL || bytes_out == NULL || len_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *bytes_out = NULL;
    *len_out = 0;

    size_t hdr_len = 2;
    if (frame->payload_len <= 125) {
        hdr_len += 0;
    } else if (frame->payload_len <= 0xFFFF) {
        hdr_len += 2;
    } else {
        hdr_len += 8;
    }

    if (frame->masked) {
        hdr_len += 4;
    }

    size_t total_len = hdr_len + (size_t)frame->payload_len;
    uint8_t *out = ws_malloc(ctx, total_len + 1);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }

    out[0] = (frame->fin ? 0x80 : 0x00) | ((uint8_t)frame->opcode & 0x0F);

    size_t idx = 1;
    uint8_t mask_bit = frame->masked ? 0x80 : 0x00;

    if (frame->payload_len <= 125) {
        out[idx++] = mask_bit | (uint8_t)frame->payload_len;
    } else if (frame->payload_len <= 0xFFFF) {
        out[idx++] = mask_bit | 126;
        out[idx++] = (uint8_t)((frame->payload_len >> 8) & 0xFF);
        out[idx++] = (uint8_t)(frame->payload_len & 0xFF);
    } else {
        out[idx++] = mask_bit | 127;
        for (int i = 7; i >= 0; i--) {
            out[idx++] = (uint8_t)((frame->payload_len >> (i * 8)) & 0xFF);
        }
    }

    if (frame->masked) {
        memcpy(out + idx, frame->mask_key, 4);
        idx += 4;
        if (frame->payload_len > 0 && frame->payload != NULL) {
            for (size_t i = 0; i < (size_t)frame->payload_len; i++) {
                out[idx + i] = (uint8_t)(frame->payload[i] ^ frame->mask_key[i % 4]);
            }
        }
    } else {
        if (frame->payload_len > 0 && frame->payload != NULL) {
            memcpy(out + idx, frame->payload, (size_t)frame->payload_len);
        }
    }
    out[total_len] = 0;

    *bytes_out = out;
    *len_out = total_len;
    return MCP_OK;
}

void mcp_ws_frame_destroy(mcp_context_t *ctx, mcp_ws_frame_t *frame) {
    if (frame == NULL) {
        return;
    }
    if (frame->payload != NULL) {
        ws_free(ctx, frame->payload);
        frame->payload = NULL;
    }
    frame->payload_len = 0;
}
