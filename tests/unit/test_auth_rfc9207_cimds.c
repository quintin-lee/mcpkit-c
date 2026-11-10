/**
 * @file test_auth_rfc9207_cimds.c
 * @brief P3 block-1 red/green test: RFC 9207 issuer capture + MUST gate.
 *
 * Red phase (new symbols not yet present):
 *  - mcp_oauth_token_response_parse does not populate token.iss.
 *  - mcp_oauth_gate_issuer (new pure gate helper) does not exist.
 *  - mcp_proto_policy_auth_hardening_active does not exist.
 * Green phase:
 *  - iss captured on parse; missing iss accepted (SEP-2468 omits iss as OK).
 *  - gate_issuer rejects an issuer mismatch as MCP_ERR_PROTOCOL when
 *    enforce_must=true; enforces when enforce_must=false (SHOULD, returns OK
 *    and records a warning side effect observable only via log, not returned).
 *  - policy hook returns true at >=2025-11-25 and false at 2025-06-18 /
 *    2024-11-05 / NULL session (2024 baseline).
 */
#include <stdio.h>
#include <string.h>
#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "mcpkit/protocol/policy.h"
#include "mcpkit/protocol/version.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* 1. Token response parse captures "iss" (RFC 9207 / SEP-2468) */
    const char *json_with_iss =
        "{\"access_token\":\"acc-1\",\"token_type\":\"Bearer\","
        "\"expires_in\":3600,\"iss\":\"https://auth.example.com\"}";
    mcp_oauth_token_response_t tok;
    memset(&tok, 0, sizeof(tok));
    CHECK(mcp_oauth_token_response_parse(ctx, json_with_iss, strlen(json_with_iss), &tok) == MCP_OK);
    CHECK(tok.iss != NULL && strcmp(tok.iss, "https://auth.example.com") == 0);
    mcp_oauth_token_response_cleanup(ctx, &tok);

    /* 2. Token response without iss still parses; iss pointer stays NULL */
    const char *json_no_iss =
        "{\"access_token\":\"acc-2\",\"token_type\":\"Bearer\",\"expires_in\":3600}";
    memset(&tok, 0, sizeof(tok));
    CHECK(mcp_oauth_token_response_parse(ctx, json_no_iss, strlen(json_no_iss), &tok) == MCP_OK);
    CHECK(tok.iss == NULL);
    mcp_oauth_token_response_cleanup(ctx, &tok);

    /* 3. Gate helper: matching issuer -> OK */
    {
        mcp_oauth_token_response_t r;
        memset(&r, 0, sizeof(r));
        CHECK(mcp_oauth_token_response_parse(ctx, json_with_iss, strlen(json_with_iss), &r) == MCP_OK);
        CHECK(mcp_oauth_gate_issuer(ctx, &r, "https://auth.example.com", true) == MCP_OK);
        mcp_oauth_token_response_cleanup(ctx, &r);
    }

    /* 4. Gate helper: mismatching issuer -> MCP_ERR_PROTOCOL under MUST */
    {
        mcp_oauth_token_response_t r;
        memset(&r, 0, sizeof(r));
        CHECK(mcp_oauth_token_response_parse(ctx, json_with_iss, strlen(json_with_iss), &r) == MCP_OK);
        CHECK(mcp_oauth_gate_issuer(ctx, &r, "https://attacker.example.com", true) == MCP_ERR_PROTOCOL);
        mcp_oauth_token_response_cleanup(ctx, &r);
    }

    /* 5. Gate helper: missing iss -> OK (SEP-2468: iss omitted is acceptable) */
    {
        mcp_oauth_token_response_t r;
        memset(&r, 0, sizeof(r));
        CHECK(mcp_oauth_token_response_parse(ctx, json_no_iss, strlen(json_no_iss), &r) == MCP_OK);
        CHECK(mcp_oauth_gate_issuer(ctx, &r, "https://auth.example.com", true) == MCP_OK);
        mcp_oauth_token_response_cleanup(ctx, &r);
    }

    /* 6. Gate helper: NULL expected issuer -> OK (no issuer to enforce) */
    {
        mcp_oauth_token_response_t r;
        memset(&r, 0, sizeof(r));
        CHECK(mcp_oauth_token_response_parse(ctx, json_with_iss, strlen(json_with_iss), &r) == MCP_OK);
        CHECK(mcp_oauth_gate_issuer(ctx, &r, NULL, true) == MCP_OK);
        mcp_oauth_token_response_cleanup(ctx, &r);
    }

    /* 7. Gate helper: SHOULD mode (enforce_must=false) never returns error */
    {
        mcp_oauth_token_response_t r;
        memset(&r, 0, sizeof(r));
        CHECK(mcp_oauth_token_response_parse(ctx, json_with_iss, strlen(json_with_iss), &r) == MCP_OK);
        CHECK(mcp_oauth_gate_issuer(ctx, &r, "https://attacker.example.com", false) == MCP_OK);
        mcp_oauth_token_response_cleanup(ctx, &r);
    }

    /* 8. mcp_oauth_validate_issuer legacy semantics preserved */
    CHECK(mcp_oauth_validate_issuer("https://auth.example.com", "https://auth.example.com"));
    CHECK(!mcp_oauth_validate_issuer("https://auth.example.com", "https://attacker.example.com"));
    CHECK(mcp_oauth_validate_issuer("https://auth.example.com", NULL)); /* omitted -> OK */
    CHECK(!mcp_oauth_validate_issuer(NULL, "https://auth.example.com")); /* no expectation */

    /* 9. P3 policy hook: auth_hardening_active true at >=2025-11-25, false below */
    mcp_server_t *srv = mcp_server_create(ctx, "rfc9207-test", "1.0");
    CHECK(srv != NULL);

    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);
    CHECK(mcp_session_set_negotiated(ctx, s, MCP_PROTO_2025_11_25) == MCP_OK);
    CHECK(mcp_proto_policy_auth_hardening_active(ctx, s) == true);

    CHECK(mcp_session_set_negotiated(ctx, s, MCP_PROTO_2026_07_28) == MCP_OK);
    CHECK(mcp_proto_policy_auth_hardening_active(ctx, s) == true);

    CHECK(mcp_session_set_negotiated(ctx, s, MCP_PROTO_2025_06_18) == MCP_OK);
    CHECK(mcp_proto_policy_auth_hardening_active(ctx, s) == false);

    CHECK(mcp_session_set_negotiated(ctx, s, MCP_PROTO_2024_11_05) == MCP_OK);
    CHECK(mcp_proto_policy_auth_hardening_active(ctx, s) == false);

    mcp_server_destroy_session(ctx, srv, s);

    /* NULL session -> 2024 baseline -> false */
    CHECK(mcp_proto_policy_auth_hardening_active(ctx, NULL) == false);

    mcp_server_destroy(ctx, srv);

    mcp_context_destroy(ctx);
    printf("test_auth_rfc9207_cimds OK\n");
    return 0;
}
