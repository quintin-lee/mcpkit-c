# RFC 8693 Token Exchange Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement RFC 8693 OAuth 2.0 Token Exchange request builders, response parsers, and standard URN constants in `mcpkit-c` for Enterprise Managed Authorization (SEP-990).

**Architecture:** Add standard RFC 8693 token type and grant type URN macros along with `mcp_oauth_token_exchange_req_t` and `mcp_oauth_token_exchange_response_t` in `include/mcpkit/core/auth.h`. Implement `mcp_oauth_build_token_exchange_request()`, `mcp_oauth_token_exchange_response_parse()`, and cleanup in `src/core/auth.c` with parameter validation and URL-encoding.

**Tech Stack:** Pure C99/C23, CMake, CTest, internal JSON parsing API.

---

### Task 1: RFC 8693 Constants and Data Structures

**Files:**
- Modify: `include/mcpkit/core/auth.h:20-50`
- Test: `tests/unit/test_auth_client.c:1-30`

- [x] **Step 1: Write the failing test**

In `tests/unit/test_auth_client.c`, add an assertion checking the presence of the RFC 8693 grant type macro:

```c
    /* RFC 8693 URN constants check */
    CHECK(strcmp(MCP_OAUTH_GRANT_TYPE_TOKEN_EXCHANGE, "urn:ietf:params:oauth:grant-type:token-exchange") == 0);
    CHECK(strcmp(MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN, "urn:ietf:params:oauth:token-type:access_token") == 0);
```

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R test_auth_client --output-on-failure`  
Expected: Compilation failure due to undeclared identifier `MCP_OAUTH_GRANT_TYPE_TOKEN_EXCHANGE`.

- [x] **Step 3: Define constants and structures in header**

In `include/mcpkit/core/auth.h`, add:

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

typedef struct mcp_oauth_token_exchange_req {
    const char *subject_token;
    const char *subject_token_type;
    const char *actor_token;
    const char *actor_token_type;
    const char *resource;
    const char *audience;
    const char *scope;
    const char *requested_token_type;
} mcp_oauth_token_exchange_req_t;

typedef struct mcp_oauth_token_exchange_response {
    char *access_token;
    char *issued_token_type;
    char *token_type;
    uint32_t expires_in;
    char *refresh_token;
    char *scope;
} mcp_oauth_token_exchange_response_t;
```

- [x] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_auth_client --output-on-failure`  
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add include/mcpkit/core/auth.h tests/unit/test_auth_client.c
git commit -m "feat(auth): declare RFC 8693 Token Exchange constants and structures"
```

---

### Task 2: Implement Token Exchange Request Builder

**Files:**
- Modify: `include/mcpkit/core/auth.h:200-245`
- Modify: `src/core/auth.c:480-550`
- Test: `tests/unit/test_auth_client.c`

- [x] **Step 1: Write the failing test**

In `tests/unit/test_auth_client.c`, add tests for `mcp_oauth_build_token_exchange_request`:

```c
    /* Minimal Token Exchange request */
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
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &bad_req, &ex_body) == MCP_ERR_INVALID_ARGUMENT); // missing subject_token_type
    bad_req.subject_token_type = MCP_OAUTH_TOKEN_TYPE_JWT;
    bad_req.actor_token = "actor"; // actor_token without actor_token_type
    CHECK(mcp_oauth_build_token_exchange_request(ctx, &bad_req, &ex_body) == MCP_ERR_INVALID_ARGUMENT);
```

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R test_auth_client --output-on-failure`  
Expected: Compilation failure due to undeclared function `mcp_oauth_build_token_exchange_request`.

- [x] **Step 3: Implement request builder**

Declare `mcp_oauth_build_token_exchange_request` in `include/mcpkit/core/auth.h` and implement in `src/core/auth.c`:
- Validate `req != NULL && body_out != NULL`.
- Validate `req->subject_token != NULL && req->subject_token[0] != '\0'`.
- Validate `req->subject_token_type != NULL && req->subject_token_type[0] != '\0'`.
- If `req->actor_token != NULL && req->actor_token[0] != '\0'`:
  - Validate `req->actor_token_type != NULL && req->actor_token_type[0] != '\0'`.
- URL-encode all fields and assemble `application/x-www-form-urlencoded` string starting with `grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange`.

- [x] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_auth_client --output-on-failure`  
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add include/mcpkit/core/auth.h src/core/auth.c tests/unit/test_auth_client.c
git commit -m "feat(auth): implement RFC 8693 Token Exchange request builder"
```

---

### Task 3: Implement Token Exchange Response Parser and Cleanup

**Files:**
- Modify: `include/mcpkit/core/auth.h:230-245`
- Modify: `src/core/auth.c:550-620`
- Test: `tests/unit/test_auth_client.c`

- [x] **Step 1: Write the failing test**

In `tests/unit/test_auth_client.c`, add tests for `mcp_oauth_token_exchange_response_parse` and cleanup:

```c
    /* Valid Token Exchange response */
    const char *resp_json =
        "{\"access_token\":\"eyJhbGciOi...\",\"issued_token_type\":\"urn:ietf:params:oauth:token-type:access_token\","
        "\"token_type\":\"Bearer\",\"expires_in\":3600,\"scope\":\"tools:read\",\"refresh_token\":\"r-12345\"}";
    mcp_oauth_token_exchange_response_t ex_resp;
    memset(&ex_resp, 0, sizeof(ex_resp));
    CHECK(mcp_oauth_token_exchange_response_parse(ctx, resp_json, strlen(resp_json), &ex_resp) == MCP_OK);
    CHECK(strcmp(ex_resp.access_token, "eyJhbGciOi...") == 0);
    CHECK(strcmp(ex_resp.issued_token_type, "urn:ietf:params:oauth:token-type:access_token") == 0);
    CHECK(strcmp(ex_resp.token_type, "Bearer") == 0);
    CHECK(ex_resp.expires_in == 3600);
    CHECK(strcmp(ex_resp.scope, "tools:read") == 0);
    CHECK(strcmp(ex_resp.refresh_token, "r-12345") == 0);
    mcp_oauth_token_exchange_response_cleanup(ctx, &ex_resp);
    CHECK(ex_resp.access_token == NULL);

    /* Negative response parsing tests */
    const char *missing_issued = "{\"access_token\":\"abc\",\"token_type\":\"Bearer\"}";
    CHECK(mcp_oauth_token_exchange_response_parse(ctx, missing_issued, strlen(missing_issued), &ex_resp) == MCP_ERR_PROTOCOL);

    const char *missing_access = "{\"issued_token_type\":\"urn:ietf:params:oauth:token-type:access_token\",\"token_type\":\"Bearer\"}";
    CHECK(mcp_oauth_token_exchange_response_parse(ctx, missing_access, strlen(missing_access), &ex_resp) == MCP_ERR_PROTOCOL);

    const char *bad_json = "not-json";
    CHECK(mcp_oauth_token_exchange_response_parse(ctx, bad_json, strlen(bad_json), &ex_resp) == MCP_ERR_PROTOCOL);
```

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R test_auth_client --output-on-failure`  
Expected: Compilation failure due to undeclared function `mcp_oauth_token_exchange_response_parse`.

- [x] **Step 3: Implement response parser and cleanup**

Declare in `include/mcpkit/core/auth.h` and implement in `src/core/auth.c`:
- Validate `json_str != NULL && resp_out != NULL`.
- Parse JSON root object with `mcp_json_parse`.
- Validate required fields: `access_token`, `issued_token_type`, `token_type` are non-null string values.
- Extract optional fields: `expires_in` (number), `scope` (string), `refresh_token` (string).
- Implement `mcp_oauth_token_exchange_response_cleanup()` to free all allocated strings and `memset(resp, 0, sizeof(*resp))`.

- [x] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_auth_client --output-on-failure`  
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add include/mcpkit/core/auth.h src/core/auth.c tests/unit/test_auth_client.c
git commit -m "feat(auth): implement RFC 8693 Token Exchange response parser and cleanup"
```

---

### Task 4: Documentation and Full Project Verification

**Files:**
- Modify: `CHANGELOG.md`

- [x] **Step 1: Update CHANGELOG**

In `CHANGELOG.md`, document RFC 8693 OAuth 2.0 Token Exchange support under `[Unreleased]` with references to SEP-990 and RFC 8693.

- [x] **Step 2: Run full project test suite**

Run: `ctest --test-dir build --output-on-failure`  
Expected: 59/59 tests pass (100% pass rate).

- [x] **Step 3: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs: document RFC 8693 Token Exchange additions in CHANGELOG"
```
