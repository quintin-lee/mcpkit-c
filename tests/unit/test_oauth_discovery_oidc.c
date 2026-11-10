/**
 * @file test_oauth_discovery_oidc.c
 * @brief P3 block-5 (T5) test: OIDC Discovery 1.0 endpoint capture + merge.
 *
 * Red phase (fields/merge not yet present):
 *  - mcp_oauth_metadata_t has no end_session_endpoint / revocation_endpoint /
 *    introspection_endpoint members.
 *  - mcp_oauth_discovery_merge does not exist.
 *
 * Green phase:
 *  - mcp_oauth_metadata_parse populates the three new endpoint fields when
 *    present in the JSON document, leaves them empty (not "missing -> error")
 *    when absent.
 *  - mcp_oauth_discovery_merge(base, oidc, out) implements "base wins,
 *    oidc fills gaps, capability flags OR" semantics; in-place use
 *    (out==base) is safe.
 */
#include <stdio.h>
#include <string.h>
#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* 1. OIDC Discovery 1.0 document: all RFC 8414 fields + 3 OIDC endpoints */
    const char *oidc_json =
        "{"
        "\"issuer\":\"https://as.example.com\","
        "\"authorization_endpoint\":\"https://as.example.com/oauth/authorize\","
        "\"token_endpoint\":\"https://as.example.com/oauth/token\","
        "\"end_session_endpoint\":\"https://as.example.com/oauth/end-session\","
        "\"revocation_endpoint\":\"https://as.example.com/oauth/revoke\","
        "\"introspection_endpoint\":\"https://as.example.com/oauth/introspect\","
        "\"code_challenge_methods_supported\":[\"S256\"],"
        "\"client_id_metadata_document_supported\":true"
        "}";
    mcp_json_value_t *oidc_v = mcp_json_parse(ctx, oidc_json, strlen(oidc_json));
    CHECK(oidc_v != NULL);

    mcp_oauth_metadata_t oidc_meta;
    CHECK(mcp_oauth_metadata_parse(ctx, oidc_v, &oidc_meta) == MCP_OK);
    CHECK(strcmp(oidc_meta.end_session_endpoint,
                 "https://as.example.com/oauth/end-session") == 0);
    CHECK(strcmp(oidc_meta.revocation_endpoint,
                 "https://as.example.com/oauth/revoke") == 0);
    CHECK(strcmp(oidc_meta.introspection_endpoint,
                 "https://as.example.com/oauth/introspect") == 0);
    CHECK(oidc_meta.supports_pkce_s256 == true);
    CHECK(oidc_meta.client_id_metadata_document_supported == true);
    mcp_json_destroy(ctx, oidc_v);

    /* 2. Plain RFC 8414 document (no OIDC extension): new fields must stay empty */
    const char *base_json =
        "{"
        "\"issuer\":\"https://as.example.com\","
        "\"authorization_endpoint\":\"https://as.example.com/oauth/authorize\","
        "\"token_endpoint\":\"https://as.example.com/oauth/token\","
        "\"registration_endpoint\":\"https://as.example.com/oauth/register\","
        "\"jwks_uri\":\"https://as.example.com/.well-known/jwks.json\","
        "\"code_challenge_methods_supported\":[\"S256\"]"
        "}";
    mcp_json_value_t *base_v = mcp_json_parse(ctx, base_json, strlen(base_json));
    CHECK(base_v != NULL);

    mcp_oauth_metadata_t base_meta;
    CHECK(mcp_oauth_metadata_parse(ctx, base_v, &base_meta) == MCP_OK);
    CHECK(base_meta.end_session_endpoint[0] == '\0');
    CHECK(base_meta.revocation_endpoint[0] == '\0');
    CHECK(base_meta.introspection_endpoint[0] == '\0');
    CHECK(strcmp(base_meta.registration_endpoint,
                 "https://as.example.com/oauth/register") == 0);
    mcp_json_destroy(ctx, base_v);

    /* 3. Merge: base wins on non-empty fields, oidc fills gaps */
    mcp_oauth_metadata_t merged;
    CHECK(mcp_oauth_discovery_merge(ctx, &base_meta, &oidc_meta, &merged) == MCP_OK);
    /* base non-empty -> base wins */
    CHECK(strcmp(merged.issuer, "https://as.example.com") == 0);
    CHECK(strcmp(merged.authorization_endpoint, "https://as.example.com/oauth/authorize") == 0);
    CHECK(strcmp(merged.token_endpoint, "https://as.example.com/oauth/token") == 0);
    CHECK(strcmp(merged.registration_endpoint, "https://as.example.com/oauth/register") == 0);
    CHECK(strcmp(merged.jwks_uri, "https://as.example.com/.well-known/jwks.json") == 0);
    /* base empty -> oidc fills */
    CHECK(strcmp(merged.end_session_endpoint, "https://as.example.com/oauth/end-session") == 0);
    CHECK(strcmp(merged.revocation_endpoint, "https://as.example.com/oauth/revoke") == 0);
    CHECK(strcmp(merged.introspection_endpoint, "https://as.example.com/oauth/introspect") == 0);
    /* flags: OR */
    CHECK(merged.supports_pkce_s256 == true);
    CHECK(merged.client_id_metadata_document_supported == true);

    /* 4. Merge: both empty -> stays empty; flags both false -> false */
    mcp_oauth_metadata_t empty;
    memset(&empty, 0, sizeof(empty));
    /* issuer/authorization/token are required for parse, but for merge they
     * may legitimately be empty when constructed manually. */
    mcp_oauth_metadata_t both_false;
    memset(&both_false, 0, sizeof(both_false));
    mcp_oauth_metadata_t out2;
    CHECK(mcp_oauth_discovery_merge(ctx, &empty, &both_false, &out2) == MCP_OK);
    CHECK(out2.end_session_endpoint[0] == '\0');
    CHECK(out2.supports_pkce_s256 == false);
    CHECK(out2.client_id_metadata_document_supported == false);

    /* 5. Merge: oidc flag true, base flag false -> true (OR direction 2) */
    mcp_oauth_metadata_t base_flag_off;
    memset(&base_flag_off, 0, sizeof(base_flag_off));
    base_flag_off.supports_pkce_s256 = false;
    base_flag_off.client_id_metadata_document_supported = true;
    mcp_oauth_metadata_t oidc_flag_only;
    memset(&oidc_flag_only, 0, sizeof(oidc_flag_only));
    oidc_flag_only.supports_pkce_s256 = true;
    mcp_oauth_metadata_t out3;
    CHECK(mcp_oauth_discovery_merge(ctx, &base_flag_off, &oidc_flag_only, &out3) == MCP_OK);
    CHECK(out3.supports_pkce_s256 == true);
    CHECK(out3.client_id_metadata_document_supported == true);

    /* 6. In-place merge (out == base) is safe */
    mcp_oauth_metadata_t inplace;
    CHECK(mcp_oauth_discovery_merge(ctx, &base_meta, &oidc_meta, &inplace) == MCP_OK);
    CHECK(mcp_oauth_discovery_merge(ctx, &inplace, &inplace, &inplace) == MCP_OK);
    CHECK(strcmp(inplace.end_session_endpoint, "https://as.example.com/oauth/end-session") == 0);

    /* 7. NULL arguments -> INVALID_ARGUMENT */
    mcp_oauth_metadata_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    CHECK(mcp_oauth_discovery_merge(ctx, NULL, &dummy, &dummy) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_oauth_discovery_merge(ctx, &dummy, NULL, &dummy) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_oauth_discovery_merge(ctx, &dummy, &dummy, NULL) == MCP_ERR_INVALID_ARGUMENT);
    /* ctx may be NULL */
    CHECK(mcp_oauth_discovery_merge(NULL, &base_meta, &oidc_meta, &merged) == MCP_OK);

    mcp_context_destroy(ctx);
    printf("test_oauth_discovery_oidc OK\n");
    return 0;
}
