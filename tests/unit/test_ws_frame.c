#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/ws.h"

static void test_ws_small_unmasked_text_frame(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_ws_frame_t frame = {
        .fin = true,
        .opcode = MCP_WS_OP_TEXT,
        .masked = false,
        .mask_key = {0},
        .payload_len = 11,
        .payload = "Hello, MCP!",
    };

    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(mcp_ws_encode_frame(ctx, &frame, &bytes, &len) == MCP_OK);
    CHECK(bytes != NULL);
    CHECK(len == 2 + 11); /* 2 byte header + 11 bytes payload */
    CHECK(bytes[0] == 0x81); /* FIN=1, opcode=0x1 */
    CHECK(bytes[1] == 11);   /* MASK=0, len=11 */
    CHECK(memcmp(bytes + 2, "Hello, MCP!", 11) == 0);

    /* Decode it back */
    mcp_ws_frame_t parsed = {0};
    size_t consumed = 0;
    CHECK(mcp_ws_parse_frame(ctx, bytes, len, &parsed, &consumed) == MCP_OK);
    CHECK(consumed == len);
    CHECK(parsed.fin == true);
    CHECK(parsed.opcode == MCP_WS_OP_TEXT);
    CHECK(parsed.masked == false);
    CHECK(parsed.payload_len == 11);
    CHECK(memcmp(parsed.payload, "Hello, MCP!", 11) == 0);

    mcp_ws_frame_destroy(ctx, &parsed);
    mcp_json_free_string(ctx, (char *)bytes);
    mcp_context_destroy(ctx);
}

static void test_ws_small_masked_text_frame(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_ws_frame_t frame = {
        .fin = true,
        .opcode = MCP_WS_OP_TEXT,
        .masked = true,
        .mask_key = {0x12, 0x34, 0x56, 0x78},
        .payload_len = 5,
        .payload = "Hello",
    };

    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(mcp_ws_encode_frame(ctx, &frame, &bytes, &len) == MCP_OK);
    CHECK(bytes != NULL);
    CHECK(len == 2 + 4 + 5); /* 2 header + 4 mask + 5 payload */
    CHECK(bytes[0] == 0x81);
    CHECK(bytes[1] == (0x80 | 5)); /* MASK=1, len=5 */
    CHECK(bytes[2] == 0x12 && bytes[3] == 0x34 && bytes[4] == 0x56 && bytes[5] == 0x78);

    /* Decode it back */
    mcp_ws_frame_t parsed = {0};
    size_t consumed = 0;
    CHECK(mcp_ws_parse_frame(ctx, bytes, len, &parsed, &consumed) == MCP_OK);
    CHECK(consumed == len);
    CHECK(parsed.fin == true);
    CHECK(parsed.opcode == MCP_WS_OP_TEXT);
    CHECK(parsed.masked == true);
    CHECK(parsed.payload_len == 5);
    CHECK(memcmp(parsed.payload, "Hello", 5) == 0);

    mcp_ws_frame_destroy(ctx, &parsed);
    mcp_json_free_string(ctx, (char *)bytes);
    mcp_context_destroy(ctx);
}

static void test_ws_medium_payload_16bit(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    char *buf = malloc(300);
    CHECK(buf != NULL);
    memset(buf, 'A', 300);

    mcp_ws_frame_t frame = {
        .fin = true,
        .opcode = MCP_WS_OP_BINARY,
        .masked = false,
        .payload_len = 300,
        .payload = buf,
    };

    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(mcp_ws_encode_frame(ctx, &frame, &bytes, &len) == MCP_OK);
    CHECK(len == 2 + 2 + 300); /* 2 base + 2 ext16 + 300 */
    CHECK(bytes[0] == 0x82);
    CHECK(bytes[1] == 126); /* 126 indicates 16-bit ext length */
    CHECK(((uint16_t)bytes[2] << 8 | bytes[3]) == 300);

    mcp_ws_frame_t parsed = {0};
    size_t consumed = 0;
    CHECK(mcp_ws_parse_frame(ctx, bytes, len, &parsed, &consumed) == MCP_OK);
    CHECK(consumed == len);
    CHECK(parsed.opcode == MCP_WS_OP_BINARY);
    CHECK(parsed.payload_len == 300);
    CHECK(memcmp(parsed.payload, buf, 300) == 0);

    mcp_ws_frame_destroy(ctx, &parsed);
    mcp_json_free_string(ctx, (char *)bytes);
    free(buf);
    mcp_context_destroy(ctx);
}

static void test_ws_control_frames(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* Ping frame */
    mcp_ws_frame_t ping = {
        .fin = true,
        .opcode = MCP_WS_OP_PING,
        .masked = false,
        .payload_len = 4,
        .payload = "ping",
    };
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(mcp_ws_encode_frame(ctx, &ping, &bytes, &len) == MCP_OK);
    CHECK(bytes[0] == 0x89); /* FIN=1, Ping=9 */

    mcp_ws_frame_t parsed = {0};
    size_t consumed = 0;
    CHECK(mcp_ws_parse_frame(ctx, bytes, len, &parsed, &consumed) == MCP_OK);
    CHECK(parsed.opcode == MCP_WS_OP_PING);
    CHECK(parsed.payload_len == 4);
    CHECK(memcmp(parsed.payload, "ping", 4) == 0);

    mcp_ws_frame_destroy(ctx, &parsed);
    mcp_json_free_string(ctx, (char *)bytes);

    /* Close frame with status code 1000 */
    uint8_t close_payload[2] = {0x03, 0xE8}; /* 1000 big endian */
    mcp_ws_frame_t close_f = {
        .fin = true,
        .opcode = MCP_WS_OP_CLOSE,
        .masked = false,
        .payload_len = 2,
        .payload = (char *)close_payload,
    };
    CHECK(mcp_ws_encode_frame(ctx, &close_f, &bytes, &len) == MCP_OK);
    CHECK(bytes[0] == 0x88); /* FIN=1, Close=8 */

    CHECK(mcp_ws_parse_frame(ctx, bytes, len, &parsed, &consumed) == MCP_OK);
    CHECK(parsed.opcode == MCP_WS_OP_CLOSE);
    CHECK(parsed.payload_len == 2);
    uint16_t code = ((uint16_t)(uint8_t)parsed.payload[0] << 8) | (uint8_t)parsed.payload[1];
    CHECK(code == 1000);

    mcp_ws_frame_destroy(ctx, &parsed);
    mcp_json_free_string(ctx, (char *)bytes);
    mcp_context_destroy(ctx);
}

static void test_ws_incomplete_and_malformed(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* Incomplete header: 1 byte */
    uint8_t incomplete1[] = {0x81};
    mcp_ws_frame_t parsed = {0};
    size_t consumed = 0;
    CHECK(mcp_ws_parse_frame(ctx, incomplete1, sizeof(incomplete1), &parsed, &consumed) == MCP_OK);
    CHECK(consumed == 0); /* Need more data */

    /* Incomplete payload: header says 10 bytes, only 3 provided */
    uint8_t incomplete2[] = {0x81, 10, 'a', 'b', 'c'};
    CHECK(mcp_ws_parse_frame(ctx, incomplete2, sizeof(incomplete2), &parsed, &consumed) == MCP_OK);
    CHECK(consumed == 0);

    /* RSV bits set -> MCP_ERR_PROTOCOL */
    uint8_t rsv_frame[] = {0xC1, 0x00}; /* RSV1 set */
    CHECK(mcp_ws_parse_frame(ctx, rsv_frame, sizeof(rsv_frame), &parsed, &consumed) == MCP_ERR_PROTOCOL);

    /* Control frame with fin=false -> MCP_ERR_PROTOCOL */
    uint8_t fragmented_ctrl[] = {0x09, 0x00}; /* fin=0 for ping */
    CHECK(mcp_ws_parse_frame(ctx, fragmented_ctrl, sizeof(fragmented_ctrl), &parsed, &consumed) == MCP_ERR_PROTOCOL);

    mcp_context_destroy(ctx);
}

int main(void) {
    test_ws_small_unmasked_text_frame();
    test_ws_small_masked_text_frame();
    test_ws_medium_payload_16bit();
    test_ws_control_frames();
    test_ws_incomplete_and_malformed();
    printf("test_ws_frame: OK\n");
    return 0;
}
