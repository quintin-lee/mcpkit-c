# Phase 7 MCP Apps Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the MCP Apps extension layer (`ui://` resources, `_meta.ui` attach, structured CSP, tool visibility, permission gating, lifecycle callbacks) on top of the Phase 3 server primitives, with a `get_system_status`-style end-to-end proving text-fallback vs Apps-Host paths.

**Architecture:** Layering iron rule — Apps→Server, never Server→Apps. Therefore visibility enum + session flags + enforcement live in **server** headers (`tool.h`, `session.h`, typeless `uint32_t` masks so server code never includes apps headers); the **apps** module (`csp.h`, `ui.h`) provides value objects and builders on top. Tool results stay plain JSON; Apps-Host detection is an explicit server-side opt-in (`mcp_session_set_apps_host`), never sniffed from wire params.

**Tech Stack:** C23, CMake + CTest, assert-style unit tests, gcc + clang + ASan/UBSan gates.

---

## Chunk 1: Header contracts (write these FIRST, then TDD each task against them)

### 1A. Server primitives (modify existing headers — no apps dependency)

`include/mcpkit/server/tool.h` — append:

```c
typedef enum mcp_tool_visibility {
    MCP_TOOL_VIS_MODEL = 0,
    MCP_TOOL_VIS_APP = 1,
    MCP_TOOL_VIS_BOTH = 2,
} mcp_tool_visibility_t;

mcp_status_t mcp_tool_set_visibility(mcp_context_t *ctx, mcp_tool_t *tool,
                                     mcp_tool_visibility_t vis);
mcp_status_t mcp_tool_require_perms(mcp_context_t *ctx, mcp_tool_t *tool,
                                    uint32_t perm_mask);
```

- Rules: any NULL arg → `MCP_ERR_INVALID_ARGUMENT`; unknown `vis` (> BOTH) → `MCP_ERR_INVALID_ARGUMENT`. `mcp_tool_new` defaults: vis = BOTH, required = 0. Needs `#include <stdint.h>`.

`include/mcpkit/server/session.h` — append:

```c
#include <stdint.h>

mcp_status_t mcp_session_set_apps_host(mcp_context_t *ctx, mcp_session_t *session, bool apps_host);
bool mcp_session_is_apps_host(mcp_context_t *ctx, const mcp_session_t *session);
mcp_status_t mcp_session_grant(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask);
mcp_status_t mcp_session_revoke(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask);
bool mcp_session_grants(mcp_context_t *ctx, const mcp_session_t *session, uint32_t perm_mask);
```

- Rules: NULL session → INVALID_ARGUMENT for setters; getters on NULL return false. New sessions default: apps_host = false, granted = `0xFFFFFFFFu` (all). `grants` = `(session->granted & mask) == mask`; mask 0 → true.

### 1B. Dispatcher enforcement (modify `src/server/dispatcher.c`, no new headers)

- `route_tools_list`: skip tool `t` for session `s` unless `t->vis == BOTH || (t->vis == APP) == s->apps_host`. Needs session pointer — check current signature (takes `srv, req` only?): if so, thread the session through from the dispatch site (the dispatcher already resolves the session for gating/dup-id; pass it in).
- `route_tools_call` order: find_tool → **visibility check** (fail → `-32601 "tools/call: unknown tool"`, no existence leak) → existing schema validate → **permission check** (`(t->required & ~s->granted) == 0`, fail → `-32602 "tools/call: permission denied"`) → handler.
- `struct mcp_tool` += `mcp_tool_visibility_t vis; uint32_t required;`. `struct mcp_session` += `bool apps_host; uint32_t granted;`. Defaults set in `mcp_tool_new` (registry.c) and session create (session.c) — read both init sites first (T1 Step 0).

### 1C. Apps module (new headers, depend on server + json + core only)

`include/mcpkit/apps/csp.h`:

```c
typedef struct mcp_csp mcp_csp_t;   // opaque

mcp_csp_t *mcp_csp_default_deny_new(mcp_context_t *ctx);  // default-src 'none', rest unset
void mcp_csp_destroy(mcp_context_t *ctx, mcp_csp_t *csp);
mcp_status_t mcp_csp_set(mcp_context_t *ctx, mcp_csp_t *csp, const char *directive, const char *sources_or_null);
// supported directives: "default-src", "script-src", "style-src", "connect-src". Unknown -> INVALID_ARGUMENT.
// sources NULL/"" clears the directive. All strings are copied.
mcp_status_t mcp_csp_serialize(mcp_context_t *ctx, const mcp_csp_t *csp, char **out);
// e.g. "default-src 'none'; script-src 'self'". Caller frees *out with same ctx. No trailing ';'.
// Unset directives are omitted. Deny-all (nothing set but default-src 'none') serializes to "default-src 'none'".
```

`include/mcpkit/apps/ui.h`:

```c
#define MCP_APPS_UI_MIME "text/html;profile=mcp-app"
#define MCP_APPS_UI_SCHEME "ui://"

#define MCP_APPS_PERM_CALL_TOOL (1u << 0)
#define MCP_APPS_PERM_READ_STATE (1u << 1)

typedef struct mcp_session mcp_session_t;  // reuse, no include cycle: forward declare
typedef struct mcp_resource mcp_resource_t;

mcp_resource_t *mcp_apps_ui_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                         const char *html, const mcp_csp_t *csp_or_null);
// uri MUST start with "ui://" else NULL. html is copied. csp NULL -> default-deny embedded.
// Returned resource has mime MCP_APPS_UI_MIME; its reader serves contents=[{uri, mimeType, text: full_doc}]
// where full_doc = html wrapped with <meta http-equiv="Content-Security-Policy" ...> when it has a <head>
// ... NO. Keep honest and simple: full_doc = html AS-IS when it already contains "Content-Security-Policy",
// else prepend the meta tag: "<meta http-equiv=\"Content-Security-Policy\" content=\"S\">\n" + html.
// (Document this exact rule in the plan and the test.)

mcp_status_t mcp_apps_result_with_ui(mcp_context_t *ctx, mcp_json_value_t *result, const char *resource_uri);
// Sets result._meta.ui.resourceUri = uri. result must be an object, uri non-NULL, else INVALID_ARGUMENT.
// Overwrites any existing _meta.ui (documented).

typedef struct mcp_apps_mount mcp_apps_mount_t;  // opaque handle
typedef void (*mcp_apps_lifecycle_fn)(mcp_context_t *ctx, mcp_session_t *session, void *user_data);
mcp_status_t mcp_apps_mount(mcp_context_t *ctx, mcp_session_t *session,
                            mcp_apps_lifecycle_fn on_mount_or_null,
                            mcp_apps_lifecycle_fn on_unmount_or_null,
                            void *user_data, mcp_apps_mount_t **handle_out);
// Runs on_mount immediately (if non-NULL). NULL session/handle_out -> INVALID_ARGUMENT.
mcp_status_t mcp_apps_unmount(mcp_context_t *ctx, mcp_apps_mount_t *handle);
// Runs on_unmount (if non-NULL), frees handle. NULL -> INVALID_ARGUMENT.
// NOTE: explicit-invoke by design (layering: server core never calls into apps).
```

- Apps sources use the `mcp_context_allocator(ctx)->malloc_fn` NULL-ctx-fallback pattern (copied from message.c/transport.c), NOT server-private `srv_*` helpers. Strings: malloc+memcpy manually.

### 1D. Wiring

- `src/apps/csp.c`, `src/apps/ui.c` into the single `mcpkit_core` lib (established pattern) + `mcpkit.h` umbrella additions.
- Tests: `test_visibility` (T1), `test_apps` (T2), `test_apps_e2e` (T3) registered in tests/CMakeLists.txt.
- Examples (T4): `examples/mcp-app/main.c` (stdio serve, plain-text path), `examples/dashboard-app/main.c` (manual apps-session dispatch: init→notify→call→resources/read, prints HTML). Both under `MCPKIT_BUILD_EXAMPLES` in examples/CMakeLists.txt.

---

## Chunk 2: TDD tasks

### T1: Server primitives — visibility + permission enforcement

- [ ] **Step 0: Read init sites.** Read `mcp_tool_new` in src/server/registry.c and session create in src/server/session.c; note exact lines where struct fields are assigned (defaults go there). Also confirm `route_tools_list` caller site for threading the session pointer.
- [ ] **Step 1: Write failing test** `tests/unit/test_visibility.c`:
  - default tool visible to both (list contains it for plain + apps sessions);
  - APP tool hidden from plain session list, shown for apps session; MODEL tool inverse;
  - `tools/call` on hidden tool → error response code `-32601`;
  - `mcp_tool_require_perms` + revoke → call fails `-32602`; grant → passes;
  - NULL guards: `mcp_tool_set_visibility(NULL-kit…)` → INVALID_ARGUMENT; `mcp_tool_set_visibility(ctx, NULL, BOTH)` → INVALID_ARGUMENT; vis=99 → INVALID_ARGUMENT.
  - (Follow test_registry.c/test_dispatcher.c setup patterns: create server, register echo tool, create sessions, build request messages, dispatch.)
- [ ] **Step 2: Configure+build, watch it fail** (undeclared `mcp_tool_set_visibility` etc.).
- [ ] **Step 3: Implement minimal**: tool.h/session.h additions, internals.h fields, registry.c/session.c defaults, dispatcher.c filter + two checks.
- [ ] **Step 4: 25/25 green** (`ctest --test-dir build`).
- [ ] **Step 5: Commit** `feat(server): add tool visibility and permission gating`.

### T2: Apps module — CSP + UI resource + result wrap + lifecycle

- [ ] **Step 1: Write failing test** `tests/unit/test_apps.c`:
  - `mcp_csp_default_deny_new` → serialize == `"default-src 'none'"`;
  - set script-src `'self'` → `"default-src 'none'; script-src 'self'"` (directive order: default, script, style, connect; unset omitted);
  - unknown directive → INVALID_ARGUMENT; clear via NULL sources;
  - `mcp_apps_ui_resource_new` with `https://x` uri → NULL; with `ui://app/main` → non-NULL; reader contents[0].mimeType == `text/html;profile=mcp-app`, text starts with `<meta http-equiv="Content-Security-Policy"` when input html lacks it, passed through when present;
  - `mcp_apps_result_with_ui` on `{content:[]}` → `_meta.ui.resourceUri` == uri; on non-object → INVALID_ARGUMENT; NULL uri → INVALID_ARGUMENT;
  - mount runs on_mount with (session, user_data); unmount runs on_unmount then handle dead (second unmount → INVALID_ARGUMENT? NO — use-after-free is UB; instead assert unmount returns OK once and mount callback ordering via counter). NULL session → INVALID_ARGUMENT.
- [ ] **Step 2: Build, watch it fail** (missing headers).
- [ ] **Step 3: Implement** csp.h/ui.h + csp.c/ui.c; wire CMake + mcpkit.h.
- [ ] **Step 4: 26/26 green** incl. ASan clean (string copies are the leak risk — every `set`/`new` path must free old on replace).
- [ ] **Step 5: Commit** `feat(apps): add CSP, UI resource, result wrap and lifecycle`.

### T3: End-to-end — get_system_status text vs Apps paths

- [ ] **Step 1: Write failing test** `tests/unit/test_apps_e2e.c`:
  - Server with `get_system_status` tool: handler returns `{content:[{type:"text",text:"ok"}]}` ALWAYS, and attaches `_meta.ui` **only if** `mcp_session_is_apps_host(ctx, session)` (this conditional-attach IS the specified fallback behavior — document it).
  - Register `ui://sys/status` UI resource with default-deny CSP.
  - Plain session: init→notify→call → result has `content[0].text=="ok"` and NO `_meta`.
  - Apps session (`mcp_session_set_apps_host(...,true)`): call → result HAS `_meta.ui.resourceUri=="ui://sys/status"`; then `resources/read` that uri → contents[0].text contains `<meta` + `default-src 'none'` + original html marker.
  - MODEL-visibility status tool hidden from apps session list (cross-check T1 in e2e).
- [ ] **Step 2–4: Implement (no new src expected — test-only task), 27/27 green.**
- [ ] **Step 5: Commit** `test(apps): prove text-fallback vs apps-host end to end`.

### T4: Examples — mcp-app (plain stdio) + dashboard-app (apps session)

- [ ] **Step 1: `examples/mcp-app/main.c`** — mirror examples/stdio-server lifecycle (start→serve→stop): registers `get_system_status` (text-only, plain host) + `ui://sys/status` resource. Prints nothing itself.
- [ ] **Step 2: `examples/dashboard-app/main.c`** — manual dispatch (create server+session, enable apps host, parse/build messages via message API, `mcp_server_dispatch`-equivalent used in tests, serialize responses to stdout): init → initialized-notify → tools/call → resources/read `ui://sys/status` → print HTML. Follow test_apps_e2e.c flow; keep under ~150 lines.
- [ ] **Step 3: Register both in examples/CMakeLists.txt** (MCPKIT_BUILD_EXAMPLES gate, established pattern).
- [ ] **Step 4: Run both**: `printf init/notify/call | ./mcp-app` → text result rc=0; `./dashboard-app` → HTML with CSP meta rc=0.
- [ ] **Step 5: Commit** `feat(examples): add mcp-app and dashboard-app`.

### T5: CHANGELOG + triple gates + cleanup

- [ ] **Step 1: CHANGELOG.md Phase 7 entry** (one line, established style).
- [ ] **Step 2: Triple gates from clean configures**: gcc 27/27, clang 27/27, `-fsanitize=address,undefined` 27/27 (verify libasan linkage via ldd, lesson from Phase 2).
- [ ] **Step 3: `rm -rf` all build dirs; `git status` clean except intended files.**
- [ ] **Step 4: Commit** `chore: log phase 7 apps` (then await merge instruction — standing instruction says auto-merge to master and continue to P8).
