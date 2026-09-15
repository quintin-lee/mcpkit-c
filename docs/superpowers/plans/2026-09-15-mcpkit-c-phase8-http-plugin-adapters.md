# Phase 8 HTTP+Plugin+Adapters Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add buffer-level HTTP/1.1 framing + Streamable HTTP serve loop (SSE compat only) + static plugin registry + adapters stub, all behind `MCPKIT_BUILD_HTTP` (except the zero-dep plugin registry which is always built).

**Architecture:** HTTP framing is pure buffer in/buffer out (no sockets in core, preserving zero-dep); the serve loop drives dispatcher through byte-oriented read/write callbacks because HTTP request/response framing does not fit the line-oriented `mcp_transport_t` ops. Plugin registry is process-global name→pointer with borrowed names (precedent: `mcp_json_builtin_backend()` takes no ctx). Session map lives in the serve loop, keyed by `Mcp-Session-Id`.

**Tech Stack:** C23, CMake + CTest, assert-style unit tests, gcc/clang/ASan+UBSan gates, POSIX sockets only in the example.

---

## Chunk 1: Header contracts (full text — write verbatim)

### `include/mcpkit/plugin/plugin.h` (ALWAYS built)

```c
#ifndef MCPKIT_PLUGIN_PLUGIN_H
#define MCPKIT_PLUGIN_PLUGIN_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef enum {
    MCP_PLUGIN_JSON_BACKEND,
    MCP_PLUGIN_TRANSPORT,
    MCP_PLUGIN_ADAPTER,
} mcp_plugin_kind_t;

#define MCP_PLUGIN_MAX_ENTRIES 32

mcp_status_t mcp_plugin_register(mcp_plugin_kind_t kind, const char *name, const void *ptr);
mcp_status_t mcp_plugin_unregister(mcp_plugin_kind_t kind, const char *name);
const void *mcp_plugin_find(mcp_plugin_kind_t kind, const char *name);
size_t mcp_plugin_count(mcp_plugin_kind_t kind);

#endif
```

Contract: `name` is BORROWED (must outlive registration; static literals in practice) — registry never copies, never frees, never owns `ptr`. NULL name/ptr → `MCP_ERR_INVALID_ARGUMENT`; dup (kind,name) → `MCP_ERR_ALREADY_EXISTS`; unregister missing → `MCP_ERR_NOT_FOUND`; find missing → NULL. No ctx param (precedent: `mcp_json_builtin_backend()`). No NOMEM (fixed table, no alloc) — full table overwrites nothing, returns `MCP_ERR_NOMEM`.

### `include/mcpkit/transport/http.h` (gated behind MCPKIT_BUILD_HTTP)

```c
#ifndef MCPKIT_TRANSPORT_HTTP_H
#define MCPKIT_TRANSPORT_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_http_request mcp_http_request_t;
typedef struct mcp_http_response mcp_http_response_t;

typedef enum {
    MCP_HTTP_GET,
    MCP_HTTP_POST,
    MCP_HTTP_DELETE,
    MCP_HTTP_UNKNOWN,
} mcp_http_method_t;

#define MCP_HTTP_MAX_HEADERS 64
#define MCP_HTTP_MAX_LINE 8192
#define MCP_HTTP_MAX_BODY (4u << 20)

mcp_http_request_t *mcp_http_parse_request(mcp_context_t *ctx, const char *data, size_t len);
void mcp_http_request_destroy(mcp_context_t *ctx, mcp_http_request_t *req);
mcp_http_method_t mcp_http_request_method(mcp_context_t *ctx, const mcp_http_request_t *req);
const char *mcp_http_request_target(mcp_context_t *ctx, const mcp_http_request_t *req);
const char *mcp_http_header(mcp_context_t *ctx, const mcp_http_request_t *req, const char *name);
const char *mcp_http_request_body(mcp_context_t *ctx, const mcp_http_request_t *req, size_t *len_out);

mcp_http_response_t *mcp_http_response_new(mcp_context_t *ctx, int status, const char *reason);
mcp_status_t mcp_http_response_set_header(mcp_context_t *ctx, mcp_http_response_t *resp,
                                          const char *name, const char *value);
mcp_status_t mcp_http_response_set_body(mcp_context_t *ctx, mcp_http_response_t *resp,
                                        const char *body, size_t len);
const char *mcp_http_response_serialize(mcp_context_t *ctx, const mcp_http_response_t *resp);
void mcp_http_response_destroy(mcp_context_t *ctx, mcp_http_response_t *resp);
```

Contract: all fns take ctx first, NULL ctx allowed (default-allocator fallback, precedent: transport.c `alloc_of`). Parse failures (malformed line, >64 headers, line >8KB, body >4MB, Content-Length mismatch) → NULL. Header names case-insensitive on lookup. Headers COPIED via ctx allocator; body BORROWED from input buffer (valid while input alive — document). `mcp_http_header` returns NULL when missing. Response serialize emits `HTTP/1.1 <status> <reason>\r\nName: Value\r\n...Content-Length: N\r\n\r\n<body>`; body may be NULL (no body, still emits Content-Length: 0). Serialize returns a BORROWED pointer valid until `mcp_http_response_destroy` — no free call needed.

### `include/mcpkit/transport/streamable_http.h` (gated)

```c
#ifndef MCPKIT_TRANSPORT_STREAMABLE_HTTP_H
#define MCPKIT_TRANSPORT_STREAMABLE_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;

typedef struct mcp_http_io mcp_http_io_t;
struct mcp_http_io {
    void *user;
    mcp_status_t (*read)(mcp_context_t *ctx, void *user, char *buf, size_t cap, size_t *n_out);
    mcp_status_t (*write)(mcp_context_t *ctx, void *user, const char *data, size_t len);
};

#define MCP_HTTP_MAX_SESSIONS 16

mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_http_io_t *io);
char *mcp_sse_wrap(mcp_context_t *ctx, const char *json_text);
```

Contract: does NOT own server/io. Loop: read request bytes (accumulate until `\r\n\r\n` + Content-Length body; read returning NOMEM/EOF-mid-request aborts loop with that status; clean EOF with zero bytes → MCP_OK break). Routing: POST → JSON-RPC dispatch (session from `Mcp-Session-Id` header; missing header + `initialize` method → create session, id `sess-<N>`, reply carries `Mcp-Session-Id`; missing header + other method → 400; unknown id → 404; DELETE with id → destroy session → 200, missing/unknown → 404; GET with `Accept: text/event-stream` → 200 SSE response with `data: {"ping":"ok"}\n\n` compat body; GET otherwise → 405. Dispatch errors map: notify → 202 with empty body; response → 200 JSON. Session ids generated as `sess-<counter>` via snprintf into ctx-allocated copy. `mcp_sse_wrap` returns `data: <json>\n\n` (NULL on bad args/NOMEM).

---

## Chunk 2: Tasks

### Task 0: Plan commit

- [ ] Step 1: Write this file (done)
- [ ] Step 2: Commit

```bash
git add docs/superpowers/plans/2026-09-15-mcpkit-c-phase8-http-plugin-adapters.md
git commit -m "docs: add phase 8 http-plugin-adapters detailed plan"
```

### T1: Plugin static registry

**Files:**
- Create: `include/mcpkit/plugin/plugin.h` (verbatim Chunk 1), `src/plugin/plugin.c`, `tests/unit/test_plugin.c`
- Modify: `include/mcpkit/mcpkit.h` (+1 include), `tests/CMakeLists.txt` (+3 lines, UNCONDITIONAL)

- [ ] Step 1: Write the failing test `tests/unit/test_plugin.c`:

```c
#include <assert.h>
#include <string.h>

#include "mcpkit/plugin/plugin.h"

static int dummy_a;
static int dummy_b;

int main(void) {
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "a", &dummy_a) == MCP_OK);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "a", &dummy_a)
           == MCP_ERR_ALREADY_EXISTS);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, NULL, &dummy_a)
           == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "b", NULL)
           == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "a") == &dummy_a);
    assert(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "missing") == NULL);
    assert(mcp_plugin_find(MCP_PLUGIN_TRANSPORT, "a") == NULL);
    assert(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 1);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "b", &dummy_b) == MCP_OK);
    assert(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 2);
    assert(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "a") == MCP_OK);
    assert(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "a") == MCP_ERR_NOT_FOUND);
    assert(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "a") == NULL);
    assert(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "b") == MCP_OK);
    assert(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 0);
    return 0;
}
```

- [ ] Step 2: Register in `tests/CMakeLists.txt`, configure — Expected: FAIL (no `src/plugin/plugin.c` in build; link error)
- [ ] Step 3: Write `src/plugin/plugin.c`: static `entries[MCP_PLUGIN_MAX_ENTRIES]` of {kind, name, ptr, used}; linear scan; no alloc, no ctx. Add `src/plugin/plugin.c` to root `CMakeLists.txt` UNCONDITIONALLY + `#include "mcpkit/plugin/plugin.h"` to `mcpkit.h`
- [ ] Step 4: Rebuild, `ctest` — Expected: 28/28 (27 + test_plugin)
- [ ] Step 5: Commit `feat(plugin): add static name-pointer registry`

### T2: HTTP/1.1 framing

**Files:**
- Create: `include/mcpkit/transport/http.h` (verbatim), `src/transport/http.c`, `tests/unit/test_http.c`
- Modify: root `CMakeLists.txt` (`if(MCPKIT_BUILD_HTTP) target_sources(... src/transport/http.c) endif()`; T3 appends `src/transport/streamable_http.c`), `mcpkit.h` (+http.h include now, +streamable include in T3), `tests/CMakeLists.txt` (`if(MCPKIT_BUILD_HTTP)` block for test_http)

- [ ] Step 1: Write `tests/unit/test_http.c`:

```c
#include <assert.h>
#include <string.h>

#include "mcpkit/transport/http.h"

int main(void) {
    /* minimal POST */
    const char *raw =
        "POST /mcp HTTP/1.1\r\nContent-Length: 2\r\nContent-Type: application/json\r\n\r\n{}";
    mcp_http_request_t *req = mcp_http_parse_request(NULL, raw, strlen(raw));
    assert(req != NULL);
    assert(mcp_http_request_method(NULL, req) == MCP_HTTP_POST);
    assert(strcmp(mcp_http_request_target(NULL, req), "/mcp") == 0);
    assert(strcmp(mcp_http_header(NULL, req, "content-type"), "application/json") == 0);
    size_t blen = 0;
    assert(strncmp(mcp_http_request_body(NULL, req, &blen), "{}", blen) == 0 && blen == 2);
    mcp_http_request_destroy(NULL, req);
    /* malformed + truncated */
    assert(mcp_http_parse_request(NULL, "GARBAGE", 7) == NULL);
    assert(mcp_http_parse_request(NULL, "POST /mcp HTTP/1.1\r\n", 19) == NULL);
    assert(mcp_http_parse_request(NULL, NULL, 0) == NULL);
    /* Content-Length mismatch */
    assert(mcp_http_parse_request(NULL,
        "POST /mcp HTTP/1.1\r\nContent-Length: 10\r\n\r\n{}", 44) == NULL);
    /* response round shape */
    mcp_http_response_t *r = mcp_http_response_new(NULL, 200, "OK");
    assert(r != NULL);
    assert(mcp_http_response_set_header(NULL, r, "Content-Type", "application/json") == MCP_OK);
    assert(mcp_http_response_set_body(NULL, r, "{}", 2) == MCP_OK);
    const char *s = mcp_http_response_serialize(NULL, r);
    assert(s != NULL && strstr(s, "HTTP/1.1 200 OK\r\n") == s);
    assert(strstr(s, "Content-Length: 2\r\n\r\n{}") != NULL);
    /* s is BORROWED (valid until destroy) — no free */
    mcp_http_response_destroy(NULL, r);
    return 0;
}
```

NOTE (resolved): `mcp_http_response_serialize` returns BORROWED pointer valid until destroy — no free call; impl keeps the serialized bytes owned inside the response struct.

- [ ] Step 2: Configure with `-DMCPKIT_BUILD_HTTP=ON`, build — Expected: FAIL (test_http registered, no impl)
- [ ] Step 3: Write `src/transport/http.c`: request-line manual token scan (no sscanf), header split at first `:`, trim OWS, case-insensitive lookup via tolower loop, Content-Length strict strtoul, body bounds check, MAX_BODY → NULL. Alloc pattern: copy `alloc_of` NULL-ctx fallback from transport.c. Response: fixed 16-slot header array, serialize into grown buffer.
- [ ] Step 4: `ctest` default → 28/28; `ctest` with HTTP=ON → 29/29
- [ ] Step 5: Commit `feat(transport): add HTTP/1.1 buffer-level framing`

### T3: SSE-compat + serve loop with session map

**Files:**
- Create: `include/mcpkit/transport/streamable_http.h` (verbatim), `src/transport/streamable_http.c` (CMake already wired in T2), `tests/unit/test_http_serve.c`
- Modify: `mcpkit.h` (+streamable include + CMake appends `src/transport/streamable_http.c` in the HTTP block), `tests/CMakeLists.txt` (test_http_serve in HTTP block)

- [ ] Step 1: Write `tests/unit/test_http_serve.c` with in-memory IO: scripted request buffers, capturing writer. Cases: (a) POST initialize without session → 200 + `Mcp-Session-Id: sess-1` + body contains `2025-06-18`; (b) POST notifications/initialized with id → 202; (c) POST tools/call with id → 200 echo; (d) POST tools/call without id → 400; (e) unknown id → 404; (f) GET with Accept: text/event-stream → 200 + `text/event-stream` + `data: ` prefix; (g) GET without → 405; (h) DELETE id → 200, then reuse → 404; (i) `mcp_sse_wrap(NULL, "{\"a\":1}")` equals `"data: {\"a\":1}\n\n"`, NULL args → NULL. Server fixture: echo tool (copy shape from test_stdio_loopback / dispatcher tests — read that file first). IO read feeds one request per call then clean EOF → serve returns MCP_OK.
- [ ] Step 2: Build HTTP=ON — Expected: FAIL (undeclared serve/sse)
- [ ] Step 3: Write `src/transport/streamable_http.c`: accumulate reader (grow to headers end + body; EOF with 0 bytes → OK break; EOF mid-request → MCP_ERR_IO abort); route per Chunk 1; dispatch via `mcp_server_dispatch`/`mcp_server_notify` (server.h signatures confirmed); session table fixed 16, full → 500? use 503? No — 500 with `{"full"}`? Keep 500 + JSON error body via message error path? Simplest: 500 status, empty body. Session id `sess-<n>` counter.
- [ ] Step 4: `ctest` HTTP=ON → 30/30; default → 28/28
- [ ] Step 5: Commit `feat(transport): add Streamable HTTP serve loop with session map`

### T4: http-server example + adapters stub

**Files:**
- Create: `examples/http-server/main.c`, `examples/http-server/CMakeLists.txt`? (follow existing: single files wired in examples/CMakeLists.txt — check how stdio-server is wired; likely direct add_executable), `adapters/README.md`
- Modify: `examples/CMakeLists.txt` (`if(MCPKIT_BUILD_HTTP) add_executable(http-server ...) endif()`)

- [ ] Step 1: Write `adapters/README.md`: seam table (yyjson→json backend slot, libuv→executor/timer, curl→client IO, OpenSSL→TLS termination in example, SQLite→resource persistence), each registered via `mcp_plugin_register` with kind; explicit NON-GOALS (no dynamic .so until ABI stable — master plan P8).
- [ ] Step 2: Write `examples/http-server/main.c`: POSIX TCP listen 127.0.0.1:8765 (`_POSIX_C_SOURCE 200809L`, socket/recv/send), per-connection: read until full request, run one-shot serve over the bytes (reuse framing+dispatch via a memory-IO adapter calling the same internal route fn — if route fn is static in streamable_http.c, expose a one-shot `mcp_http_handle(ctx, server, sessions, req_bytes, ...)`? YAGNI check: simpler — example owns its own session table? NO, duplication. Expose minimal one-shot helper in streamable_http.h NOW (add to header in T4, document as example-facing): `mcp_http_handle_one(ctx, server, sessions_state?, ...)`. Cleaner: make serve-loop's session table a public opaque `mcp_http_sessions_t` with create/destroy, and both serve + example use it. DECISION (lock in T4 step 1 after reading T3 code): refactor session table to `mcp_http_sessions_{create,destroy}` + `mcp_http_handle(ctx, server, sess, raw, raw_len, out_status, out_bytes...)`. Keep refactor tiny.
- [ ] Step 3: Build HTTP=ON, run server, verify with raw socket client: `python3 -c` POST initialize → expect `Mcp-Session-Id` + 2025-06-18; POST tools/call → echo; GET SSE → event-stream. Expected: all 200/202, RC=0
- [ ] Step 4: Commit `feat(examples): add POSIX http-server + adapters stub`

### T5: CHANGELOG + gates + cleanup + merge

- [ ] Step 1: CHANGELOG Phase 8 entry
- [ ] Step 2: Triple gates default (28/28 gcc, clang, ASan+UBSan manual flags — check whether MCPKIT_ENABLE_ASAN is wired; Phase 6 lesson: it was NOT wired, use manual `-fsanitize=address,undefined` + ldd verify) + one HTTP=ON configure build+test (30/30)
- [ ] Step 3: `rm -rf build*`, `git status` clean, commit `chore: log phase 8 http-plugin-adapters`, merge to master per standing instruction, post-merge default 28/28 + HTTP=ON 30/30, delete branch
