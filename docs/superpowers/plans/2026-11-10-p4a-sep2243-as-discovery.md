# P4-a Implementation Plan: SEP-2243 x-mcp-header + RFC 9728 AS Discovery

**Date:** 2026-11-10
**Spec:** `docs/superpowers/specs/2026-11-10-mcp-p4a-sep2243-as-discovery-design.md`
**Scope:** 4 slices (S1 registry validation, S4 AS discovery data-plane, S2 streamable_http header check, S3 client mirroring) + base64 tool + policy gate + error code.

## Key Findings from Codebase Exploration (must reflect in code)

1. **S1 add_tool lives in `server.c`** (`mcp_server_add_tool` L261), NOT `registry.c`. Registry only has object constructors. S1 validation inserts at the top of `mcp_server_add_tool` (after null check, before `mcp_tool_new`).
2. **Client has NO schema cache.** `mcp_client_t` (client/internals.h) only holds `ctx/t/next_id/version/bearer_token/callbacks`. S3 MUST add `mcp_client_set_tool_schema` + `schema_cache` field (a small name->inputSchema map, one entry per `tools/list` result; deep-clone the JSON).
3. **Client HTTP headers are set via `mcp_http_request_set_mcp_metadata`** (http.c:424) which sets `MCP-Protocol-Version`/`Mcp-Method`/`Mcp-Name`. S3's `Mcp-Param-*` mirroring MUST extend this function (or add a sibling `mcp_http_request_set_xmcp_param`) called from the client's HTTP transport request-construction path — NOT from `client.c` internals (client.c only has `mcp_transport_send`, the line-based send; the HTTP header layer is in the client's HTTP transport glue).
4. **S2 insertion point** is in `handle_post` (streamable_http.c:303) at the `is_2026` branch (L407-411) — AFTER session resolution (L417-463) and BEFORE dispatch (L472-473). The 2026 request's `Mcp-Param-*` headers are read via `mcp_http_header(ctx, req, "Mcp-Param-" + Name)`; tool name from `Mcp-Name` header (L212 pattern) or `Mcp-Method` = "tools/call" + `Mcp-Name`. The server-side tool schema is accessed via `mcp_server_get_tool_schema(ctx, server, tool_name)` (new helper to be added to server.c, returning the inputSchema JSON tree).
5. **`-32001` = `MCP_RPC_XMCP_PARAM_MISMATCH`** (new enum in message.h, alongside existing `MCP_RPC_HEADER_MISMATCH=-32020` for P2 standard headers). Do NOT rename or change -32020.
6. **`mcp_oauth_discovery_merge`** is at auth.c:432, signature: `(ctx, const mcp_oauth_metadata_t *oidc, const mcp_oauth_metadata_t *as, mcp_oauth_metadata_t *out)`. S4 reuses it directly.
7. **`mcp_oauth_metadata_parse`** parses AS/OIDC metadata JSON (used in P3 T5). S4 reuses it for the AS discovery JSON.
8. **Policy signature** is `(ctx, session)` only (no meta param) — consistent with P1-P3. `mcp_proto_policy_sep2243_active` follows this.
9. **Test framework** (test_http_serve.c): `mem_io_t` + `make_server` + `run_script` pattern. 2026 requests need `MCP-Protocol-Version: 2026-07-28` header + `Mcp-Method` + body `protocolVersion` consistency (P2 strict validation, -32020/-32602 gate before auth).

## Task 0: Shared foundation — base64 + error code + policy gate (core library, no HTTP gate)

**Files:**
- `include/mcpkit/protocol/message.h` — add `MCP_RPC_XMCP_PARAM_MISMATCH = -32001`
- `include/mcpkit/protocol/policy.h` — add `bool mcp_proto_policy_sep2243_active(const mcp_context_t *ctx, const mcp_session_t *session);`
- `src/protocol/policy.c` — implement (return `mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2026_07_28)`)
- `include/mcpkit/core/auth.h` — add `char *mcp_base64_encode(mcp_context_t *ctx, const uint8_t *in, size_t in_len);` and `int mcp_base64_decode(mcp_context_t *ctx, const char *in, size_t in_len, uint8_t **out, size_t *out_len);`
- `src/core/auth.c` — implement `mcp_base64_encode/decode` (RFC 4648 std base64, with `=` padding)

**Tests:** `tests/unit/test_base64_sep2243.c` (new, core library, not HTTP-gated):
- base64 encode/decode roundtrip (binary bytes incl. `\x00`/`\xff`)
- base64 decode rejects non-base64 chars, bad padding
- `mcp_proto_policy_sep2243_active` true at 2026-07-28, false at 2025-11-25, false at 2025-06-18
- `-32001` enum value is distinct from `-32020` (compile-time assert or test)

```
TDD RED: new test fails (base64 not implemented / policy fn missing)
TDD GREEN: implement, all pass
```

**Commit:** `P4-a T0: base64 std codec + SEP-2243 policy gate + -32001 error code`

---

## Task 1: S1 — registry add_tool `x-mcp-header` annotation validation

**Files:**
- `src/server/server.c` — in `mcp_server_add_tool` (L261), before `mcp_tool_new`, walk `tool->schema` recursively for `x-mcp-header` annotations; validate per spec (tchar, no CR/LF, case-insensitive uniqueness, type ∈ {string,integer,boolean} — union types pass if ANY allowed type present); on violation return `MCP_ERR_INVALID_ARGUMENT` WITHOUT registering.
- Add a static helper `xmcp_validate_schema(mcp_context_t *ctx, const mcp_json_value_t *schema)` that does the recursive walk (in server.c or a new `src/server/xmcp.c`).

**Tests:** `tests/unit/test_sep2243_xmcp_header.c` §1 (core library, not HTTP-gated):
- number-type `x-mcp-header` → `mcp_server_add_tool` returns `INVALID_ARGUMENT`
- valid string/integer/boolean → `add_tool` returns `MCP_OK`
- union type `["string","integer"]` → passes (any allowed type present)
- `type:"number"` → rejects
- duplicate `x-mcp-header` names (case-insensitive) → rejects
- CR/LF in annotation value → rejects
- nested (depth 3+) `x-mcp-header` → found
- no `x-mcp-header` in schema → zero overhead, registers (zero regression)

```
TDD RED: invalid add_tool (number type) currently succeeds; test asserts failure → red
TDD GREEN: implement validation → all pass
```

**Commit:** `P4-a T1: x-mcp-header annotation validation in mcp_server_add_tool`

---

## Task 2: S4 — AS auto-discovery (data-plane, core library, no HTTP gate)

**Files:**
- `include/mcpkit/core/auth.h` — add `mcp_http_fetch_fn` typedef + `mcp_oauth_discover_as` declaration
- `src/core/auth.c` — implement `mcp_oauth_discover_as`:
  1. NULL check `pr_meta` and `pr_meta->authorization_server` → `MCP_ERR_INVALID_ARGUMENT`
  2. Build RFC 8414 §3.1 probe URL: issuer minus trailing `/` + `/.well-known/oauth-authorization-server`
  3. Call `fetch(fetch_ud, url, &body, &body_len)`; on failure (fetch returns -1) → `MCP_ERR_PROTOCOL`
  4. `mcp_oauth_metadata_parse(ctx, body, &parsed)` on failure → `MCP_ERR_PROTOCOL`
  5. If `existing_as != NULL` → `mcp_oauth_discovery_merge(ctx, existing_as, &parsed, out_merged)`; else `*out_merged = parsed`
  6. Free `body` via `mcp_oauth_free_string(ctx, body)`
  7. Return `MCP_OK`

**Tests:** `tests/unit/test_sep2243_xmcp_header.c` §2 (or `test_oauth_discovery_as.c`, core library):
- `pr_meta->authorization_server = "https://as.example.com"` → probe URL = `https://as.example.com/.well-known/oauth-authorization-server`
- fetch returns valid AS JSON → `out_merged.token_endpoint` set, `out_merged.authorization_endpoint` set
- fetch returns NULL (NULL fetch) → `MCP_ERR_PROTOCOL`
- `existing_as` non-NULL → merge precedence (AS fields win over OIDC-specific)
- `existing_as` NULL → direct parse, no merge
- probe URL with trailing slash issuer `https://as.example.com/` → still correct (strip slash before concat)

```
TDD RED: mcp_oauth_discover_as not implemented → test red
TDD GREEN: implement → all pass
```

**Commit:** `P4-a T2: RFC 9728 AS auto-discovery (data-plane URL probe + host fetch + merge)`

---

## Task 3: S2 — streamable_http `Mcp-Param-*` header consistency check (HTTP-gated, 2026 only)

**Files:**
- `src/transport/streamable_http.c` — in `handle_post` at the `is_2026` branch (L407-411), AFTER session resolution (L417-463) and BEFORE `mcp_server_dispatch` (L472), insert the `Mcp-Param-*` consistency check:
  1. Read `Mcp-Method` + `Mcp-Name` headers (existing pattern L211-212); if `Mcp-Method == "tools/call"` and `Mcp-Name` present, get tool name
  2. Call new `mcp_server_get_tool_schema(ctx, server, tool_name)` (added to server.c, returns the registered tool's inputSchema JSON tree, NULL if not found)
  3. Walk the schema for `x-mcp-header` annotations (reuse S1's walker, exposed as a query API)
  4. For each annotation: read `Mcp-Param-{Name}` header via `mcp_http_header`; if body has a value but header is missing → 400 + `-32001`; if header present, decode base64 sentinel if needed, compare with body value (numeric comparison for integer, string equality for string/boolean); mismatch → 400 + `-32001`
  5. Gate: only when `mcp_proto_policy_sep2243_active(ctx, sess)` is true (i.e. `is_2026` AND session negotiated ≥ 2026-07-28); otherwise skip entirely (zero regression)
- `include/mcpkit/server/server.h` — add `mcp_server_get_tool_schema` + `mcp_server_get_tool_xmcp_annotations` (returns list of annotation names for the tool, for the header-reading loop)

**Tests:** `tests/unit/test_http_mcp_param.c` §1 (HTTP-gated):
- 2026 request: `Mcp-Param-Region` header matches body `region` value → 200
- 2026 request: header value differs from body value → 400 + `-32001`
- 2026 request: body has `region` value but no `Mcp-Param-Region` header → 400 + `-32001`
- 2026 request: base64 sentinel `=?base64?dGVzdA==?=` in header decodes to `test`, body has `test` → 200
- 2025-11-25 request: same payload, `Mcp-Param-Region` header present but NOT checked → 200 (no -32001, zero regression)
- 2026 request, tool not in registry → schema NULL → skip check → 200 (or dispatch handles it)

```
TDD RED: 2026 request with mismatched Mcp-Param-* header currently returns 200 (no check); test asserts 400/-32001 → red
TDD GREEN: implement check → all pass
```

**Commit:** `P4-a T3: streamable_http Mcp-Param-* consistency check (2026 only, -32001)`

---

## Task 4: S3 — client `Mcp-Param-*` mirroring (HTTP-gated, 2026 only)

**Files:**
- `include/mcpkit/client/client.h` — add `mcp_client_set_tool_schema` declaration
- `src/client/internals.h` — add `mcp_tool_schema_cache_t *schema_cache` field to `mcp_client`
- `src/client/client.c` — implement:
  - `mcp_client_set_tool_schema(ctx, client, tool_name, input_schema)` — deep-clone `input_schema`, insert into `client->schema_cache` (grow-on-demand array of `{char *tool_name; mcp_json_value_t *schema;}` entries)
  - `mcp_client_call_tool` / `mcp_client_call_tool_mrtr` — BEFORE sending the request, check `client->schema_cache` for the tool name; if found, walk it for `x-mcp-header` annotations; for each annotated param, extract the value from `params`, apply the 5-step encoding (type conversion → leading/trailing whitespace check → ASCII check → control char check → base64 sentinel if needed), set `Mcp-Param-{Name}` header on the outgoing HTTP request via the HTTP transport layer
  - If `schema_cache` is NULL or tool not found → do NOT set `Mcp-Param-*` headers (spec: no-schema clients SHOULD not send them)
  - Gate: only when `client->version` ≥ 2026-07-28 (client-side gate); ≤ 2025-11-25 → skip mirroring entirely
- `src/transport/http.c` — add `mcp_http_request_set_xmcp_param(ctx, req, const char *name, const char *value)` that sets the `Mcp-Param-{Name}` header (sibling to `set_mcp_metadata`)

**Tests:** `tests/unit/test_http_mcp_param.c` §2 (HTTP-gated):
- `mcp_client_set_tool_schema` with schema containing `x-mcp-header: "Region"` on `region` param
- `mcp_client_call_tool` with `region = "us-east-1"` → outgoing request has `Mcp-Param-Region: us-east-1` header
- `region = "  us-east-1  "` (leading/trailing whitespace) → header value is `=?base64?ICA...?=` (base64 sentinel, UTF-8 encoded `us-east-1` with spaces)
- `region = null` → no `Mcp-Param-Region` header set
- `region` not in params → no `Mcp-Param-Region` header set
- No `mcp_client_set_tool_schema` called (schema_cache NULL) → no `Mcp-Param-*` headers
- 2025-11-25 client (version gate) → no mirroring even with schema set

```
TDD RED: client call_tool with x-mcp-header param currently sends no Mcp-Param-* header; test asserts header present → red
TDD GREEN: implement mirroring → all pass
```

**Commit:** `P4-a T4: client Mcp-Param-* mirroring (schema cache + 5-step encoding + HTTP header)`

---

## Task 5: Documentation + final regression

**Files:**
- `docs/module-reference.md` — add sections for: `auth.h` (base64 + `mcp_oauth_discover_as` + `mcp_http_fetch_fn`), `policy.h` (`mcp_proto_policy_sep2243_active`), `server.h` (`mcp_server_get_tool_schema` + `mcp_server_get_tool_xmcp_annotations`), `client.h` (`mcp_client_set_tool_schema`), `streamable_http.c` (S2 check description), `http.c` (`mcp_http_request_set_xmcp_param`)

**Verification:**
- Full `ctest` Default + Release (all existing tests pass, no regression)
- ASan build (compile + link clean)
- `grep` all new public symbols to confirm they exist in source

**Commit:** `P4-a T5: module-reference.md P4-a API surface + full regression`

---

## Execution Order & Dependencies

```
Task 0 (base64 + policy + -32001)   ← no deps, pure core
  ↓
Task 1 (S1 registry validation)     ← depends on Task 0 (uses error code)
  ↓
Task 2 (S4 AS discovery)            ← depends on Task 0 (base64 for sentinel decode in merge,
                                       P3 T5 discovery_merge already exists)
  ↓
Task 3 (S2 http header check)       ← depends on Task 0 + Task 1 (tool schema query API)
  ↓
Task 4 (S3 client mirroring)        ← depends on Task 0 (base64 encoding) + Task 1 (schema
                                       annotations for the client-side walk)
  ↓
Task 5 (docs + regression)          ← depends on all
```

Each task is independently shippable: Task 0+1+2 deliver a core-library slice (no HTTP gate, `MCPKIT_BUILD_HTTP` not required); Tasks 3+4 deliver the HTTP-gated slice. Task 5 is documentation + regression only.

## Test Registration (CMakeLists.txt)

New tests to register in `tests/CMakeLists.txt`:
- `test_base64_sep2243.c` → `add_test(NAME base64_sep2243 ...)` (core library, always built)
- `test_sep2243_xmcp_header.c` → `add_test(NAME sep2243_xmcp_header ...)` (core library, always built; covers S1 + S4)
- `test_http_mcp_param.c` → `add_test(NAME http_mcp_param ...)` inside `if(MCPKIT_BUILD_HTTP)` block (S2 + S3)

## Build Verification (per task)

After each task's TDD GREEN:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j 2>&1 | grep -E "warning|error"
ctest --test-dir build -R <new_test_name> --output-on-failure
```
Final (Task 5):
```bash
ctest -C Debug --test-dir build 2>&1 | grep -E "tests passed|tests failed|Total"
ctest -C Release --test-dir build-release 2>&1 | grep -E "tests passed|tests failed|Total"
```
`-Wall -Wextra` zero warnings in both builds.
