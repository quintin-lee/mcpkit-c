# MCP 2026-07-28 Full Conformance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the remaining official MCP 2026-07-28 specification capabilities and SEP extensions in mcpkit-c: Content Annotations (SEP-1249), Client Roots Changed Active Notification, RFC 6570 URI Templates Extraction Engine, Streamable HTTP SSE Parser & Last-Event-ID Reconnection, and RFC 7636 PKCE / OAuth 2.1 Metadata Utilities.

**Architecture:** Maintain strict pure C99/C23 zero-dependency architecture. Build modular components following existing mcpkit ownership rules (transfer on success, clean up on error, ctx-routed allocations). Ensure 100% CTest pass rate and zero memory leaks under AddressSanitizer and UndefinedBehaviorSanitizer.

**Tech Stack:** C99/C23, CMake, GCC/Clang, POSIX sockets, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan).

---

## File Structure

### New Files
- `include/mcpkit/protocol/content.h`: Content annotations (`audience`, `priority`, `lastModified`) and annotated content item builders/parsers (SEP-1249).
- `src/protocol/content.c`: Implementation of content annotation builders, serializers, and parsers.
- `tests/unit/test_content_annotations.c`: Unit tests for content annotation construction and JSON serialization/deserialization.
- `include/mcpkit/protocol/uri_template.h`: RFC 6570 Level 1 and Level 2 URI template matching and parameter extraction engine.
- `src/protocol/uri_template.c`: Pure C99 URI template parser and variable extractor.
- `tests/unit/test_uri_template.c`: Unit tests for URI template matching, variable extraction, and expansion.
- `include/mcpkit/transport/sse.h`: Streamable HTTP Server-Sent Events (SSE) streaming frame parser and `Last-Event-ID` tracking.
- `src/transport/sse.c`: Streaming buffer parser for SSE `event:`, `data:`, `id:`, and `retry:` fields.
- `tests/unit/test_sse_parser.c`: Unit tests for SSE chunked framing, multiline data, and reconnection state.
- `include/mcpkit/core/auth.h`: RFC 7636 PKCE (`code_verifier`, `code_challenge` via SHA-256 + Base64URL) and OAuth 2.1 metadata discovery parser.
- `src/core/auth.c`: Implementation of PKCE generation and authorization server metadata parsing.
- `tests/unit/test_auth_pkce.c`: Unit tests for PKCE challenge computation against RFC 7636 test vectors and metadata JSON parsing.

### Modified Files
- `include/mcpkit/client/client.h`: Add `mcp_client_notify_roots_list_changed()` declaration.
- `src/client/client.c`: Implement `mcp_client_notify_roots_list_changed()`.
- `tests/unit/test_client.c`: Add test case for `notifications/roots/list_changed` emission.
- `CMakeLists.txt`: Register new source files in `MCPKIT_CORE_SOURCES`.
- `tests/CMakeLists.txt`: Register new test executables.
- `docs/module-reference.md`: Document new public APIs.
- `CHANGELOG.md`: Document new features.

---

## Task 1: Content Annotations & Audience Tagging (SEP-1249)

### Description
Implement Content Annotations on MCP content items (`TextContent`, `ImageContent`, `EmbeddedResource`) as defined in SEP-1249 and MCP 2026-07-28 specification:
- `audience`: array of strings (`"user"`, `"assistant"`).
- `priority`: float from `0.0` to `1.0` (context window trimming priority).
- `lastModified`: optional ISO-8601 string.
- Provide content item builder functions that produce standard JSON schema structures.

**Files:**
- Create: `include/mcpkit/protocol/content.h`
- Create: `src/protocol/content.c`
- Create: `tests/unit/test_content_annotations.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/protocol/content.h`**

Define `mcp_content_audience_t` flags, `mcp_content_annotations_t`, and builders:
```c
#ifndef MCPKIT_PROTOCOL_CONTENT_H
#define MCPKIT_PROTOCOL_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/value.h"

typedef enum mcp_content_audience {
    MCP_AUDIENCE_NONE      = 0,
    MCP_AUDIENCE_USER      = (1 << 0),
    MCP_AUDIENCE_ASSISTANT = (1 << 1),
    MCP_AUDIENCE_ALL       = (MCP_AUDIENCE_USER | MCP_AUDIENCE_ASSISTANT)
} mcp_content_audience_t;

typedef struct mcp_content_annotations {
    uint32_t audience;      /* Bitmask of mcp_content_audience_t */
    double priority;        /* 0.0 to 1.0; -1.0 if not specified */
    char last_modified[64]; /* ISO-8601 string, or empty */
} mcp_content_annotations_t;

mcp_json_value_t *mcp_content_text_new_annotated(mcp_context_t *ctx,
                                                 const char *text,
                                                 const mcp_content_annotations_t *annotations);

mcp_json_value_t *mcp_content_image_new_annotated(mcp_context_t *ctx,
                                                  const char *data_base64,
                                                  const char *mime_type,
                                                  const mcp_content_annotations_t *annotations);

mcp_json_value_t *mcp_content_resource_new_annotated(mcp_context_t *ctx,
                                                     const char *uri,
                                                     const char *mime_type,
                                                     const char *text_or_blob_base64,
                                                     bool is_binary,
                                                     const mcp_content_annotations_t *annotations);

mcp_status_t mcp_content_extract_annotations(mcp_context_t *ctx,
                                             const mcp_json_value_t *content_obj,
                                             mcp_content_annotations_t *out_annotations);

#endif
```

- [x] **Step 2: Write failing unit test in `tests/unit/test_content_annotations.c`**

Test:
1. Construct text content with `audience = MCP_AUDIENCE_ALL`, `priority = 0.85`, and `lastModified = "2026-09-24T00:00:00Z"`.
2. Verify JSON structure contains `{"type":"text","text":"...","annotations":{"audience":["user","assistant"],"priority":0.85,"lastModified":"2026-09-24T00:00:00Z"}}`.
3. Extract annotations back with `mcp_content_extract_annotations` and verify fields.
4. Test image content and embedded resource content with annotations.

- [x] **Step 3: Register in CMakeLists.txt and compile**

Add `src/protocol/content.c` to `MCPKIT_CORE_SOURCES` in `CMakeLists.txt` and `test_content_annotations` in `tests/CMakeLists.txt`.

- [x] **Step 4: Implement `src/protocol/content.c`**

Implement JSON construction and extraction with null checks and bounded string copies.

- [x] **Step 5: Run tests and verify PASS**

Run `ctest --test-dir build -R test_content_annotations --output-on-failure`.

- [x] **Step 6: Git commit**

```bash
git add include/mcpkit/protocol/content.h src/protocol/content.c tests/unit/test_content_annotations.c CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(protocol): implement Content Annotations and audience tagging (SEP-1249)"
```

---

## Task 2: Client Active Roots Changed Notification

### Description
Provide high-level client helper `mcp_client_notify_roots_list_changed(ctx, client)` that constructs and transmits `notifications/roots/list_changed` to the server when host workspace roots are added, removed, or renamed.

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `src/client/client.c`
- Modify: `tests/unit/test_client.c`

- [x] **Step 1: Declare in `include/mcpkit/client/client.h`**

```c
/**
 * @brief Sends notifications/roots/list_changed to notify the server that
 *        the client's workspace roots have changed.
 *
 * @param ctx Context; may be NULL.
 * @param client Target client; must not be NULL.
 * @return MCP_OK on success; transport or allocation status on error.
 */
mcp_status_t mcp_client_notify_roots_list_changed(mcp_context_t *ctx, mcp_client_t *client);
```

- [x] **Step 2: Implement in `src/client/client.c`**

Build notification `notifications/roots/list_changed` with empty or NULL params and transmit via `mcp_transport_send()`.

- [x] **Step 3: Add test case in `tests/unit/test_client.c`**

Verify `mcp_client_notify_roots_list_changed(ctx, c2)` sends JSON-RPC notification with method `"notifications/roots/list_changed"` and no `id` field.

- [x] **Step 4: Run tests and commit**

```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_client.c
git commit -m "feat(client): add mcp_client_notify_roots_list_changed helper API"
```

---

## Task 3: RFC 6570 URI Templates Variable Extraction Engine

### Description
Implement a zero-dependency RFC 6570 Level 1 (`{var}`) and Level 2 (`{+var}`) URI template matching and variable extraction engine for `resources/templates/list` integration.

**Files:**
- Create: `include/mcpkit/protocol/uri_template.h`
- Create: `src/protocol/uri_template.c`
- Create: `tests/unit/test_uri_template.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/protocol/uri_template.h`**

```c
#ifndef MCPKIT_PROTOCOL_URI_TEMPLATE_H
#define MCPKIT_PROTOCOL_URI_TEMPLATE_H

#include <stdbool.h>
#include <stddef.h>
#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/value.h"

/**
 * @brief Matches a concrete URI against an RFC 6570 template and extracts variables.
 *
 * Example:
 *   Template: "db://{table}/{id}"
 *   URI:      "db://users/42"
 *   Extracted params JSON object: {"table": "users", "id": "42"}
 *
 * @param ctx Context; may be NULL.
 * @param template_str Template string (e.g. "file:///{path}"); not owned.
 * @param uri Concrete URI string; not owned.
 * @param params_out On match, receives a caller-owned JSON object of key-value string pairs.
 *                   On mismatch, set to NULL and returns MCP_ERR_NOT_FOUND.
 * @return MCP_OK on match; MCP_ERR_NOT_FOUND on mismatch; MCP_ERR_INVALID_ARGUMENT on bad args.
 */
mcp_status_t mcp_uri_template_match(mcp_context_t *ctx,
                                    const char *template_str,
                                    const char *uri,
                                    mcp_json_value_t **params_out);

/**
 * @brief Expands an RFC 6570 template using a JSON object of parameters.
 *
 * @param ctx Context; may be NULL.
 * @param template_str Template string (e.g. "db://{table}/{id}").
 * @param params JSON object containing variable keys.
 * @param uri_out On success, receives caller-owned expanded URI string.
 * @return MCP_OK on success; error code on missing variables or allocation failure.
 */
mcp_status_t mcp_uri_template_expand(mcp_context_t *ctx,
                                     const char *template_str,
                                     const mcp_json_value_t *params,
                                     char **uri_out);

#endif
```

- [x] **Step 2: Write failing unit test in `tests/unit/test_uri_template.c`**

Test:
1. Exact match without variables: `"test://static/doc"` matches `"test://static/doc"`, returns empty JSON object `{}`.
2. Single variable Level 1: `"file:///{path}"` matches `"file:///var/log/syslog"`, extracts `{"path": "var/log/syslog"}`.
3. Multi-segment Level 1: `"db://{database}/{table}/{id}"` matches `"db://prod/users/123"`, extracts `{"database":"prod","table":"users","id":"123"}`.
4. Mismatch detection: `"db://{table}/{id}"` against `"http://users/123"` returns `MCP_ERR_NOT_FOUND`.
5. Template expansion: expanding `"db://{table}/{id}"` with `{"table":"items","id":"99"}` produces `"db://items/99"`.

- [x] **Step 3: Implement `src/protocol/uri_template.c`**

Implement recursive token matching parsing literal prefixes, `{var}` expressions, extracting segment substrings, and building JSON key-value objects.

- [x] **Step 4: Run tests and verify PASS**

Run `ctest --test-dir build -R test_uri_template --output-on-failure`.

- [x] **Step 5: Git commit**

```bash
git add include/mcpkit/protocol/uri_template.h src/protocol/uri_template.c tests/unit/test_uri_template.c CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(protocol): implement RFC 6570 URI template matching and variable extraction"
```

---

## Task 4: Streamable HTTP SSE Streaming Frame Parser & Reconnection State

### Description
Implement a streaming Server-Sent Events (SSE) frame parser capable of processing incremental I/O chunks on client connections, extracting event types, multiline data payloads, and tracking `Last-Event-ID` for resilient reconnection.

**Files:**
- Create: `include/mcpkit/transport/sse.h`
- Create: `src/transport/sse.c`
- Create: `tests/unit/test_sse_parser.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/transport/sse.h`**

```c
#ifndef MCPKIT_TRANSPORT_SSE_H
#define MCPKIT_TRANSPORT_SSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"

typedef struct mcp_sse_event {
    char *event_type;  /* e.g. "message" or custom; caller frees or parser owns */
    char *data;        /* Concat multiline data payload; caller frees or parser owns */
    char *id;          /* Event ID string; caller frees or parser owns */
    uint64_t retry_ms; /* Reconnect delay; 0 if omitted */
} mcp_sse_event_t;

typedef struct mcp_sse_parser mcp_sse_parser_t;

mcp_sse_parser_t *mcp_sse_parser_create(mcp_context_t *ctx);
void mcp_sse_parser_destroy(mcp_context_t *ctx, mcp_sse_parser_t *parser);

/**
 * @brief Feeds chunk of data into SSE parser.
 *
 * Whenever a full event is completed (double newline), event_ready returns true
 * and out_event receives caller-owned event strings.
 */
mcp_status_t mcp_sse_parser_feed(mcp_context_t *ctx,
                                 mcp_sse_parser_t *parser,
                                 const char *chunk,
                                 size_t len,
                                 mcp_sse_event_t *out_event,
                                 bool *event_ready);

void mcp_sse_event_cleanup(mcp_context_t *ctx, mcp_sse_event_t *ev);

const char *mcp_sse_parser_last_event_id(const mcp_sse_parser_t *parser);

#endif
```

- [x] **Step 2: Write failing unit test in `tests/unit/test_sse_parser.c`**

Test:
1. Feeding single complete SSE frame: `"event: message\ndata: {\"jsonrpc\":\"2.0\"}\nid: evt-101\n\n"`.
2. Verify `event_ready == true`, `data` contains `{"jsonrpc":"2.0"}`, `id` is `"evt-101"`, `last_event_id` is `"evt-101"`.
3. Feeding fragmented chunk across multiple `mcp_sse_parser_feed` calls:
   Chunk 1: `"event: msg\nda"` -> `event_ready == false`.
   Chunk 2: `"ta: line1\ndata: line2\n\n"` -> `event_ready == true`, `data == "line1\nline2"`.
4. Comment line (`:ping`) ignored properly.

- [x] **Step 3: Implement `src/transport/sse.c`**

Stateful line-based parser with carry buffer handling `\r\n` and `\n` line endings, stripping leading space after colon, concatenating multiple `data:` lines with `\n`.

- [x] **Step 4: Run tests and verify PASS**

Run `ctest --test-dir build -R test_sse_parser --output-on-failure`.

- [x] **Step 5: Git commit**

```bash
git add include/mcpkit/transport/sse.h src/transport/sse.c tests/unit/test_sse_parser.c CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(transport): implement streaming Server-Sent Events (SSE) frame parser"
```

---

## Task 5: RFC 7636 PKCE & OAuth 2.1 Metadata Client Utilities

### Description
Implement pure C99 PKCE code challenge and verifier generation (RFC 7636) and OAuth 2.1 Authorization Server Metadata parser (RFC 8414) for secure remote MCP server connections.

**Files:**
- Create: `include/mcpkit/core/auth.h`
- Create: `src/core/auth.c`
- Create: `tests/unit/test_auth_pkce.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/core/auth.h`**

```c
#ifndef MCPKIT_CORE_AUTH_H
#define MCPKIT_CORE_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"

typedef struct mcp_oauth_metadata {
    char issuer[256];
    char authorization_endpoint[256];
    char token_endpoint[256];
    char registration_endpoint[256];
    bool supports_pkce_s256;
} mcp_oauth_metadata_t;

/**
 * @brief Generates an RFC 7636 PKCE code_verifier and S256 code_challenge.
 *
 * Verifier: 43 to 128 characters of unreserved characters.
 * Challenge: BASE64URL-ENCODE(SHA256(verifier)) without padding.
 *
 * @param ctx Context; may be NULL.
 * @param verifier_buf Output buffer (minimum 128 bytes).
 * @param verifier_buf_size Size of verifier_buf.
 * @param challenge_buf Output buffer (minimum 64 bytes).
 * @param challenge_buf_size Size of challenge_buf.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_pkce_generate(mcp_context_t *ctx,
                               char *verifier_buf,
                               size_t verifier_buf_size,
                               char *challenge_buf,
                               size_t challenge_buf_size);

/**
 * @brief Computes S256 code_challenge for a given code_verifier.
 */
mcp_status_t mcp_pkce_compute_challenge(mcp_context_t *ctx,
                                        const char *verifier,
                                        char *challenge_out,
                                        size_t challenge_out_size);

/**
 * @brief Parses OAuth 2.0 / 2.1 Authorization Server Metadata JSON string (RFC 8414).
 */
mcp_status_t mcp_oauth_metadata_parse(mcp_context_t *ctx,
                                      const char *json_str,
                                      mcp_oauth_metadata_t *meta_out);

#endif
```

- [x] **Step 2: Write failing unit test in `tests/unit/test_auth_pkce.c`**

Test:
1. RFC 7636 Appendix B test vector:
   `verifier = "dBjftJeZ4CVP-m5GQoBX_gjlCmGhQ54FuQg-9FFqK-q"`
   `expected_challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"`
   Verify `mcp_pkce_compute_challenge` yields exact match.
2. `mcp_pkce_generate` generates valid unreserved verifier (length >= 43) and valid 43-char S256 challenge.
3. Metadata parser correctly extracts `issuer`, `authorization_endpoint`, `token_endpoint`, and detects `"S256"` in `code_challenge_methods_supported`.

- [x] **Step 3: Implement `src/core/auth.c`**

Implement PKCE with SHA-256 and URL-safe Base64 without padding. Implement metadata parsing with `mcp_json_parse` and string extractors.

- [x] **Step 4: Run tests and verify PASS**

Run `ctest --test-dir build -R test_auth_pkce --output-on-failure`.

- [x] **Step 5: Git commit**

```bash
git add include/mcpkit/core/auth.h src/core/auth.c tests/unit/test_auth_pkce.c CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(auth): implement RFC 7636 PKCE S256 challenge generation and OAuth 2.1 metadata parsing"
```

---

## Task 6: Documentation, Module Reference & Triple-Gate Verification

### Description
Document all new modules in `docs/module-reference.md` and `CHANGELOG.md`. Run full test suite under both standard build and ASan + UBSan gate checks to ensure zero leaks and 100% test pass rate.

**Files:**
- Modify: `docs/module-reference.md`
- Modify: `CHANGELOG.md`

- [x] **Step 1: Update documentation and CHANGELOG**
  - Document `mcpkit/protocol/content.h`, `mcpkit/protocol/uri_template.h`, `mcpkit/transport/sse.h`, `mcpkit/core/auth.h` in `docs/module-reference.md`.
  - Add entries under `[Unreleased]` in `CHANGELOG.md`.

- [x] **Step 2: Run full test suite on standard build**

Run `ctest --test-dir build -j1 --output-on-failure`. Expected: 100% PASS (56 / 56 tests).

- [x] **Step 3: Run full test suite with ASan + UBSan**

Run `cmake -B build-asan -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON && cmake --build build-asan && LD_PRELOAD=/usr/lib/libasan.so ctest --test-dir build-asan -j1 --output-on-failure`.
Expected: 100% PASS with 0 leaks, 0 warnings. Clean up `build-asan`.

- [x] **Step 4: Git commit**

```bash
git add docs/module-reference.md CHANGELOG.md
git commit -m "docs(changelog): document Content Annotations, URI Templates, SSE Parser, and PKCE Auth"
```
