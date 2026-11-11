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

/**
 * @brief Number of days between the deprecation announcement of the RFC 7591
 *        Dynamic Client Registration endpoint and its scheduled Sunset.
 *
 * P3: when the server serves a DCR endpoint and the negotiated session is at
 *        or above the P3 gate (mcp_proto_policy_cimd_preferred), the response
 *        carries `Deprecation: true` and `Sunset: <now + this many days>`.
 */
#define MCP_OAUTH_DCR_DEPRECATION_WINDOW_DAYS 365

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
    /* P3: OIDC Discovery 1.0 (OpenID Connect Discovery, draft-ietf-oauth-openid-connect-discovery / RFC 8414 §10) */
    char end_session_endpoint[256];
    char revocation_endpoint[256];
    char introspection_endpoint[256];
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
 * @brief RFC 9728 Protected Resource Metadata (P3 T3).
 *
 * Serialized form of a document served at
 * `/.well-known/oauth-protected-resource`. `authorization_server` holds
 * the first entry of the `authorization_servers` array (RFC 9728 §2); the
 * plan keeps the rest out of scope (single-AS assumption, documented in
 * the P3 spec).
 */
typedef struct mcp_protected_resource_metadata {
    char issuer[256];
    char resource[512];
    char authorization_server[512];
    char scope[256];
    char jwks_uri[256];
} mcp_protected_resource_metadata_t;

/**
 * @brief Parses an RFC 9728 Protected Resource Metadata document.
 *
 * Recognized keys: `resource` (string, REQUIRED), `authorization_servers`
 * (array; first string element taken into authorization_server, optional),
 * `scopes_supported` (array; first element into scope, optional),
 * `issuer` and `jwks_uri` (strings, optional). A missing `resource` key
 * returns MCP_ERR_PROTOCOL.
 *
 * @param ctx       Context; may be NULL.
 * @param json_str JSON document text.
 * @param len       Length of json_str.
 * @param out_meta  Receives populated metadata (zero-initialized on entry).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on NULL args or
 *         malformed JSON; MCP_ERR_PROTOCOL if `resource` is absent;
 *         MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_oauth_protected_resource_parse(mcp_context_t *ctx,
                                                const char *json_str,
                                                size_t len,
                                                mcp_protected_resource_metadata_t *out_meta);

/**
 * @brief Serializes a Protected Resource Metadata document to JSON.
 *
 * Emits only non-empty fields; the result is suitable as the body of a
 * 200 response for GET /.well-known/oauth-protected-resource.
 *
 * @param ctx  Context; may be NULL.
 * @param meta Metadata to serialize (non-NULL).
 * @return Heap-allocated JSON string; free with mcp_oauth_free_string().
 *         NULL on allocation failure.
 */
char *mcp_oauth_protected_resource_serialize(mcp_context_t *ctx,
                                              const mcp_protected_resource_metadata_t *meta);

/**
 * @brief Merges a base RFC 8414 metadata document with an OpenID Connect
 *        Discovery 1.0 extension document into a single combined view (P3 T5).
 *
 * Merge semantics (both inputs must be non-NULL; out==base or out==oidc is
 * accepted for in-place use):
 *  - Endpoint / issuer fields: `base` value wins when non-empty; otherwise
 *    the `oidc` value fills the slot (the OIDC document may repeat the
 *    RFC 8414 fields with identical content).
 *  - `supports_pkce_s256` and `client_id_metadata_document_supported`: OR of
 *    the two inputs.
 *  - All other fields are copied from `base`.
 *  - `ctx` may be NULL (no allocation happens).
 *
 * @param ctx     Context; may be NULL.
 * @param base    Base metadata (RFC 8414, possibly populated via
 *                mcp_oauth_metadata_parse); must be non-NULL.
 * @param oidc    OIDC Discovery 1.0 extension metadata (same struct, parsed
 *                from the .well-known/openid-configuration document); must
 *                be non-NULL.
 * @param merged_out  Receives the merged result; must be non-NULL. May
 *                alias `base` or `oidc` for in-place merge.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on any NULL argument.
 */
mcp_status_t mcp_oauth_discovery_merge(mcp_context_t *ctx,
                                       const mcp_oauth_metadata_t *base,
                                       const mcp_oauth_metadata_t *oidc,
                                       mcp_oauth_metadata_t *merged_out);

/**
 * @brief OAuth 2.0 / 2.1 Token Response (RFC 6749 Section 5.1).
 *
 * `iss` is populated when the token endpoint returns the RFC 9207
 * issuer field (SEP-2468); NULL when omitted (which is valid per the
 * SEP).
 */
typedef struct mcp_oauth_token_response {
    char *access_token;
    char *token_type;
    uint32_t expires_in;
    char *refresh_token;
    char *scope;
    char *iss;
} mcp_oauth_token_response_t;

/**
 * @brief RFC 9207 issuer gate (P3).
 *
 * Validates `resp->iss` (if present) against `expected_issuer`. When
 * `enforce_must` is true, a non-NULL `expected_issuer` paired with a
 * mismatching `resp->iss` returns MCP_ERR_PROTOCOL (MUST-grade, used by
 * 2026-07-28 sessions). When `enforce_must` is false, mismatches are
 * treated as SHOULD-grade: the call returns MCP_OK; the caller may log
 * a warning but must not block the response (used by 2025-11-25 sessions).
 * A NULL `expected_issuer` always returns MCP_OK (no expectation recorded);
 * a NULL `resp->iss` is accepted in both modes (SEP-2468: issuer omitted
 * is valid).
 *
 * @param ctx           Context; may be NULL.
 * @param resp          Parsed token response; must be non-NULL.
 * @param expected_issuer Recorded issuer from discovery; NULL skips the check.
 * @param enforce_must  true enforces MUST (reject on mismatch); false enforces SHOULD.
 * @return MCP_OK on pass or SHOULD-grade warning; MCP_ERR_PROTOCOL on MUST-grade
 *         mismatch; MCP_ERR_INVALID_ARGUMENT on NULL `resp`.
 */
mcp_status_t mcp_oauth_gate_issuer(mcp_context_t *ctx,
                                   const mcp_oauth_token_response_t *resp,
                                   const char *expected_issuer,
                                   bool enforce_must);

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
 * @brief Builds the Deprecation / Sunset response header pair announcing
 *        that the RFC 7591 Dynamic Client Registration (DCR) endpoint is
 *        deprecated in favor of Client ID Metadata Documents (SEP-991).
 *
 * The two returned owned strings are:
 *  - `Deprecation: true`
 *  - `Sunset: <RFC 1123 date>`, computed as the current time plus
 *    MCP_OAUTH_DCR_DEPRECATION_WINDOW_DAYS.
 *
 * Both are freed via mcp_oauth_free_string(). Callers that are not
 * serving a DCR endpoint should skip the call.
 *
 * @param ctx           Context; may be NULL.
 * @param deprecation_out Receives `"true"` (owned) on MCP_OK.
 * @param sunset_out    Receives `"Wed, 04 Nov 2027 12:00:00 GMT"`-style
 *                      string (owned) on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if either out pointer
 *         is NULL; MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_oauth_build_dcr_deprecation_headers(mcp_context_t *ctx,
                                                     char **deprecation_out,
                                                     char **sunset_out);

/**
 * @brief Builds application/x-www-form-urlencoded body for RFC 8693 Token Exchange.
 *
 * @param ctx       Context; may be NULL.
 * @param req       Populated request descriptor (non-NULL).
 * @param body_out  Receives newly allocated URL-encoded request body on MCP_OK.
 *                  Caller frees with mcp_oauth_free_string().
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if req, subject_token, subject_token_type, or body_out is NULL/invalid,
 *                                  or if actor_token is set without actor_token_type;
 *         MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_oauth_build_token_exchange_request(mcp_context_t *ctx,
                                                    const mcp_oauth_token_exchange_req_t *req,
                                                    char **body_out);

/**
 * @brief Parses an RFC 8693 OAuth 2.0 Token Exchange response JSON string.
 *
 * @param ctx       Context; may be NULL.
 * @param json_str  JSON string returned by the token endpoint.
 * @param len       Length of json_str.
 * @param resp_out  Receives populated token exchange response. Caller frees via mcp_oauth_token_exchange_response_cleanup().
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT on NULL args;
 *         MCP_ERR_PROTOCOL if access_token, issued_token_type, or token_type is missing/invalid,
 *                          or if JSON parsing fails;
 *         MCP_ERR_NOMEM on memory allocation failure.
 */
mcp_status_t mcp_oauth_token_exchange_response_parse(mcp_context_t *ctx,
                                                     const char *json_str,
                                                     size_t len,
                                                     mcp_oauth_token_exchange_response_t *resp_out);

/**
 * @brief Frees allocated strings in mcp_oauth_token_exchange_response_t and zeroes the struct.
 *
 * @param ctx   Context; may be NULL.
 * @param resp  Token exchange response structure to clean up.
 */
void mcp_oauth_token_exchange_response_cleanup(mcp_context_t *ctx,
                                               mcp_oauth_token_exchange_response_t *resp);

/**
 * @brief Frees a string allocated by mcp_oauth_build_* functions.
 */
void mcp_oauth_free_string(mcp_context_t *ctx, char *str);

/* -------------------------------------------------------------------------
 * P4-a SEP-2243: RFC 4648 §4 std base64 (padded, '/' character).
 * Used for the base64 sentinel encoding of x-mcp-header values whose
 * representation is not plain-ASCII-safe (whitespace / non-ASCII / control
 * chars). The encode output is allocated via the ctx allocator and freed
 * with mcp_oauth_free_string; the decode output is allocated via libc
 * malloc and freed with free().
 * ---------------------------------------------------------------------- */

/**
 * @brief Encodes `in` as RFC 4648 §4 std base64 (padded).
 *
 * @param ctx    Context; may be NULL (uses default allocator).
 * @param in     Input bytes.
 * @param in_len Input length; 0 yields an empty string "".
 * @return Heap-allocated NUL-terminated base64 string, or NULL on OOM.
 *         Free with mcp_oauth_free_string(ctx, out).
 */
char *mcp_base64_encode(mcp_context_t *ctx, const uint8_t *in, size_t in_len);

/**
 * @brief Decodes `in` as RFC 4648 §4 std base64.
 *
 * @param ctx    Context (currently unused; reserved for allocator routing).
 * @param in     Base64 string.
 * @param in_len Length of in.
 * @param out    Receives a libc-malloc'd byte buffer (free with free()).
 * @param out_len Receives the decoded byte count.
 * @return 0 on success, non-zero on invalid input (bad char / bad length /
 *         OOM).
 */
int mcp_base64_decode(mcp_context_t *ctx, const char *in, size_t in_len,
                      uint8_t **out, size_t *out_len);

/* -------------------------------------------------------------------------
 * P4-a: RFC 9728 AS auto-discovery.
 * Given an RFC 9728 protected-resource metadata's `authorization_server`
 * (issuer URL), probes the RFC 8414 §3.1 discovery path
 * `/.well-known/oauth-authorization-server` and merges the result with
 * any existing AS metadata using mcp_oauth_discovery_merge.
 * The HTTP fetch is performed by the host via the supplied callback;
 * the library does not perform network I/O itself.
 * ---------------------------------------------------------------------- */

/**
 * @brief Host-injected HTTP fetch callback for AS discovery.
 *
 * @param ud          Opaque user data.
 * @param url         The URL to fetch (NUL-terminated).
 * @param body_out    Receives a libc-malloc'd response body (caller frees
 *                    via mcp_oauth_free_string after use). Must be NUL-terminated.
 * @param body_len_out Receives the body length (excluding NUL).
 * @return 0 on success, -1 on failure.
 */
typedef int (*mcp_http_fetch_fn)(void *ud, const char *url,
                                 char **body_out, size_t *body_len_out);

/**
 * @brief Discovers AS metadata from an RFC 9728 protected-resource document.
 *
 * Builds the RFC 8414 §3.1 probe URL from `prm->authorization_server`,
 * fetches via `fetch`, parses the AS discovery document, and merges with
 * `existing_as` (if non-NULL) into `out_merged`.
 *
 * @param ctx        Context; may be NULL.
 * @param prm        RFC 9728 protected-resource metadata; NULL -> INVALID_ARGUMENT.
 * @param fetch      Host fetch callback; NULL -> PROTOCOL.
 * @param fetch_ud   Opaque user data for fetch.
 * @param existing_as  Existing AS/OIDC metadata to merge into; NULL ->
 *                     direct parse without merge.
 * @param out_merged Receives the merged metadata. On failure all fields are
 *                   zeroed. `mcp_oauth_metadata_t` is a plain POD struct;
 *                   no destructor is required.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if prm or its
 *         authorization_server is NULL; MCP_ERR_PROTOCOL if fetch fails,
 *         the response is not valid JSON, or parse fails.
 */
mcp_status_t mcp_oauth_discover_as(mcp_context_t *ctx,
                                   const mcp_protected_resource_metadata_t *prm,
                                   mcp_http_fetch_fn fetch, void *fetch_ud,
                                   const mcp_oauth_metadata_t *existing_as,
                                   mcp_oauth_metadata_t *out_merged);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_AUTH_H */
