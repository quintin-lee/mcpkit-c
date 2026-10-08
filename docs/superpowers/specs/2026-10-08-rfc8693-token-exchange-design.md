# RFC 8693 OAuth 2.0 Token Exchange Design Specification

**Status**: Approved  
**Date**: 2026-10-08  
**Topic**: RFC 8693 OAuth 2.0 Token Exchange Support for Enterprise Managed Authorization (SEP-990) in `mcpkit-c`  
**Target Specification**: Model Context Protocol (MCP) 2026-07-28  

---

## 1. Background & Goals

Enterprise deployments of the Model Context Protocol (MCP) often require identity propagation across microservices and corporate Identity Providers (IdP). Under the MCP Enterprise Managed Authorization extension (SEP-990 / `extensions/auth/enterprise-managed-authorization.md`), client applications and agents exchange internal tokens (e.g., corporate IdP JWTs or SAML assertions) for an MCP resource-scoped access token via RFC 8693 (OAuth 2.0 Token Exchange).

While `mcpkit-c` already implements OAuth 2.1 PKCE code exchange, refresh token flows, client credentials, and Client ID Metadata Documents (SEP-991), it previously lacked dedicated RFC 8693 token exchange payload builders and parser helpers.

This specification details the data structures, standard URN constants, parameter validation, request encoding, and response decoding needed to bring full RFC 8693 token exchange support to `mcpkit-c`.

---

## 2. Architecture & Public Data Structures

In [`include/mcpkit/core/auth.h`](file:///data/home/quintin/workspace/source/c/mcpkit-c/include/mcpkit/core/auth.h), add the following definitions:

### 2.1 Standard URN Constants (RFC 8693 Section 2.1 & Section 3)
```c
/** @brief RFC 8693 OAuth 2.0 Token Exchange grant type */
#define MCP_OAUTH_GRANT_TYPE_TOKEN_EXCHANGE "urn:ietf:params:oauth:grant-type:token-exchange"

/** @brief RFC 8693 Standard Security Token Types */
#define MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN   "urn:ietf:params:oauth:token-type:access_token"
#define MCP_OAUTH_TOKEN_TYPE_REFRESH_TOKEN  "urn:ietf:params:oauth:token-type:refresh_token"
#define MCP_OAUTH_TOKEN_TYPE_ID_TOKEN       "urn:ietf:params:oauth:token-type:id_token"
#define MCP_OAUTH_TOKEN_TYPE_SAML1          "urn:ietf:params:oauth:token-type:saml1"
#define MCP_OAUTH_TOKEN_TYPE_SAML2          "urn:ietf:params:oauth:token-type:saml2"
#define MCP_OAUTH_TOKEN_TYPE_JWT            "urn:ietf:params:oauth:token-type:jwt"
```

### 2.2 Request Descriptor (`mcp_oauth_token_exchange_req_t`)
```c
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
```

### 2.3 Response Structure (`mcp_oauth_token_exchange_response_t`)
```c
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
```

---

## 3. API Signatures & Functional Behavior

### 3.1 Request Builder (`mcp_oauth_build_token_exchange_request`)
```c
mcp_status_t mcp_oauth_build_token_exchange_request(mcp_context_t *ctx,
                                                    const mcp_oauth_token_exchange_req_t *req,
                                                    char **body_out);
```
- **Validation**:
  - `req` and `body_out` must not be `NULL`.
  - `req->subject_token` and `req->subject_token_type` must be non-NULL and non-empty.
  - If `req->actor_token` is non-NULL and non-empty, `req->actor_token_type` must be non-NULL and non-empty (RFC 8693 requirement).
  - Returns `MCP_ERR_INVALID_ARGUMENT` upon any violation.
- **Encoding**:
  - Content type: `application/x-www-form-urlencoded`.
  - Serializes `grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange`.
  - Encodes `subject_token` and `subject_token_type`.
  - Optionally encodes `actor_token` and `actor_token_type`.
  - Optionally encodes `resource`, `audience`, `scope`, and `requested_token_type`.
- **Memory**:
  - Dynamically allocated via `mcp_context_t` allocator.
  - Caller frees with `mcp_oauth_free_string(ctx, str)`.

### 3.2 Response Parsing & Cleanup
```c
mcp_status_t mcp_oauth_token_exchange_response_parse(mcp_context_t *ctx,
                                                     const char *json_str,
                                                     size_t len,
                                                     mcp_oauth_token_exchange_response_t *resp_out);

void mcp_oauth_token_exchange_response_cleanup(mcp_context_t *ctx,
                                               mcp_oauth_token_exchange_response_t *resp);
```
- **Validation**:
  - `json_str` and `resp_out` must not be `NULL`.
  - JSON must parse into an object; otherwise returns `MCP_ERR_PROTOCOL`.
  - `access_token`, `issued_token_type`, and `token_type` are mandatory strings. Missing or invalid types return `MCP_ERR_PROTOCOL`.
  - Optional `expires_in` (number), `refresh_token` (string), and `scope` (string) are safely extracted.
- **Cleanup**:
  - Frees all heap strings and resets memory with `memset(resp, 0, sizeof(*resp))`.

---

## 4. Test Matrix

Add test cases to [`tests/unit/test_auth_client.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/tests/unit/test_auth_client.c):
1. **Request Builder Tests**:
   - Minimal request with `subject_token` and `subject_token_type`.
   - Full request with delegation (`actor_token`, `actor_token_type`, `resource`, `audience`, `scope`, `requested_token_type`).
   - Negative tests: missing `subject_token`, missing `subject_token_type`, un-paired `actor_token` without `actor_token_type`.
2. **Response Parser Tests**:
   - Complete valid JSON response matching RFC 8693 Section 2.2.2.
   - Negative tests: missing `access_token`, missing `issued_token_type`, missing `token_type`, malformed JSON.
   - Leak-free verification with `cleanup()`.
