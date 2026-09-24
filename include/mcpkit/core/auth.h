/**
 * @file auth.h
 * @brief RFC 7636 PKCE (Proof Key for Code Exchange) & OAuth 2.1 utilities.
 *
 * Implements S256 code challenge computation, cryptographically secure code
 * verifier generation, and RFC 8414 Authorization Server Metadata parsing.
 *
 * @ingroup mcpkit-core
 */

#ifndef MCPKIT_CORE_AUTH_H
#define MCPKIT_CORE_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_PKCE_VERIFIER_MIN_LEN 43
#define MCP_PKCE_VERIFIER_MAX_LEN 128
#define MCP_PKCE_CHALLENGE_LEN    43

/**
 * @brief OAuth 2.0 / 2.1 Authorization Server Metadata (RFC 8414).
 */
typedef struct mcp_oauth_metadata {
    char issuer[256];
    char authorization_endpoint[256];
    char token_endpoint[256];
    char registration_endpoint[256];
    char jwks_uri[256];
    bool supports_pkce_s256;
} mcp_oauth_metadata_t;

/**
 * @brief Computes the RFC 7636 S256 code_challenge for a given code_verifier.
 *
 * `code_challenge = BASE64URL-ENCODE(SHA256(ASCII(code_verifier)))` without padding.
 *
 * @param code_verifier       Input verifier string (must be 43-128 chars, unreserved chars).
 * @param code_challenge_out  Output buffer of at least 44 bytes (receives 43 chars + NUL).
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if code_verifier is NULL, out of length range,
 *         contains invalid characters, or code_challenge_out is NULL.
 */
mcp_status_t mcp_pkce_compute_challenge(const char *code_verifier,
                                        char code_challenge_out[44]);

/**
 * @brief Generates a cryptographically random code_verifier and its S256 code_challenge.
 *
 * @param ctx                 Context; may be NULL.
 * @param code_verifier_out   Output buffer of at least 129 bytes (receives 64 random chars + NUL).
 * @param code_challenge_out  Output buffer of at least 44 bytes (receives 43 chars + NUL).
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if output buffers are NULL.
 */
mcp_status_t mcp_pkce_generate(mcp_context_t *ctx,
                               char code_verifier_out[129],
                               char code_challenge_out[44]);

/**
 * @brief Parses an RFC 8414 OAuth 2.0 / 2.1 Authorization Server Metadata JSON object.
 *
 * Extracts issuer, endpoints, and checks code_challenge_methods_supported for "S256".
 *
 * @param ctx            Context; may be NULL.
 * @param json_metadata  Parsed JSON metadata object.
 * @param out_metadata   Receives populated metadata structure.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if json_metadata or out_metadata is NULL;
 *         MCP_ERR_PROTOCOL if required fields (issuer, authorization_endpoint, token_endpoint)
 *                          are missing or not strings.
 */
mcp_status_t mcp_oauth_metadata_parse(mcp_context_t *ctx,
                                      const mcp_json_value_t *json_metadata,
                                      mcp_oauth_metadata_t *out_metadata);

/**
 * @brief OAuth 2.0 / 2.1 Token Response (RFC 6749 Section 5.1).
 */
typedef struct mcp_oauth_token_response {
    char *access_token;
    char *token_type;
    uint32_t expires_in;
    char *refresh_token;
    char *scope;
} mcp_oauth_token_response_t;

/**
 * @brief Parses an OAuth 2.0 / 2.1 Token Response JSON string.
 *
 * @param ctx       Context; may be NULL.
 * @param json_str  JSON string returned by the token endpoint.
 * @param len       Length of json_str.
 * @param resp_out  Receives populated token response. Caller frees via mcp_oauth_token_response_cleanup().
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT on NULL args;
 *         MCP_ERR_PROTOCOL if access_token or token_type is missing/invalid.
 */
mcp_status_t mcp_oauth_token_response_parse(mcp_context_t *ctx,
                                            const char *json_str,
                                            size_t len,
                                            mcp_oauth_token_response_t *resp_out);

/**
 * @brief Frees allocated strings in mcp_oauth_token_response_t.
 */
void mcp_oauth_token_response_cleanup(mcp_context_t *ctx,
                                      mcp_oauth_token_response_t *resp);

/**
 * @brief Builds application/x-www-form-urlencoded body for PKCE authorization code exchange.
 *
 * grant_type=authorization_code&code=...&code_verifier=...&redirect_uri=...&client_id=...
 */
mcp_status_t mcp_oauth_build_token_request_pkce(mcp_context_t *ctx,
                                                const char *code,
                                                const char *code_verifier,
                                                const char *redirect_uri,
                                                const char *client_id,
                                                char **body_out);

/**
 * @brief Builds application/x-www-form-urlencoded body for token refresh (SEP-2207).
 *
 * grant_type=refresh_token&refresh_token=...&client_id=...&scope=...
 */
mcp_status_t mcp_oauth_build_refresh_request(mcp_context_t *ctx,
                                             const char *refresh_token,
                                             const char *client_id,
                                             const char *scope,
                                             char **body_out);

/**
 * @brief Builds application/x-www-form-urlencoded body for OAuth Client Credentials (SEP-1046).
 *
 * grant_type=client_credentials&client_id=...&client_secret=...&scope=...
 */
mcp_status_t mcp_oauth_build_client_credentials_request(mcp_context_t *ctx,
                                                        const char *client_id,
                                                        const char *client_secret,
                                                        const char *scope,
                                                        char **body_out);

/**
 * @brief Frees a string allocated by mcp_oauth_build_* functions.
 */
void mcp_oauth_free_string(mcp_context_t *ctx, char *str);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_AUTH_H */
