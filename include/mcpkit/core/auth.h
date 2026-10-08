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

/** @brief RFC 8693 OAuth 2.0 Token Exchange grant type */
#define MCP_OAUTH_GRANT_TYPE_TOKEN_EXCHANGE "urn:ietf:params:oauth:grant-type:token-exchange"

/** @brief RFC 8693 Standard Security Token Types */
#define MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN   "urn:ietf:params:oauth:token-type:access_token"
#define MCP_OAUTH_TOKEN_TYPE_REFRESH_TOKEN  "urn:ietf:params:oauth:token-type:refresh_token"
#define MCP_OAUTH_TOKEN_TYPE_ID_TOKEN       "urn:ietf:params:oauth:token-type:id_token"
#define MCP_OAUTH_TOKEN_TYPE_SAML1          "urn:ietf:params:oauth:token-type:saml1"
#define MCP_OAUTH_TOKEN_TYPE_SAML2          "urn:ietf:params:oauth:token-type:saml2"
#define MCP_OAUTH_TOKEN_TYPE_JWT            "urn:ietf:params:oauth:token-type:jwt"

/**
 * @brief Parameters for an RFC 8693 Token Exchange request.
 */
typedef struct mcp_oauth_token_exchange_req {
    const char *subject_token;        /**< REQUIRED. Subject security token */
    const char *subject_token_type;   /**< REQUIRED. Type URI of subject_token */
    const char *actor_token;          /**< OPTIONAL. Actor security token for delegation */
    const char *actor_token_type;     /**< OPTIONAL. Type URI of actor_token (REQUIRED if actor_token set) */
    const char *resource;             /**< OPTIONAL. Target service URI */
    const char *audience;             /**< OPTIONAL. Logical name of target service */
    const char *scope;                /**< OPTIONAL. Requested scope */
    const char *requested_token_type; /**< OPTIONAL. Desired token type URI */
} mcp_oauth_token_exchange_req_t;

/**
 * @brief Parsed response from an RFC 8693 Token Exchange endpoint.
 */
typedef struct mcp_oauth_token_exchange_response {
    char *access_token;               /**< REQUIRED. Issued security token */
    char *issued_token_type;          /**< REQUIRED. URI indicating type of issued token */
    char *token_type;                 /**< REQUIRED. Case-insensitive token type (e.g. "Bearer") */
    uint32_t expires_in;              /**< OPTIONAL. Lifetime in seconds */
    char *refresh_token;              /**< OPTIONAL. Refresh token if issued */
    char *scope;                      /**< OPTIONAL. Granted scope */
} mcp_oauth_token_exchange_response_t;

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
    bool client_id_metadata_document_supported;
} mcp_oauth_metadata_t;

/**
 * @brief Validates an authorization response 'iss' parameter against the expected issuer (RFC 9207 / SEP-2468).
 *
 * Per SEP-2468, if the authorization response contains an 'iss' parameter, the client MUST validate
 * that it matches the recorded issuer identifier. If 'iss' is omitted, validation succeeds.
 *
 * @param expected_issuer The recorded issuer URL from discovery (must not be NULL).
 * @param response_issuer The 'iss' parameter returned in authorization response (may be NULL if omitted).
 * @return true if valid (matches or response_issuer is NULL); false if response_issuer is present and does not match.
 */
bool mcp_oauth_validate_issuer(const char *expected_issuer, const char *response_issuer);

/**
 * @brief Client ID Metadata Document structure (SEP-991 / PR #2858).
 *
 * Used for URL-based client registration in place of deprecated RFC 7591.
 */
typedef struct mcp_oauth_client_metadata {
    char *client_id;                  /**< HTTPS URL of this metadata document (required) */
    char *client_name;                /**< Human-readable client name (required) */
    char *client_uri;                 /**< Optional client web page URI */
    char *logo_uri;                   /**< Optional logo URI */
    char **redirect_uris;             /**< Array of redirection URIs (required >= 1) */
    size_t redirect_uris_count;
    char **grant_types;               /**< Array of allowed grant types, e.g. ["authorization_code"] */
    size_t grant_types_count;
    char **response_types;            /**< Array of response types, e.g. ["code"] */
    size_t response_types_count;
    char *token_endpoint_auth_method; /**< e.g. "none", "client_secret_post", "private_key_jwt" */
    char *jwks_uri;                   /**< Optional JWKS URI */
} mcp_oauth_client_metadata_t;

/**
 * @brief Parses a Client ID Metadata Document JSON string (SEP-991).
 *
 * @param ctx        Context; may be NULL.
 * @param json_str   JSON string content.
 * @param len        Length of json_str.
 * @param meta_out   Receives populated metadata. Caller frees via mcp_oauth_client_metadata_cleanup().
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT on NULL arguments;
 *         MCP_ERR_PROTOCOL if JSON is invalid or required fields (client_id, client_name, redirect_uris) are missing.
 */
mcp_status_t mcp_oauth_client_metadata_parse(mcp_context_t *ctx,
                                             const char *json_str,
                                             size_t len,
                                             mcp_oauth_client_metadata_t *meta_out);

/**
 * @brief Validates a Client ID Metadata Document (SEP-991).
 *
 * Checks:
 * - client_id is an HTTPS URL and contains a path component (e.g. https://.../...)
 * - If expected_url is non-NULL, client_id matches expected_url exactly
 * - client_name is non-empty
 * - redirect_uris has at least one valid URI
 *
 * @param meta         Parsed client metadata.
 * @param expected_url Optional expected URL where document was fetched (may be NULL).
 * @return MCP_OK if valid; MCP_ERR_INVALID_ARGUMENT or MCP_ERR_PROTOCOL on failure.
 */
mcp_status_t mcp_oauth_client_metadata_validate(const mcp_oauth_client_metadata_t *meta,
                                                const char *expected_url);

/**
 * @brief Serializes a Client ID Metadata Document to JSON (SEP-991).
 *
 * @param ctx   Context; may be NULL.
 * @param meta  Client metadata structure.
 * @return Heap-allocated JSON string; caller frees with mcp_oauth_free_string().
 */
char *mcp_oauth_client_metadata_serialize(mcp_context_t *ctx,
                                          const mcp_oauth_client_metadata_t *meta);

/**
 * @brief Frees all allocated memory in mcp_oauth_client_metadata_t.
 */
void mcp_oauth_client_metadata_cleanup(mcp_context_t *ctx,
                                       mcp_oauth_client_metadata_t *meta);


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
