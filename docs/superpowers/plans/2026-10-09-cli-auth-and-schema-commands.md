# MCP CLI Tooling Enhancement (Auth & Schema) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `schema validate` and `auth` (`pkce`, `token-exchange`) developer subcommands to `mcpkit-cli`.

**Architecture:** Implement input loader, `cmd_auth`, and `cmd_schema` handlers in `tools/mcpkit-cli/main.c` leveraging `mcpkit/core/auth.h` and `mcpkit/json/schema.h`. Add automated end-to-end acceptance tests to `tests/acceptance/cli_accept.sh`.

**Tech Stack:** C99/POSIX, CMake, CTest, shell acceptance script.

---

### Task 1: Implement `mcpkit-cli auth` Subcommands (`pkce` and `token-exchange`)

**Files:**
- Modify: `tools/mcpkit-cli/main.c`
- Modify: `tests/acceptance/cli_accept.sh`

- [ ] **Step 1: Write the failing acceptance tests**

In `tests/acceptance/cli_accept.sh`, add acceptance tests for `mcpkit-cli auth pkce` and `mcpkit-cli auth token-exchange`:

```bash
# auth pkce
out="$("$CLI" auth pkce)"
case "$out" in *code_verifier*code_challenge*) ;; *) echo "FAIL: auth pkce missing verifier/challenge"; exit 1 ;; esac

# auth token-exchange
out="$("$CLI" auth token-exchange "subject-token-123")"
case "$out" in *grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange*subject_token=subject-token-123*) ;; *) echo "FAIL: auth token-exchange failed"; exit 1 ;; esac
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R cli_acceptance --output-on-failure`  
Expected: FAIL because `auth` subcommand is unknown to `mcpkit-cli`.

- [ ] **Step 3: Implement `cmd_auth` in `tools/mcpkit-cli/main.c`**

1. Include `"mcpkit/core/auth.h"` in `tools/mcpkit-cli/main.c`.
2. Implement `cmd_auth(int argc, char **argv)`:
   - If `argc < 3`: print usage (`mcpkit-cli auth pkce`, `mcpkit-cli auth token-exchange ...`) and return 2.
   - If `strcmp(argv[2], "pkce") == 0`:
     - Create context, call `mcp_pkce_generate(ctx, verifier, challenge)`.
     - Print:
       ```
       code_verifier:  %s
       code_challenge: %s (method: S256)
       ```
     - Return 0.
   - If `strcmp(argv[2], "token-exchange") == 0`:
     - If `argc < 4`: print usage and return 2.
     - Extract `subject_token = argv[3]`.
     - Extract `subject_token_type = argc >= 5 ? argv[4] : MCP_OAUTH_TOKEN_TYPE_JWT`.
     - Extract `actor_token = argc >= 6 ? argv[5] : NULL`.
     - Extract `actor_token_type = argc >= 7 ? argv[6] : (actor_token ? MCP_OAUTH_TOKEN_TYPE_ACCESS_TOKEN : NULL)`.
     - Populate `mcp_oauth_token_exchange_req_t req`.
     - Call `mcp_oauth_build_token_exchange_request(ctx, &req, &body)`.
     - Print `body` to stdout, free `body`, return 0.
3. Wire `auth` into `main()` and update usage help text.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R cli_acceptance --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/mcpkit-cli/main.c tests/acceptance/cli_accept.sh
git commit -m "feat(cli): add auth pkce and token-exchange subcommands"
```

---

### Task 2: Implement `mcpkit-cli schema validate` Subcommand

**Files:**
- Modify: `tools/mcpkit-cli/main.c`
- Modify: `tests/acceptance/cli_accept.sh`

- [ ] **Step 1: Write the failing acceptance tests**

In `tests/acceptance/cli_accept.sh`, add acceptance tests for `mcpkit-cli schema validate`:

```bash
# schema validate (inline valid)
out="$("$CLI" schema validate '{"type":"string"}' '"hello"')"
case "$out" in *valid*) ;; *) echo "FAIL: schema validate valid inline failed"; exit 1 ;; esac

# schema validate (inline invalid)
set +e
out="$("$CLI" schema validate '{"type":"integer"}' '"hello"' 2>&1)"
rc=$?
set -e
if [ $rc -ne 1 ]; then echo "FAIL: schema validate invalid should return 1, got $rc"; exit 1; fi
case "$out" in *Validation failed*) ;; *) echo "FAIL: schema validate invalid output missing failure reason"; exit 1 ;; esac
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R cli_acceptance --output-on-failure`  
Expected: FAIL because `schema` subcommand is unknown to `mcpkit-cli`.

- [ ] **Step 3: Implement `load_json_input` and `cmd_schema` in `tools/mcpkit-cli/main.c`**

1. Include `"mcpkit/json/schema.h"` in `tools/mcpkit-cli/main.c`.
2. Implement `load_json_input(mcp_context_t *ctx, const char *arg, mcp_json_value_t **out_val)`:
   - Check if `arg` looks like inline JSON: begins with `{`, `[`, `"`, or is a number/bool. If so, attempt `mcp_json_parse`.
   - If not inline or if `fopen` succeeds: read file buffer up to 10MB, call `mcp_json_parse(ctx, buf, sz)`.
   - If file does not exist, fall back to parsing `arg` as JSON string.
3. Implement `cmd_schema(int argc, char **argv)`:
   - If `argc < 3` or `strcmp(argv[2], "validate") != 0`: print usage (`mcpkit-cli schema validate <schema> <instance>`) and return 2.
   - If `argc < 5`: print usage and return 2.
   - Load schema with `load_json_input(ctx, argv[3], &schema)`.
   - Load instance with `load_json_input(ctx, argv[4], &instance)`.
   - Call `mcp_schema_validate_verbose(ctx, schema, instance, err_buf, sizeof(err_buf))`.
   - If `MCP_OK`: print `"Instance is valid against schema.\n"` and return 0.
   - If failure: print `"Validation failed: %s\n"` to stderr and return 1.
   - Free JSON values and context before return.
4. Wire `schema` into `main()` and update usage help text.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R cli_acceptance --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/mcpkit-cli/main.c tests/acceptance/cli_accept.sh
git commit -m "feat(cli): add schema validate subcommand"
```

---

### Task 3: Documentation and Full Project Verification

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update CHANGELOG**

In `CHANGELOG.md`, document the new CLI subcommands under `[Unreleased]`:
- `mcpkit-cli auth pkce` and `mcpkit-cli auth token-exchange`
- `mcpkit-cli schema validate`

- [ ] **Step 2: Run full project test suite**

Run: `ctest --test-dir build --output-on-failure`  
Expected: All 59 tests pass.

- [ ] **Step 3: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs: document mcpkit-cli auth and schema subcommands in CHANGELOG"
```
