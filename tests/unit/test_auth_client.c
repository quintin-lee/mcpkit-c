#include <stdio.h>
#include <string.h>
#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. Parse valid token response
    const char *json_ok =
        "{\"access_token\":\"acc-123\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"ref-456\",\"scope\":\"read write\"}";
    mcp_oauth_token_response_t token;
    memset(&token, 0, sizeof(token));
    CHECK(mcp_oauth_token_response_parse(ctx, json_ok, strlen(json_ok), &token) == MCP_OK);
    CHECK(token.access_token != NULL && strcmp(token.access_token, "acc-123") == 0);
    CHECK(token.token_type != NULL && strcmp(token.token_type, "Bearer") == 0);
    CHECK(token.expires_in == 3600);
    CHECK(token.refresh_token != NULL && strcmp(token.refresh_token, "ref-456") == 0);
    CHECK(token.scope != NULL && strcmp(token.scope, "read write") == 0);
    mcp_oauth_token_response_cleanup(ctx, &token);

    // 2. Parse invalid token response (missing access_token or token_type)
    const char *json_bad1 = "{\"token_type\":\"Bearer\"}";
    memset(&token, 0, sizeof(token));
    CHECK(mcp_oauth_token_response_parse(ctx, json_bad1, strlen(json_bad1), &token) == MCP_ERR_PROTOCOL);

    const char *json_bad2 = "{\"access_token\":\"123\"}";
    memset(&token, 0, sizeof(token));
    CHECK(mcp_oauth_token_response_parse(ctx, json_bad2, strlen(json_bad2), &token) == MCP_ERR_PROTOCOL);

    CHECK(mcp_oauth_token_response_parse(ctx, NULL, 0, &token) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_oauth_token_response_parse(ctx, json_ok, strlen(json_ok), NULL) == MCP_ERR_INVALID_ARGUMENT);

    // 3. Build authorization code token request with PKCE
    char *req_body = NULL;
    CHECK(mcp_oauth_build_token_request_pkce(ctx, "auth-code-xyz", "verifier-abc", "https://app/callback", "client-1", &req_body) == MCP_OK);
    CHECK(req_body != NULL);
    CHECK(strstr(req_body, "grant_type=authorization_code") != NULL);
    CHECK(strstr(req_body, "code=auth-code-xyz") != NULL);
    CHECK(strstr(req_body, "code_verifier=verifier-abc") != NULL);
    CHECK(strstr(req_body, "client_id=client-1") != NULL);
    CHECK(strstr(req_body, "redirect_uri=https%3A%2F%2Fapp%2Fcallback") != NULL ||
          strstr(req_body, "redirect_uri=https://app/callback") != NULL);
    mcp_oauth_free_string(ctx, req_body);

    CHECK(mcp_oauth_build_token_request_pkce(ctx, NULL, "verifier", NULL, NULL, &req_body) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_oauth_build_token_request_pkce(ctx, "code", NULL, NULL, NULL, &req_body) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_oauth_build_token_request_pkce(ctx, "code", "verifier", NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    // 4. Build refresh token request (SEP-2207)
    char *ref_body = NULL;
    CHECK(mcp_oauth_build_refresh_request(ctx, "ref-456", "client-1", "read", &ref_body) == MCP_OK);
    CHECK(ref_body != NULL);
    CHECK(strstr(ref_body, "grant_type=refresh_token") != NULL);
    CHECK(strstr(ref_body, "refresh_token=ref-456") != NULL);
    CHECK(strstr(ref_body, "client_id=client-1") != NULL);
    CHECK(strstr(ref_body, "scope=read") != NULL);
    mcp_oauth_free_string(ctx, ref_body);

    CHECK(mcp_oauth_build_refresh_request(ctx, NULL, NULL, NULL, &ref_body) == MCP_ERR_INVALID_ARGUMENT);

    // 5. Build client credentials request (SEP-1046)
    char *cc_body = NULL;
    CHECK(mcp_oauth_build_client_credentials_request(ctx, "client-1", "secret-key", "admin", &cc_body) == MCP_OK);
    CHECK(cc_body != NULL);
    CHECK(strstr(cc_body, "grant_type=client_credentials") != NULL);
    CHECK(strstr(cc_body, "client_id=client-1") != NULL);
    CHECK(strstr(cc_body, "client_secret=secret-key") != NULL);
    CHECK(strstr(cc_body, "scope=admin") != NULL);
    mcp_oauth_free_string(ctx, cc_body);

    CHECK(mcp_oauth_build_client_credentials_request(ctx, NULL, "secret", NULL, &cc_body) == MCP_ERR_INVALID_ARGUMENT);

    mcp_context_destroy(ctx);
    printf("test_auth_client OK\n");
    return 0;
}
