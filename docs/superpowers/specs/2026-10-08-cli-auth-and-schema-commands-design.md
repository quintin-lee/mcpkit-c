# MCP CLI Tooling Enhancement: Auth & Schema Subcommands Design

**Date:** 2026-10-08  
**Status:** Approved  
**Author:** Antigravity  
**Topic:** Developer CLI tooling enhancement for OAuth authentication & JSON Schema inspection

---

## 1. Background & Motivation

[`tools/mcpkit-cli/main.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/tools/mcpkit-cli/main.c) provides a developer CLI tool for inspecting and interacting with MCP servers and manifests (`discover`, `inspect`, `call`, `listen`, `manifest init`, `manifest validate`, `registry search`, `registry info`, `validate`, `test`).

With recent MCP 2026-07-28 features—specifically RFC 7636 S256 PKCE, RFC 8693 OAuth 2.0 Token Exchange (SEP-990), and JSON Schema validation for tool input/output schemas (SEP-2106, SEP-1613)—developers need quick command-line utilities to:
1. Generate test PKCE code verifier and code challenge pairs.
2. Build RFC 8693 Token Exchange POST request bodies for service token delegation/impersonation debugging.
3. Validate JSON instances against JSON Schema definitions with descriptive path-level diagnostic messages.

---

## 2. Command Specifications

### 2.1 `mcpkit-cli schema validate <schema> <instance>`

- **Syntax:**
  ```bash
  mcpkit-cli schema validate <schema-file-or-json> <instance-file-or-json>
  ```
- **Input Loading Behavior:**
  - Employs a hybrid loader:
    1. If the argument string starts with `{` or `[`, parse directly as inline JSON string.
    2. Otherwise, attempt `fopen()` on the argument as a filesystem path. If file exists, read its contents and parse as JSON.
    3. If `fopen()` fails (e.g. file does not exist), attempt parsing the argument directly as an inline JSON string (e.g. `"\"hello\""` or `123`).
  - If parsing fails for either schema or instance, output `"mcpkit-cli: failed to parse <schema|instance> as JSON\n"` to stderr and exit with code `2`.
- **Validation:**
  - Invokes `mcp_schema_validate_verbose(ctx, schema, instance, err_buf, sizeof(err_buf))`.
  - **Success (exit code 0):**
    ```
    Instance is valid against schema.
    ```
  - **Failure (exit code 1):**
    ```
    Validation failed: <err_buf>
    ```

### 2.2 `mcpkit-cli auth pkce`

- **Syntax:**
  ```bash
  mcpkit-cli auth pkce
  ```
- **Behavior:**
  - Invokes `mcp_pkce_generate(ctx, verifier, challenge)`.
  - Outputs formatted verifier and challenge (exit code 0):
    ```
    code_verifier:  <64-char verifier>
    code_challenge: <43-char challenge> (method: S256)
    ```

### 2.3 `mcpkit-cli auth token-exchange <subject-token> [subject-token-type] [actor-token] [actor-token-type]`

- **Syntax:**
  ```bash
  mcpkit-cli auth token-exchange <subject-token> [subject-token-type] [actor-token] [actor-token-type]
  ```
- **Behavior:**
  - If `<subject-token>` is missing, output usage and exit with code `2`.
  - Default `subject-token-type` to `MCP_OAUTH_TOKEN_TYPE_JWT` (`"urn:ietf:params:oauth:token-type:jwt"`).
  - If `actor-token` is provided and `actor-token-type` is omitted, default `actor-token-type` to `MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN` (`"urn:ietf:params:oauth:token-type:access_token"`).
  - Calls `mcp_oauth_build_token_exchange_request(ctx, &req, &body)`.
  - Outputs the generated `application/x-www-form-urlencoded` body to stdout (exit code 0):
    ```
    grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange&subject_token=...&subject_token_type=...
    ```

---

## 3. Architecture & Code Structure

All changes are contained in [`tools/mcpkit-cli/main.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/tools/mcpkit-cli/main.c):
1. Helper function `load_json_input(mcp_context_t *ctx, const char *arg, mcp_json_value_t **out_val)`:
   - Reads from file if available, or falls back to inline string parsing.
2. `cmd_schema(int argc, char **argv)`:
   - Handles `schema validate` dispatching.
3. `cmd_auth(int argc, char **argv)`:
   - Handles `auth pkce` and `auth token-exchange` dispatching.
4. Top-level `main()` dispatch:
   - Recognizes `schema` and `auth` subcommands.
   - Updates `usage` banner with the new commands.

---

## 4. Testing & Verification

1. **Acceptance Test Suite:**
   - In [`tests/acceptance/cli_accept.sh`](file:///data/home/quintin/workspace/source/c/mcpkit-c/tests/acceptance/cli_accept.sh), add automated test cases:
     - `mcpkit-cli auth pkce` format check.
     - `mcpkit-cli auth token-exchange my-id-token` output verification.
     - `mcpkit-cli schema validate` with valid and invalid inline JSON.
     - `mcpkit-cli schema validate` with file inputs.
2. **Full Project Suite:**
   - Run `ctest --test-dir build --output-on-failure` (all tests passing including `cli_acceptance`).

---

## 5. Security & Safety

- 100% C99/C23 zero-external-dependencies.
- Bounds-checked file reading (limit file size to 10MB).
- No memory leaks: all allocated JSON structures, contexts, and strings are cleaned up on all exit paths.
