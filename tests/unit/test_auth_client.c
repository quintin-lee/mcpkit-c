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

    /* 0. RFC 8693 URN constants check */
    CHECK(strcmp(MCP_OAUTH_GRANT_TYPE_TOKEN_EXCHANGE, "urn:ietf:params:oauth:grant-type:token-exchange") == 0);
    CHECK(strcmp(MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN, "urn:ietf:params:oauth:token-type:access_token") == 0);

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

    // 7. Test RFC 9207 iss validation (SEP-2468)
    CHECK(!mcp_oauth_validate_issuer(NULL, "https://auth.example.com"));
    CHECK(mcp_oauth_validate_issuer("https://auth.example.com", NULL)); // iss omitted is OK
    CHECK(mcp_oauth_validate_issuer("https://auth.example.com", "https://auth.example.com"));
    CHECK(!mcp_oauth_validate_issuer("https://auth.example.com", "https://attacker.example.com"));

    // 8. Test Client ID Metadata Documents (SEP-991)
    const char *doc_json =
        "{"
        "  \"client_id\": \"https://app.example.com/oauth/client-metadata.json\","
        "  \"client_name\": \"Example MCP Client\","
        "  \"client_uri\": \"https://app.example.com\","
        "  \"logo_uri\": \"https://app.example.com/logo.png\","
        "  \"redirect_uris\": ["
        "    \"http://127.0.0.1:3000/callback\","
        "    \"http://localhost:3000/callback\""
        "  ],"
        "  \"grant_types\": [\"authorization_code\"],"
        "  \"response_types\": [\"code\"],"
        "  \"token_endpoint_auth_method\": \"none\""
        "}";

    mcp_oauth_client_metadata_t cm;
    memset(&cm, 0, sizeof(cm));
    CHECK(mcp_oauth_client_metadata_parse(ctx, doc_json, strlen(doc_json), &cm) == MCP_OK);
    CHECK(cm.client_id != NULL && strcmp(cm.client_id, "https://app.example.com/oauth/client-metadata.json") == 0);
    CHECK(cm.client_name != NULL && strcmp(cm.client_name, "Example MCP Client") == 0);
    CHECK(cm.client_uri != NULL && strcmp(cm.client_uri, "https://app.example.com") == 0);
    CHECK(cm.logo_uri != NULL && strcmp(cm.logo_uri, "https://app.example.com/logo.png") == 0);
    CHECK(cm.redirect_uris_count == 2);
    CHECK(strcmp(cm.redirect_uris[0], "http://127.0.0.1:3000/callback") == 0);
    CHECK(strcmp(cm.redirect_uris[1], "http://localhost:3000/callback") == 0);
    CHECK(cm.grant_types_count == 1 && strcmp(cm.grant_types[0], "authorization_code") == 0);
    CHECK(cm.response_types_count == 1 && strcmp(cm.response_types[0], "code") == 0);
    CHECK(cm.token_endpoint_auth_method != NULL && strcmp(cm.token_endpoint_auth_method, "none") == 0);

    // Validation
    CHECK(mcp_oauth_client_metadata_validate(&cm, "https://app.example.com/oauth/client-metadata.json") == MCP_OK);
    CHECK(mcp_oauth_client_metadata_validate(&cm, NULL) == MCP_OK);
    CHECK(mcp_oauth_client_metadata_validate(&cm, "https://other.example.com/client.json") == MCP_ERR_PROTOCOL);

    // Serialization & re-parse
    char *serialized_doc = mcp_oauth_client_metadata_serialize(ctx, &cm);
    CHECK(serialized_doc != NULL);
    CHECK(strstr(serialized_doc, "https://app.example.com/oauth/client-metadata.json") != NULL);
    CHECK(strstr(serialized_doc, "Example MCP Client") != NULL);

    mcp_oauth_client_metadata_t cm2;
    memset(&cm2, 0, sizeof(cm2));
    CHECK(mcp_oauth_client_metadata_parse(ctx, serialized_doc, strlen(serialized_doc), &cm2) == MCP_OK);
    CHECK(mcp_oauth_client_metadata_validate(&cm2, "https://app.example.com/oauth/client-metadata.json") == MCP_OK);
    mcp_oauth_client_metadata_cleanup(ctx, &cm2);
    mcp_oauth_free_string(ctx, serialized_doc);

    mcp_oauth_client_metadata_cleanup(ctx, &cm);

    // Edge cases for client metadata
    const char *bad_doc_nohref = "{\"client_id\":\"http://insecure.example.com/client.json\",\"client_name\":\"app\",\"redirect_uris\":[\"http://localhost\"]}";
    CHECK(mcp_oauth_client_metadata_parse(ctx, bad_doc_nohref, strlen(bad_doc_nohref), &cm) == MCP_OK);
    CHECK(mcp_oauth_client_metadata_validate(&cm, NULL) == MCP_ERR_PROTOCOL); // http is not allowed
    mcp_oauth_client_metadata_cleanup(ctx, &cm);

    const char *bad_doc_nopath = "{\"client_id\":\"https://example.com\",\"client_name\":\"app\",\"redirect_uris\":[\"http://localhost\"]}";
    CHECK(mcp_oauth_client_metadata_parse(ctx, bad_doc_nopath, strlen(bad_doc_nopath), &cm) == MCP_OK);
    CHECK(mcp_oauth_client_metadata_validate(&cm, NULL) == MCP_ERR_PROTOCOL); // no path component
    mcp_oauth_client_metadata_cleanup(ctx, &cm);

    const char *bad_doc_missing = "{\"client_id\":\"https://example.com/c.json\"}";
    CHECK(mcp_oauth_client_metadata_parse(ctx, bad_doc_missing, strlen(bad_doc_missing), &cm) == MCP_ERR_PROTOCOL);

    /* 9. RFC 8693 Token Exchange Request Builder */
    mcp_oauth_token_exchange_req_t ex_req = {
        .subject_token = "idp-subject-token-xyz",
        .subject_token_type = MCP_OAUTH_TOKEN_TYPE_JWT,
    };
    char *ex_body = NULL;
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &ex_req, &ex_body) == MCP_OK);
    CHECK(ex_body != NULL);
    CHECK(strstr(ex_body, "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange") != NULL);
    CHECK(strstr(ex_body, "subject_token=idp-subject-token-xyz") != NULL);
    CHECK(strstr(ex_body, "subject_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Ajwt") != NULL);
    mcp_oauth_free_string(ctx, ex_body);
    ex_body = NULL;

    /* Full Token Exchange request with delegation */
    ex_req.actor_token = "actor-token-abc";
    ex_req.actor_token_type = MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN;
    ex_req.resource = "https://mcp.example.com/api";
    ex_req.audience = "mcp-service";
    ex_req.scope = "tools:read tools:write";
    ex_req.requested_token_type = MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN;
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &ex_req, &ex_body) == MCP_OK);
    CHECK(strstr(ex_body, "actor_token=actor-token-abc") != NULL);
    CHECK(strstr(ex_body, "actor_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Aaccess_token") != NULL);
    CHECK(strstr(ex_body, "resource=https%3A%2F%2Fmcp.example.com%2Fapi") != NULL);
    CHECK(strstr(ex_body, "audience=mcp-service") != NULL);
    CHECK(strstr(ex_body, "scope=tools%3Aread%20tools%3Awrite") != NULL);
    CHECK(strstr(ex_body, "requested_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Aaccess_token") != NULL);
    mcp_oauth_free_string(ctx, ex_body);
    ex_body = NULL;

    /* Negative tests */
    mcp_oauth_token_exchange_req_t bad_req = { 0 };
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &bad_req, &ex_body) == MCP_ERR_INVALID_ARGUMENT);
    bad_req.subject_token = "token";
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &bad_req, &ex_body) == MCP_ERR_INVALID_ARGUMENT);
    bad_req.subject_token_type = MCP_OAUTH_TOKEN_TYPE_JWT;
    bad_req.actor_token = "actor";
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &bad_req, &ex_body) == MCP_ERR_INVALID_ARGUMENT);

    mcp_context_destroy(ctx);
    printf("test_auth_client OK\n");
    return 0;
}
