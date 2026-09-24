#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/core/auth.h"
#include "mcpkit/mcpkit.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* 1. RFC 7636 Appendix B Official Test Vector */
    const char *rfc_verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const char *rfc_challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    char computed_challenge[44];
    memset(computed_challenge, 0, sizeof(computed_challenge));

    CHECK(mcp_pkce_compute_challenge(rfc_verifier, computed_challenge) == MCP_OK);
    CHECK(strcmp(computed_challenge, rfc_challenge) == 0);

    /* 2. Validation / edge cases */
    CHECK(mcp_pkce_compute_challenge(NULL, computed_challenge) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_pkce_compute_challenge(rfc_verifier, NULL) == MCP_ERR_INVALID_ARGUMENT);

    /* Length < 43 */
    CHECK(mcp_pkce_compute_challenge("too_short_verifier", computed_challenge) == MCP_ERR_INVALID_ARGUMENT);

    /* Invalid characters (e.g. spaces or plus) */
    char invalid_chars[50];
    memset(invalid_chars, 'a', 45);
    invalid_chars[45] = '\0';
    invalid_chars[10] = ' ';
    CHECK(mcp_pkce_compute_challenge(invalid_chars, computed_challenge) == MCP_ERR_INVALID_ARGUMENT);

    /* Length > 128 */
    char too_long[140];
    memset(too_long, 'a', 135);
    too_long[135] = '\0';
    CHECK(mcp_pkce_compute_challenge(too_long, computed_challenge) == MCP_ERR_INVALID_ARGUMENT);

    /* 3. Random PKCE Generation */
    char gen_verifier[129];
    char gen_challenge[44];
    CHECK(mcp_pkce_generate(ctx, gen_verifier, gen_challenge) == MCP_OK);
    CHECK(strlen(gen_verifier) == 64);
    CHECK(strlen(gen_challenge) == 43);

    /* Verify that recomputing challenge on generated verifier matches */
    char verify_challenge[44];
    CHECK(mcp_pkce_compute_challenge(gen_verifier, verify_challenge) == MCP_OK);
    CHECK(strcmp(gen_challenge, verify_challenge) == 0);

    /* 4. OAuth 2.1 Metadata parsing */
    const char *meta_json_str =
        "{"
        "  \"issuer\": \"https://auth.example.com\","
        "  \"authorization_endpoint\": \"https://auth.example.com/oauth/authorize\","
        "  \"token_endpoint\": \"https://auth.example.com/oauth/token\","
        "  \"registration_endpoint\": \"https://auth.example.com/oauth/register\","
        "  \"jwks_uri\": \"https://auth.example.com/.well-known/jwks.json\","
        "  \"code_challenge_methods_supported\": [\"S256\", \"plain\"]"
        "}";

    mcp_json_value_t *meta_v = mcp_json_parse(ctx, meta_json_str, strlen(meta_json_str));
    CHECK(meta_v != NULL);

    mcp_oauth_metadata_t meta;
    CHECK(mcp_oauth_metadata_parse(ctx, meta_v, &meta) == MCP_OK);
    CHECK(strcmp(meta.issuer, "https://auth.example.com") == 0);
    CHECK(strcmp(meta.authorization_endpoint, "https://auth.example.com/oauth/authorize") == 0);
    CHECK(strcmp(meta.token_endpoint, "https://auth.example.com/oauth/token") == 0);
    CHECK(strcmp(meta.registration_endpoint, "https://auth.example.com/oauth/register") == 0);
    CHECK(strcmp(meta.jwks_uri, "https://auth.example.com/.well-known/jwks.json") == 0);
    CHECK(meta.supports_pkce_s256 == true);

    mcp_json_destroy(ctx, meta_v);

    /* Missing required token_endpoint returns MCP_ERR_PROTOCOL */
    const char *incomplete_json =
        "{"
        "  \"issuer\": \"https://auth.example.com\","
        "  \"authorization_endpoint\": \"https://auth.example.com/oauth/authorize\""
        "}";
    mcp_json_value_t *inc_v = mcp_json_parse(ctx, incomplete_json, strlen(incomplete_json));
    CHECK(inc_v != NULL);
    CHECK(mcp_oauth_metadata_parse(ctx, inc_v, &meta) == MCP_ERR_PROTOCOL);
    mcp_json_destroy(ctx, inc_v);

    /* NULL / Invalid arguments */
    CHECK(mcp_oauth_metadata_parse(ctx, NULL, &meta) == MCP_ERR_INVALID_ARGUMENT);

    mcp_context_destroy(ctx);
    printf("test_auth_pkce OK\n");
    return 0;
}
