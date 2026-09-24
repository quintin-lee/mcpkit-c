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

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_AUTH_H */
