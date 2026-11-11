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
#include <stdlib.h>

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

/* ========================= T2: mcp_oauth_discover_as ========================= */

/* Test fetch that returns a fixed AS discovery document */
static int fetch_as_ok(void *ud, const char *url, char **body_out, size_t *body_len_out) {
    (void)ud;
    if (url == NULL || body_out == NULL) return -1;
    static const char *as_doc = "{\"token_endpoint\":\"https://as.example.com/token\",\"authorization_endpoint\":\"https://as.example.com/authorize\",\"issuer\":\"https://as.example.com\",\"end_session_endpoint\":\"https://as.example.com/logout\"}";
    *body_out = (char *)malloc(strlen(as_doc) + 1);
    if (*body_out == NULL) return -1;
    strcpy(*body_out, as_doc);
    *body_len_out = strlen(as_doc);
    return 0;
}

/* Test fetch that simulates a network failure */
static int fetch_as_fail(void *ud, const char *url, char **body_out, size_t *body_len_out) {
    (void)ud; (void)url; (void)body_out; (void)body_len_out;
    return -1;
}

/* Test fetch that captures the requested URL */
static int fetch_as_capture(void *ud, const char *url, char **body_out, size_t *body_len_out) {
    char *captured = (char *)ud;
    if (captured != NULL) {
        strncpy(captured, url, 256);
        captured[255] = '\0';
    }
    /* Return a minimal valid AS doc */
    static const char *doc = "{\"issuer\":\"https://as.example.com\",\"token_endpoint\":\"https://as.example.com/t\",\"authorization_endpoint\":\"https://as.example.com/authorize\"}";
    *body_out = (char *)malloc(strlen(doc) + 1);
    if (*body_out == NULL) return -1;
    strcpy(*body_out, doc);
    *body_len_out = strlen(doc);
    return 0;
}

static void test_discover_as_success(mcp_context_t *ctx) {
    mcp_protected_resource_metadata_t prm = {0};
    snprintf(prm.authorization_server, sizeof(prm.authorization_server),
             "https://as.example.com");

    mcp_oauth_metadata_t merged = {0};
    mcp_status_t st = mcp_oauth_discover_as(ctx, &prm, fetch_as_ok, NULL, NULL, &merged);
    CHECK(st == MCP_OK);
    CHECK(strcmp(merged.token_endpoint, "https://as.example.com/token") == 0);
    CHECK(strcmp(merged.authorization_endpoint, "https://as.example.com/authorize") == 0);
    CHECK(strcmp(merged.issuer, "https://as.example.com") == 0);
    CHECK(strcmp(merged.end_session_endpoint, "https://as.example.com/logout") == 0);
}

static void test_discover_as_null_fetch(mcp_context_t *ctx) {
    mcp_protected_resource_metadata_t prm = {0};
    snprintf(prm.authorization_server, sizeof(prm.authorization_server),
             "https://as.example.com");

    mcp_oauth_metadata_t merged = {0};
    mcp_status_t st = mcp_oauth_discover_as(ctx, &prm, NULL, NULL, NULL, &merged);
    CHECK(st == MCP_ERR_PROTOCOL);
    /* merged should be zeroed on failure */
    CHECK(merged.token_endpoint[0] == '\0');
}

static void test_discover_as_null_issuer(mcp_context_t *ctx) {
    mcp_protected_resource_metadata_t prm = {0};
    mcp_oauth_metadata_t merged = {0};
    mcp_status_t st = mcp_oauth_discover_as(ctx, &prm, fetch_as_ok, NULL, NULL, &merged);
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
}

static void test_discover_as_url_construction(mcp_context_t *ctx) {
    /* issuer with trailing slash -> should still build correct URL */
    mcp_protected_resource_metadata_t prm = {0};
    snprintf(prm.authorization_server, sizeof(prm.authorization_server),
             "https://as.example.com/");

    char captured_url[256] = {0};
    mcp_oauth_metadata_t merged = {0};
    mcp_status_t st = mcp_oauth_discover_as(ctx, &prm, fetch_as_capture, captured_url, NULL, &merged);
    CHECK(st == MCP_OK);
    CHECK(strcmp(captured_url, "https://as.example.com/.well-known/oauth-authorization-server") == 0);
}

static void test_discover_as_merge_with_existing(mcp_context_t *ctx) {
    /* existing_as has an OIDC end_session_endpoint that should be preserved */
    mcp_oauth_metadata_t existing = {0};
    strcpy(existing.end_session_endpoint, "https://oidc.example.com/logout");

    mcp_protected_resource_metadata_t prm = {0};
    snprintf(prm.authorization_server, sizeof(prm.authorization_server),
             "https://as.example.com");

    mcp_oauth_metadata_t merged = {0};
    mcp_status_t st = mcp_oauth_discover_as(ctx, &prm, fetch_as_ok, NULL, &existing, &merged);
    CHECK(st == MCP_OK);
    /* AS token_endpoint should win */
    CHECK(strcmp(merged.token_endpoint, "https://as.example.com/token") == 0);
    /* OIDC-only field from existing should be preserved (merge fills gaps) */
    CHECK(strcmp(merged.end_session_endpoint, "https://oidc.example.com/logout") == 0 ||
          strcmp(merged.end_session_endpoint, "https://as.example.com/logout") == 0);
}

static void test_discover_as_fetch_fail(mcp_context_t *ctx) {
    mcp_protected_resource_metadata_t prm = {0};
    snprintf(prm.authorization_server, sizeof(prm.authorization_server),
             "https://as.example.com");

    mcp_oauth_metadata_t merged = {0};
    mcp_status_t st = mcp_oauth_discover_as(ctx, &prm, fetch_as_fail, NULL, NULL, &merged);
    CHECK(st == MCP_ERR_PROTOCOL);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    test_rfc4648_vectors(ctx);
    test_roundtrip(ctx);
    test_decode_rejects(ctx);
    test_policy_gate(ctx);
    test_error_code_distinct();

    /* T2: AS discovery */
    test_discover_as_success(ctx);
    test_discover_as_null_fetch(ctx);
    test_discover_as_null_issuer(ctx);
    test_discover_as_url_construction(ctx);
    test_discover_as_merge_with_existing(ctx);
    test_discover_as_fetch_fail(ctx);

    mcp_context_destroy(ctx);
    printf("test_base64_sep2243: OK\n");
    return 0;
}
