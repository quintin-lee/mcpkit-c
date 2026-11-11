/**
 * @file test_base64_sep2243.c
 * @brief P4-a T0: std base64 codec (RFC 4648 §4) + SEP-2243 policy gate.
 *
 * Core-library test, NOT HTTP-gated. Covers:
 *  - mcp_base64_encode/decode roundtrip (binary bytes incl. \x00/\xff)
 *  - mcp_base64_decode rejects non-base64 chars and bad padding
 *  - mcp_proto_policy_sep2243_active true at 2026-07-28, false at 2025-11-25/2025-06-18
 *  - MCP_RPC_XMCP_PARAM_MISMATCH is a distinct code from MCP_RPC_HEADER_MISMATCH
 */

#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/core/auth.h"
#include "mcpkit/protocol/policy.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/session.h"
#include "mcpkit/mcpkit.h"

/* RFC 4648 §4 test vectors (std base64, padded) */
static void test_rfc4648_vectors(mcp_context_t *ctx) {
    /* empty -> "" */
    char *enc = mcp_base64_encode(ctx, (const uint8_t *)"", 0);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "") == 0);
    mcp_oauth_free_string(ctx, enc);

    /* "f" -> "Zg==" */
    enc = mcp_base64_encode(ctx, (const uint8_t *)"f", 1);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "Zg==") == 0);
    mcp_oauth_free_string(ctx, enc);

    /* "fo" -> "Zm8=" */
    enc = mcp_base64_encode(ctx, (const uint8_t *)"fo", 2);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "Zm8=") == 0);
    mcp_oauth_free_string(ctx, enc);

    /* "foo" -> "Zm9v" */
    enc = mcp_base64_encode(ctx, (const uint8_t *)"foo", 3);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "Zm9v") == 0);
    mcp_oauth_free_string(ctx, enc);

    /* "foob" -> "Zm9vYg==" */
    enc = mcp_base64_encode(ctx, (const uint8_t *)"foob", 4);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "Zm9vYg==") == 0);
    mcp_oauth_free_string(ctx, enc);

    /* binary bytes incl. \x00 / \xff (3 bytes -> 4 chars, no padding) */
    uint8_t bin[3] = {0x00, 0xFF, 0x10};
    enc = mcp_base64_encode(ctx, bin, 3);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "AP8Q") == 0);
    mcp_oauth_free_string(ctx, enc);
}

static void test_roundtrip(mcp_context_t *ctx) {
    uint8_t in[64];
    for (int i = 0; i < 64; i++) in[i] = (uint8_t)i;
    char *enc = mcp_base64_encode(ctx, in, 64);
    CHECK(enc != NULL);
    uint8_t *dec = NULL;
    size_t dec_len = 0;
    int rc = mcp_base64_decode(ctx, enc, strlen(enc), &dec, &dec_len);
    CHECK(rc == 0);
    CHECK(dec != NULL);
    CHECK(dec_len == 64);
    CHECK(memcmp(dec, in, 64) == 0);
    free(dec);
    mcp_oauth_free_string(ctx, enc);
}

static void test_decode_rejects(mcp_context_t *ctx) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    /* invalid char (whitespace) in the middle */
    CHECK(mcp_base64_decode(ctx, "a b", 3, &out, &out_len) != 0);
    /* invalid char '#' */
    CHECK(mcp_base64_decode(ctx, "a#b", 3, &out, &out_len) != 0);
    /* "Zg==" (2 data chars + 2 pad) is valid base64 -> 1 byte 'f'=0x66 */
    out = NULL; out_len = 0;
    CHECK(mcp_base64_decode(ctx, "Zg==", 4, &out, &out_len) == 0);
    CHECK(out_len == 1);
    if (out) { CHECK(out[0] == 0x66); free(out); out = NULL; }
    /* "Z" (1 data char, n%4==1) is invalid base64 */
    CHECK(mcp_base64_decode(ctx, "Z", 1, &out, &out_len) != 0);
    /* empty string encodes to empty; decodes to 0 bytes */
    char *enc = mcp_base64_encode(ctx, (const uint8_t *)"", 0);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, "") == 0);
    mcp_oauth_free_string(ctx, enc);
    out = NULL; out_len = 0;
    CHECK(mcp_base64_decode(ctx, "", 0, &out, &out_len) == 0);
    CHECK(out_len == 0);
    if (out) { free(out); out = NULL; }
}

static void test_policy_gate(mcp_context_t *ctx) {
    /* 2026-07-28 -> active */
    mcp_server_t *srv = mcp_server_create(ctx, "p4t0b", "0.1.0");
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    mcp_session_set_negotiated(ctx, s, MCP_PROTO_2026_07_28);
    CHECK(mcp_proto_policy_sep2243_active(ctx, s) == true);

    /* 2025-11-25 -> NOT active (SEP-2243 custom headers only at 2026) */
    mcp_session_set_negotiated(ctx, s, MCP_PROTO_2025_11_25);
    CHECK(mcp_proto_policy_sep2243_active(ctx, s) == false);

    /* 2025-06-18 -> NOT active */
    mcp_session_set_negotiated(ctx, s, MCP_PROTO_2025_06_18);
    CHECK(mcp_proto_policy_sep2243_active(ctx, s) == false);

    /* NULL session -> conservative: negotiated baseline (2024-11-05), not active */
    CHECK(mcp_proto_policy_sep2243_active(ctx, NULL) == false);

    mcp_server_destroy(ctx, srv);
}

static void test_error_code_distinct(void) {
    /* -32001 must be a distinct value from -32020 (P2 standard header code) */
    CHECK((int)MCP_RPC_XMCP_PARAM_MISMATCH == -32001);
    CHECK((int)MCP_RPC_HEADER_MISMATCH == -32020);
    CHECK((int)MCP_RPC_XMCP_PARAM_MISMATCH != (int)MCP_RPC_HEADER_MISMATCH);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    test_rfc4648_vectors(ctx);
    test_roundtrip(ctx);
    test_decode_rejects(ctx);
    test_policy_gate(ctx);
    test_error_code_distinct();

    mcp_context_destroy(ctx);
    printf("test_base64_sep2243: OK\n");
    return 0;
}
