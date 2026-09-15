# Phase 5 Client Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `mcp_client_t`: initialize handshake, request/response with id matching, tools/resources/prompts convenience calls, over any `mcp_transport_t`.

**Architecture:** Thin synchronous caller around message builders + transport send/recv. Client owns id counter + negotiated version string; transport and ctx are borrowed (caller owns, cf. server pattern). Result values returned to caller are clones owned by the caller.

**Tech Stack:** C23, existing message/transport/initialize APIs, tmpfile-pump e2e test, bash coproc example check.

---

## Chunk 1: Header contracts (full text — implement exactly)

### `include/mcpkit/client/client.h` (new)

```c
#ifndef MCPKIT_CLIENT_CLIENT_H
#define MCPKIT_CLIENT_CLIENT_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

typedef struct mcp_client mcp_client_t;

mcp_client_t *mcp_client_create(mcp_context_t *ctx, mcp_transport_t *transport);
void mcp_client_destroy(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_connect(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_disconnect(mcp_context_t *ctx, mcp_client_t *client);
// Full handshake: sends initialize, validates response id, stores negotiated
// version, sends notifications/initialized. server_info_out (optional) receives
// a cloned serverInfo value owned by the caller, or NULL when absent.
mcp_status_t mcp_client_initialize(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *client_name, const char *client_version,
                                   mcp_json_value_t **server_info_out);
// Negotiated protocol version after initialize, or NULL before. Borrowed.
const char *mcp_client_protocol_version(mcp_context_t *ctx, const mcp_client_t *client);
// Generic call: params ownership transfers to the request on success (container
// owns on OK). result_out (optional) receives a cloned result owned by caller.
// Error responses map to mcp_status_t via mcp_rpc_code_to_status.
mcp_status_t mcp_client_request(mcp_context_t *ctx, mcp_client_t *client,
                                const char *method, mcp_json_value_t *params,
                                mcp_json_value_t **result_out);
mcp_status_t mcp_client_ping(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_list_tools(mcp_context_t *ctx, mcp_client_t *client,
                                   mcp_json_value_t **tools_out);
mcp_status_t mcp_client_call_tool(mcp_context_t *ctx, mcp_client_t *client,
                                  const char *name, mcp_json_value_t *args,
                                  mcp_json_value_t **result_out);
mcp_status_t mcp_client_read_resource(mcp_context_t *ctx, mcp_client_t *client,
                                      const char *uri, mcp_json_value_t **result_out);
mcp_status_t mcp_client_get_prompt(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *name, mcp_json_value_t *args,
                                   mcp_json_value_t **result_out);

#endif
```

**Ownership:** `mcp_client_create` → caller owns client; transport/ctx borrowed (destroy never touches them). `*_out` values are clones (see `mcp_json_clone`) owned by caller, freed with `mcp_json_free(ctx, ...)`. On any error return, `*out` is set NULL and params are destroyed (no leak on failure path — client takes params even on failure, unlike dispatcher builders; document and test).

**ID matching:** request ids are numbers from a client-owned counter starting at 1. A response whose numeric id differs → `MCP_ERR_PROTOCOL`. Non-response message → `MCP_ERR_PROTOCOL`. Transport/parse failures propagate.

### Files

- Create: `include/mcpkit/client/client.h`, `src/client/client.c`,
  `src/client/internals.h` (private struct + alloc helper decl),
  `tests/unit/test_client.c` (fake + replay tests),
  `tests/unit/test_client_e2e.c` (tmpfile pump vs real server),
  `examples/client/main.c`, `examples/CMakeLists.txt` (modify: add client)
- Modify: `include/mcpkit/mcpkit.h` (+2 lines), `CMakeLists.txt` (+1 line),
  `tests/CMakeLists.txt` (+2 tests), `CHANGELOG.md` (+1 line)

---

## Chunk 2: Tasks

### Task 1: Client core (create/destroy/connect + generic request + fake test)

- [ ] **Step 1: Write the failing test** `tests/unit/test_client.c`: local fake
  transport backend (scripted recv lines array + sent-line capture; reuse the
  record/replay shape from `tests/unit/test_transport.c` — read it first).
  Cases: create(NULL-transport)→NULL; connect calls start_op once; request
  sends one line and parses a scripted `{"jsonrpc":"2.0","id":1,"result":{...}}`
  into an equal clone; id-mismatch response → `MCP_ERR_PROTOCOL`;
  `mcp_client_request` with NULL client/method → `MCP_ERR_INVALID_ARGUMENT`.
- [ ] **Step 2: Register + configure to see it fail**

  Run: `cmake -S . -B build && cmake --build build 2>&1 | tail -3`
  Expected: FAIL (no `src/client/client.c`)
- [ ] **Step 3: Write headers + impl.** `client.h` (exact text above),
  `src/client/internals.h` (struct: `mcp_context_t *ctx; mcp_transport_t *t;
  double next_id; char *version;` + `srv`-style alloc helpers are in server
  internals — write local `static` helpers in client.c instead, no shared header
  needed beyond the struct). `src/client/client.c`: create (NULL transport →
  NULL; alloc via `mcp_context_allocator(ctx)` with NULL-ctx
  `mcp_default_allocator()` fallback — copy the 3-line pattern from
  `src/protocol/message.c`), destroy (frees version + struct, never transport),
  connect/disconnect (transport_start/stop passthrough with NULL guards),
  static `roundtrip(method, params, &resp_msg)`: build request with
  `mcp_request_new_number_id`, serialize, send, recv line, parse (NULL parse →
  destroy request, return `MCP_ERR_PROTOCOL`), kind must be RESPONSE else
  `MCP_ERR_PROTOCOL`, numeric id must equal request id else `MCP_ERR_PROTOCOL`.
  `mcp_client_request`: NULL guards → INVALID_ARGUMENT; roundtrip; error
  response → `mcp_rpc_code_to_status(code)`; ok → clone result via
  `mcp_json_clone` (NOMEM → NOMEM); params destroyed on ALL paths after the
  guards (document deviation from dispatcher convention in a 1-line comment).
- [ ] **Step 4: Wire CMake/mcpkit.h/tests + run**

  Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build`
  Expected: 20/20 (19 old + test_client)
- [ ] **Step 5: Commit**

```bash
git add include/mcpkit/client src/client tests/unit/test_client.c tests/CMakeLists.txt CMakeLists.txt include/mcpkit/mcpkit.h
git commit -m "feat(client): add synchronous request client with id matching"
```

### Task 2: Initialize + convenience calls (replay test)

- [ ] **Step 1: Extend `tests/unit/test_client.c`** with a replay fake serving:
  initialize result `{"protocolVersion":"2025-06-18","serverInfo":{"name":"s","version":"1"},"capabilities":{}}`,
  then empty notifications/initialized send (assert second sent line has no id
  and method `notifications/initialized`), then a tools/call error response
  `{"jsonrpc":"2.0","id":3,"error":{"code":-32602,"message":"Unknown tool"}}`.
  Assert: `mcp_client_initialize` → OK, `mcp_client_protocol_version` ==
  "2025-06-18", serverInfo name == "s"; `mcp_client_call_tool("nope", NULL)`
  → `MCP_ERR_INVALID_ARGUMENT`... (no — unknown tool is a server-side -32602
  → maps to `MCP_ERR_INVALID_ARGUMENT`; assert that). `mcp_client_ping` against
  `{}` result → OK. Error response with no message field → still maps by code.
- [ ] **Step 2: Run to see it fail** (missing symbols). Expected: link FAIL.
- [ ] **Step 3: Implement** in `src/client/client.c`: initialize (params via
  `mcp_initialize_params_new_v(ctx, MCP_PROTOCOL_VERSION_LATEST, name, version)`;
  NULL name → INVALID_ARGUMENT; roundtrip id 1; error → mapped status; result
  must be object with string protocolVersion else PROTOCOL; store strdup of it;
  server_info_out → clone of `serverInfo` member or NULL; send initialized
  notification via `mcp_initialized_notification_new` + send (no recv); send
  failure → PROTOCOL? No — return the send status directly). ping (params NULL,
  ignore result). list_tools (`tools/list`, params NULL, tools_out = clone of
  `tools` member array or full result? Return full result clone — simpler, test
  asserts member). call_tool (build `{name, arguments?}` object; args NULL →
  omit member; ownership: args transfers into params object). read_resource
  (`{uri}`), get_prompt (`{name, arguments?}`).
- [ ] **Step 4: Run.** Expected: 20/20 green.
- [ ] **Step 5: Commit** `feat(client): add initialize handshake and tool/resource/prompt calls`

### Task 3: End-to-end pump test vs real server (19→21)

- [ ] **Step 1: Write `tests/unit/test_client_e2e.c`**: tmpfile pair
  (c2s, s2c) like `test_stdio_loopback.c` — read it first. Real server with echo
  tool + static resource (copy the 30-line setup from the loopback test).
  `server_step()`: read one line from c2s (fgets manual), parse, if notification
  → notify, if request → dispatch + serialize into s2c. Client transport: custom
  ops where send appends line to c2s and recv reads one line from s2c (position
  with fseek/ftell pairs — keep a write-end offset and read-end offset, or
  simpler: fflush + rewind before each read and track consumed offset; the
  loopback test already solves tmpfile juggling — mirror its approach). Flow:
  initialize → version "2025-06-18"; notify initialized via direct
  `mcp_client_request`? No — initialize() already sends it; pump two lines
  (request + notification). tools/list → 1 tool; tools/call echo "hi" → text
  "hi"; unknown tool → INVALID_ARGUMENT; ping → OK.
- [ ] **Step 2: Register + run, expect FAIL** (no file yet — configure fails at
  add_executable? No: add test first, build fails). Then implement.
- [ ] **Step 3: Implement test + pump helper.** Keep under ~150 lines.
- [ ] **Step 4: Run.** Expected: 21/21.
- [ ] **Step 5: Commit** `test(client): prove client-server stdio flow over tmpfile pair`

### Task 4: examples/client + live coproc check

- [ ] **Step 1: Write `examples/client/main.c`**: usage
  `client <list|call|ping> [name] [text]`; stdio transport on stdin/stdout;
  connect → initialize("example-client","0.1.0") → run command → print result
  JSON via `mcp_json_serialize` → disconnect/destroy. Error paths print
  `mcp_status_string` to stderr, rc 1.
- [ ] **Step 2: Wire `examples/CMakeLists.txt`, build with
  `-DMCPKIT_BUILD_EXAMPLES=ON`, run live**: `coproc SRV { ./build/examples/stdio-server; };
  ./build/examples/client list <&${SRV[0]} >&${SRV[1]};` etc. for list, call,
  ping. Expected: valid JSON results, rc 0.
- [ ] **Step 3: Commit** `feat(examples): add stdio client example`

### Task 5: CHANGELOG + triple gates + merge (standing instruction: auto-merge, continue to P6)

- [ ] CHANGELOG: `- Phase 5: sync client (initialize, tools/resources/prompts) over any transport.`
- [ ] Triple gates from clean configures: gcc 21/21, clang 21/21,
  ASan+UBSan 21/21 (manual flags, verify libasan via `ldd`).
- [ ] `rm -rf build*`, merge to master, post-merge ctest 21/21, delete branch.
