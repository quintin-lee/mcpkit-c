# 路线 A：客户端 OAuth 2.1 完整鉴权闭环 (OAuth 2.1 Client Authorization Flow & Token Management) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement end-to-end OAuth 2.1 client authorization in `mcpkit-c`, including token response parsing, PKCE authorization code exchange request construction, refresh token rotation (SEP-2207), Client Credentials flow (SEP-1046), and dynamic client registration metadata (SEP-991).

**Architecture:** Build on the existing PKCE S256 and RFC 8414 metadata parser in `auth.h`/`auth.c`. Add token response data structures, request builders for code exchange, token refresh, and M2M client credentials, and wire a dynamic Bearer token injector into `mcp_client_t` to automatically attach authorization headers or respond to 401 challenges.

**Tech Stack:** C99, CMake, CTest, ASan/UBSan.

---

### File Structure Map

- `include/mcpkit/core/auth.h`: Add OAuth 2.1 token response struct, token request payload builders, and client credentials builders.
- `src/core/auth.c`: Implement payload generation, JSON parsing, URL-encoding helpers, and validation.
- `include/mcpkit/client/client.h`: Add client Bearer token accessors and token refresh hook.
- `src/client/client.c`: Store client Bearer token and inject into outbound requests/transports.
- `tests/unit/test_auth_client.c`: Full unit tests for token builders, response parsing, error handling, and client integration.
- `tests/CMakeLists.txt`: Register `test_auth_client`.

---

### Task A1: OAuth Token Response Parser & Token Request Builders

**Files:**
- Modify: `include/mcpkit/core/auth.h`
- Modify: `src/core/auth.c`
- Create: `tests/unit/test_auth_client.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing unit test in `tests/unit/test_auth_client.c`**

```c
#include <stdio.h>
#include <string.h>
#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. Parse valid token response
    const char *json_ok = "{\"access_token\":\"acc-123\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"ref-456\",\"scope\":\"read write\"}";
    mcp_oauth_token_response_t token;
    memset(&token, 0, sizeof(token));
    CHECK(mcp_oauth_token_response_parse(ctx, json_ok, strlen(json_ok), &token) == MCP_OK);
    CHECK(token.access_token != NULL && strcmp(token.access_token, "acc-123") == 0);
    CHECK(token.token_type != NULL && strcmp(token.token_type, "Bearer") == 0);
    CHECK(token.expires_in == 3600);
    CHECK(token.refresh_token != NULL && strcmp(token.refresh_token, "ref-456") == 0);
    CHECK(token.scope != NULL && strcmp(token.scope, "read write") == 0);
    mcp_oauth_token_response_cleanup(ctx, &token);

    // 2. Build authorization code token request with PKCE
    char *req_body = NULL;
    CHECK(mcp_oauth_build_token_request_pkce(ctx, "auth-code-xyz", "verifier-abc", "https://app/callback", "client-1", &req_body) == MCP_OK);
    CHECK(req_body != NULL);
    CHECK(strstr(req_body, "grant_type=authorization_code") != NULL);
    CHECK(strstr(req_body, "code=auth-code-xyz") != NULL);
    CHECK(strstr(req_body, "code_verifier=verifier-abc") != NULL);
    CHECK(strstr(req_body, "client_id=client-1") != NULL);
    mcp_context_free(ctx, req_body);

    // 3. Build refresh token request (SEP-2207)
    char *ref_body = NULL;
    CHECK(mcp_oauth_build_refresh_request(ctx, "ref-456", "client-1", "read", &ref_body) == MCP_OK);
    CHECK(ref_body != NULL);
    CHECK(strstr(ref_body, "grant_type=refresh_token") != NULL);
    CHECK(strstr(ref_body, "refresh_token=ref-456") != NULL);
    CHECK(strstr(ref_body, "scope=read") != NULL);
    mcp_context_free(ctx, ref_body);

    // 4. Build client credentials request (SEP-1046)
    char *cc_body = NULL;
    CHECK(mcp_oauth_build_client_credentials_request(ctx, "client-1", "secret-key", "admin", &cc_body) == MCP_OK);
    CHECK(cc_body != NULL);
    CHECK(strstr(cc_body, "grant_type=client_credentials") != NULL);
    CHECK(strstr(cc_body, "client_id=client-1") != NULL);
    CHECK(strstr(cc_body, "client_secret=secret-key") != NULL);
    mcp_context_free(ctx, cc_body);

    mcp_context_destroy(ctx);
    printf("test_auth_client OK\n");
    return 0;
}
```

- [ ] **Step 2: Add executable to `tests/CMakeLists.txt` and verify compile failure**

Add:
```cmake
add_executable(test_auth_client unit/test_auth_client.c)
target_link_libraries(test_auth_client PRIVATE mcpkit_core)
add_test(NAME test_auth_client COMMAND test_auth_client)
```
Run: `cmake --build build --target test_auth_client`
Expected: Compile failure due to missing token functions in `auth.h`.

- [ ] **Step 3: Define structs and APIs in `include/mcpkit/core/auth.h`**

```c
typedef struct {
    char *access_token;
    char *token_type;
    uint32_t expires_in;
    char *refresh_token;
    char *scope;
} mcp_oauth_token_response_t;

mcp_status_t mcp_oauth_token_response_parse(mcp_context_t *ctx, const char *json, size_t len,
                                            mcp_oauth_token_response_t *resp_out);
void mcp_oauth_token_response_cleanup(mcp_context_t *ctx, mcp_oauth_token_response_t *resp);

mcp_status_t mcp_oauth_build_token_request_pkce(mcp_context_t *ctx, const char *code,
                                                const char *code_verifier, const char *redirect_uri,
                                                const char *client_id, char **body_out);

mcp_status_t mcp_oauth_build_refresh_request(mcp_context_t *ctx, const char *refresh_token,
                                             const char *client_id, const char *scope,
                                             char **body_out);

mcp_status_t mcp_oauth_build_client_credentials_request(mcp_context_t *ctx, const char *client_id,
                                                        const char *client_secret, const char *scope,
                                                        char **body_out);
```

- [ ] **Step 4: Implement functions in `src/core/auth.c`**

Implement URL encoding, query string assembly, JSON parsing, and memory management.

- [ ] **Step 5: Run tests and verify PASS**

Run: `cmake --build build --target test_auth_client && ./build/tests/test_auth_client`
Expected: `test_auth_client OK`

- [ ] **Step 6: Commit**

```bash
git add include/mcpkit/core/auth.h src/core/auth.c tests/unit/test_auth_client.c tests/CMakeLists.txt
git commit -m "feat(auth): add OAuth 2.1 token response parsing and request builders"
```

---

### Task A2: Client Bearer Token Management & Auto-Refresh Hook

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `src/client/client.c`
- Modify: `tests/unit/test_auth_client.c`

- [ ] **Step 1: Write test for client token management**

Add test case in `tests/unit/test_auth_client.c`:
Verify `mcp_client_set_bearer_token()` stores and retrieves the Bearer token, and that headers or transport meta are decorated with `Authorization: Bearer <token>`.

- [ ] **Step 2: Declare token APIs in `include/mcpkit/client/client.h`**

```c
mcp_status_t mcp_client_set_bearer_token(mcp_context_t *ctx, mcp_client_t *client,
                                         const char *bearer_token);
const char *mcp_client_get_bearer_token(mcp_context_t *ctx, const mcp_client_t *client);
```

- [ ] **Step 3: Implement in `src/client/client.c`**

Add `bearer_token` string to `struct mcp_client` and cleanup upon destruction.

- [ ] **Step 4: Run test suite & verify**

Run: `cmake --build build --target test_auth_client && ./build/tests/test_auth_client`
Expected: PASS with 0 memory errors.

- [ ] **Step 5: Commit**

```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_auth_client.c
git commit -m "feat(client): add bearer token storage and accessors"
```
