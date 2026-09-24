#include <stdio.h>
#include <string.h>
#include "mcpkit/client/client.h"
#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "mcpkit/transport/transport.h"
#include "test_check.h"

static mcp_status_t noop_start(mcp_context_t *ctx, mcp_transport_t *t) { (void)ctx; (void)t; return MCP_OK; }
static mcp_status_t noop_send(mcp_context_t *ctx, mcp_transport_t *t, const char *d, size_t n) { (void)ctx; (void)t; (void)d; (void)n; return MCP_OK; }
static mcp_status_t noop_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) { (void)ctx; (void)t; *line_out = NULL; return MCP_ERR_IO; }
static mcp_status_t noop_stop(mcp_context_t *ctx, mcp_transport_t *t) { (void)ctx; (void)t; return MCP_OK; }
static const mcp_transport_ops_t kNoop = { noop_start, noop_send, noop_recv, noop_stop };


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

    // 6. Test client bearer token management
    CHECK(mcp_client_set_bearer_token(ctx, NULL, "tok") == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_get_bearer_token(ctx, NULL) == NULL);

    mcp_transport_t *t = mcp_transport_create(ctx, &kNoop, NULL);
    CHECK(t != NULL);
    mcp_client_t *client = mcp_client_create(ctx, t);
    CHECK(client != NULL);

    CHECK(mcp_client_get_bearer_token(ctx, client) == NULL);

    CHECK(mcp_client_set_bearer_token(ctx, client, "secret-token-123") == MCP_OK);
    const char *tok = mcp_client_get_bearer_token(ctx, client);
    CHECK(tok != NULL && strcmp(tok, "secret-token-123") == 0);

    CHECK(mcp_client_set_bearer_token(ctx, client, "new-token-456") == MCP_OK);
    tok = mcp_client_get_bearer_token(ctx, client);
    CHECK(tok != NULL && strcmp(tok, "new-token-456") == 0);

    CHECK(mcp_client_set_bearer_token(ctx, client, NULL) == MCP_OK);
    CHECK(mcp_client_get_bearer_token(ctx, client) == NULL);

    // Destroy client with token active to ensure cleanup without leaks
    CHECK(mcp_client_set_bearer_token(ctx, client, "token-before-destroy") == MCP_OK);
    mcp_client_destroy(ctx, client);
    mcp_transport_destroy(ctx, t);

    mcp_context_destroy(ctx);
    printf("test_auth_client OK\n");
    return 0;
}
