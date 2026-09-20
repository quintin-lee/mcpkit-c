# Advanced Protocol Methods (Phase 5) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement and route all 15 currently-unrouted spec methods so the L2 validation table and the dispatcher route stay a single honest source, and the server can originate requests (roots/list, sampling/createMessage, elicitation/create) to the client.

**Architecture:**
- Client→server requests get a new `route_advanced` handler in dispatcher.c that switches on method, backed by new per-server state in internals.h (a log-level floor, a resource-subscription set, a server-originated id counter).
- Client→server notifications (cancelled/progress) are consumed by `mcp_server_notify` (counter + DEBUG log), not routed as requests.
- Server→client *notifications* (4×list_changed, logging/message) already flow through `mcp_server_notify_client` — this phase only adds the missing *request* push primitive `mcp_server_request_client` for the 3 server-initiated requests.
- `k_l2_only_methods` shrinks to zero; every known method is either routed or (notifications/initialized) consumed by notify. Method table stays the single source of truth.

**Tech Stack:** C23, no new deps. Reuse mcp_server state, outbox, counters, dlogf.

## Locked decisions
- `logging/setLevel` sets a **per-server floor** (mcp_log_level_t), consulted by dlogf; the floor starts at MCP_LOG_DEBUG. Spec level strings map: debug/info/notice→MCP_LOG_INFO? No — spec has 5: debug,info,notice,warning,error. Our logger has 4 (DEBUG,INFO,WARN,ERROR). Mapping: debug→DEBUG, info→INFO, notice→INFO, warning→WARN, error→ERROR. Unknown string → -32602.
- `resources/subscribe`/`unsubscribe` maintain a **per-server set of uri strings** (growable, dedup); both return `{}` result. unsubscribe of an un-subscribed uri still returns OK (idempotent, spec-silent).
- `resources/templates/list` returns `{"templates":[]}` — honest: this build has no templates.
- `notifications/cancelled` + `notifications/progress` are **accepted silently** (notification counter++ + DEBUG log with method), no response (they are notifications).
- `mcp_server_request_client(ctx, server, method, params)` builds a **number-id request** using a per-server monotonic counter (starts 1.0), TAKES params on OK / retains on ERR, pushes to outbox. Same outbox-drain contract as notify_client.
- The 4 list_changed + logging/message need **no new code** — host calls `mcp_server_notify_client(ctx, server, "notifications/tools/list_changed", params)`. Documented in server.h.

## Files
- Modify: `src/server/internals.h` (struct mcp_server += `mcp_log_level_t log_floor`, `char **subscribed_uris; size_t n_subscribed; size_t cap_subscribed;`, `double next_server_id`)
- Modify: `src/server/server.c` (init new fields in create; free subscribed_uris in destroy)
- Modify: `include/mcpkit/server/server.h` (declare `mcp_server_request_client`; doc note that list_changed/logging-message go via notify_client)
- Modify: `src/server/dispatcher.c` (new `route_advanced`; wire into route_request; extend mcp_server_notify to consume cancelled/progress; dlogf consults log_floor; implement mcp_server_request_client)
- Modify: `src/protocol/validate.c` (L3: add param rules for the 4 new routed requests; drop k_l2_only_methods to empty or remove the array; mcp_method_known simplifies)
- Modify: `src/method_table.h` (move the now-routed methods into k_mcp_server_methods; update count + _Static_assert)
- Create: `tests/unit/test_advanced_methods.c` (TDD regression for all 15)
- Modify: `tests/CMakeLists.txt` (register test, unconditional)
- Modify: `docs/module-reference.md`, `CHANGELOG.md` (closeout)

---

## Chunk 1: Task T1 — server state + logging/setLevel + subscribe/unsubscribe + templates/list

### Task 1: Per-server advanced state and the 4 client→server request routes

**Files:**
- Modify: `src/server/internals.h:68` (struct mcp_server)
- Modify: `src/server/server.c` (create + destroy)
- Modify: `src/server/dispatcher.c` (route_advanced + route_request + dlogf)
- Modify: `src/protocol/validate.c` (L3 rules for logging/setLevel, resources/subscribe, resources/unsubscribe; remove resources/templates/list from k_l2_only since it's now routed)
- Modify: `src/method_table.h` (add the 4 to k_mcp_server_methods; bump MCP_SERVER_METHOD_COUNT 11→15; _Static_assert 15)
- Test: `tests/unit/test_advanced_methods.c` (new, TDD-first)

- [ ] **Step 1: Write the failing test (T1 portion)**

Create `tests/unit/test_advanced_methods.c`. Include: core/context.h, logging/logger.h, protocol/message.h, server/server.h, server/dispatcher.h, test_check.h, string.h, stdlib.h, stdbool.h.

Build helper `dispatch_new(ctx,srv,sess,method,params)`: mcp_request_new_number_id with a fresh number id via a global counter, mcp_server_dispatch, return resp. Use a `mem_sink` logger (like test_observability.c) attached to ctx so setLevel can be observed through the logger's level... actually setLevel sets the *server* floor, not the logger. Observe the floor's effect via dlogf: after setting floor=ERROR, a dispatch of an unknown method should NOT log a WARN (the unknown_method event is WARN, below floor ERROR, so suppressed). Capture with mem_sink counter.

Test cases:
1. **logging/setLevel happy**: dispatch `logging/setLevel` with params `{"level":"warn"}` → resp result is `{}` (empty object), no error. Then dispatch `logging/setLevel` `{"level":"bogus"}` → error code -32602 (INVALID_PARAMS).
2. **resources/subscribe dedup**: dispatch subscribe `{"uri":"res://a"}` twice → both OK, result `{}`. Dispatch unsubscribe `{"uri":"res://a"}` → OK `{}`.
3. **resources/templates/list**: dispatch → result has key "templates" which is an empty array (mcp_json_array_get size 0).
4. **NULL guard**: route via mcp_server_dispatch with NULL params for subscribe → -32602 (L3 require uri). (subscribe requires uri param.)

For the floor effect on dlogf, add a focused check: create server, set floor via dispatch setLevel to "error", then dispatch `roots/list` (which at T1 time is still L2-only → -32601 unknown_method WARN). The mem_sink should record 0 warnings (suppressed by floor). Then reset floor to debug (dispatch setLevel "debug"), dispatch roots/list again, now 1 warning recorded. This proves setLevel actually drives dlogf.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j && ctest --test-dir build -R test_advanced_methods --output-on-failure`
Expected: FAIL (route not implemented → unknown_method -32601 for the 4 methods; test asserts OK).

- [ ] **Step 3: Implement internals.h + server.c state**

In `src/server/internals.h`, add to `struct mcp_server` (after cap_outbox):
```c
    // Advanced-method state (Phase 5): log floor for dlogf, resource
    // subscription set, and the server-originated request id counter.
    mcp_log_level_t log_floor;
    char **subscribed_uris;
    size_t n_subscribed;
    size_t cap_subscribed;
    double next_server_id;
```
Add `#include "mcpkit/logging/log.h"` to internals.h.

In `src/server/server.c` `mcp_server_create`, after memset: `srv->log_floor = MCP_LOG_DEBUG; srv->next_server_id = 1.0;` (other fields already 0 from memset). In `mcp_server_destroy`, add `free_subscribed(ctx, srv);` before `srv_free(ctx, srv->name);`:
```c
static void free_subscribed(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_subscribed; i++) {
        srv_free(ctx, srv->subscribed_uris[i]);
    }
    srv_free(ctx, srv->subscribed_uris);
}
```

- [ ] **Step 4: Implement route_advanced in dispatcher.c**

Add a `route_advanced(ctx, srv, s, req, method)` static that handles the 4 requests. Place near route_request. It needs helpers: a `result_empty(ctx)` returning an empty object.
```c
static mcp_message_t *route_advanced(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                     const mcp_message_t *req, const char *method) {
    if (strcmp(method, "logging/setLevel") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, req);
        const char *lvl = NULL;
        if (params == NULL || get_string(ctx, params, "level", &lvl) != MCP_OK || lvl == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "logging/setLevel: missing level");
        }
        // spec level -> logger level
        mcp_log_level_t mapped;
        if (strcmp(lvl, "debug") == 0)      mapped = MCP_LOG_DEBUG;
        else if (strcmp(lvl, "info") == 0)   mapped = MCP_LOG_INFO;
        else if (strcmp(lvl, "notice") == 0)  mapped = MCP_LOG_INFO;
        else if (strcmp(lvl, "warning") == 0) mapped = MCP_LOG_WARN;
        else if (strcmp(lvl, "error") == 0)  mapped = MCP_LOG_ERROR;
        else return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "logging/setLevel: bad level");
        srv->log_floor = mapped;
        return finish(ctx, req, empty_obj(ctx));   // empty_obj returns result object or NULL
    }
    if (strcmp(method, "resources/subscribe") == 0 ||
        strcmp(method, "resources/unsubscribe") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, req);
        const char *uri = NULL;
        if (params == NULL || get_string(ctx, params, "uri", &uri) != MCP_OK || uri == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/subscribe: missing uri");
        }
        if (method[10] == 's') { /* subscribe: add if absent */
            for (size_t i = 0; i < srv->n_subscribed; i++)
                if (strcmp(srv->subscribed_uris[i], uri) == 0) break;
            else { /* grow + append srv_strdup(uri); NOMEM -> return NULL */ }
        } else { /* unsubscribe: remove first match (idempotent) */ }
        return finish(ctx, req, empty_obj(ctx));
    }
    if (strcmp(method, "resources/templates/list") == 0) {
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (result == NULL || arr == NULL) { mcp_json_destroy(ctx,result); mcp_json_destroy(ctx,arr); return NULL; }
        if (mcp_json_object_set_take(ctx, result, "templates", arr) != MCP_OK) { mcp_json_destroy(ctx,result); return NULL; }
        return finish(ctx, req, result);
    }
    return NULL; // not an advanced method
}
```
Add `finish(ctx, req, result)` helper = `mcp_response_ok_new` with result-destroy-on-fail (mirror the existing `err_resp`/ok pattern used by route_tools_list). Add `empty_obj(ctx)` = `mcp_json_object_new`.

In `route_request`, add before the final unknown_method fallback:
```c
    mcp_message_t *adv = route_advanced(ctx, srv, s, req, method);
    if (adv != NULL) return adv;
```

Modify `dlogf` to consult the floor: dlogf needs the server. But dlogf is static and called from dispatch with only ctx. The floor lives on the server. Refactor: make a `dlogf_srv(ctx, srv, level, fmt, ...)` that checks `if (level < srv->log_floor) return;` then forwards to the logger. Replace the 8 call sites' `dlogf` with `dlogf_srv(ctx, srv, ...)`. (All dispatch call sites have `srv` in scope.) Keep `dlogf` for the loop/stdio serve-start stations (no server there) — those stay floor-independent (logger-level only). Actually the serve loops in loop.c/stdio.c use their own inline dlogf; they have no srv floor, that's fine.

- [ ] **Step 5: L3 param rules + method table**

In `validate.c` `mcp_validate_params`, add branches:
```c
    } else if (strcmp(method, "logging/setLevel") == 0) {
        if (require_string_param(ctx, params, "level") != MCP_OK) goto invalid;
    } else if (strcmp(method, "resources/subscribe") == 0 ||
               strcmp(method, "resources/unsubscribe") == 0) {
        if (require_string_param(ctx, params, "uri") != MCP_OK) goto invalid;
    }
```
`resources/templates/list` needs no params (empty result) — leave it in the params==NULL allowed path.

In `src/method_table.h`: move `logging/setLevel`, `resources/subscribe`, `resources/unsubscribe`, `resources/templates/list` from k_l2_only_methods (in validate.c) into `k_mcp_server_methods` (method_table.h). Bump `MCP_SERVER_METHOD_COUNT` 11→15 and the `_Static_assert` to 15. Remove those 4 from validate.c's `k_l2_only_methods` (leaves 11). Note: route_request now must route indices 10–13 (the new 4) via route_advanced, and index 14 is notifications/initialized (still not routed — consumed by notify). Update the route_request loop bounds: routed methods are k_mcp_server_methods[0..13]; index 14 (notifications/initialized) not routed.

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R test_advanced_methods --output-on-failure`
Expected: PASS (T1 cases).

- [ ] **Step 7: Full ctest + commit**

Run: `ctest --test-dir build --output-on-failure` (expect 43/43 — 42 + new test). Then commit T1.

```bash
git add src/server/internals.h src/server/server.c src/server/dispatcher.c src/protocol/validate.c src/method_table.h tests/unit/test_advanced_methods.c tests/CMakeLists.txt
git commit -m "feat(server): route logging/setLevel, resources/subscribe|unsubscribe, resources/templates/list

- per-server log_floor consulted by dlogf; setLevel maps spec level strings
- per-server resource-subscription set (dedup on subscribe, idempotent unsubscribe)
- templates/list returns honest empty {templates: []}
- method_table now routes 15 of the known spec methods; L2-only list shrinks to 11"
```

---

## Chunk 2: Task T2 — notification consumption + server-originated request push

### Task 2: Consume cancelled/progress; add mcp_server_request_client for server-initiated requests

**Files:**
- Modify: `include/mcpkit/server/server.h` (declare mcp_server_request_client + doc)
- Modify: `src/server/dispatcher.c` (mcp_server_notify consumes cancelled/progress; implement mcp_server_request_client)
- Modify: `src/protocol/validate.c` (add cancelled/progress to L2-known; they are notifications so L3 allows params==NULL)
- Modify: `src/method_table.h` (move notifications/cancelled, notifications/progress into k_mcp_server_methods; they are consumed-by-notify not routed; keep out of route_request)
- Test: `tests/unit/test_advanced_methods.c` (add cases)

Design note: `notifications/cancelled` and `notifications/progress` are **client→server notifications**. They pass L2 (known) and L3 (no params required). They are NOT routed as requests — they are consumed by `mcp_server_notify`. So in method_table.h they join the "known" set but route_request must NOT handle them (they arrive as NOTIFICATION kind, which goes through mcp_server_notify, not route_request). mcp_server_notify currently only special-cases initialized; extend it to count+DEBUG-log cancelled/progress.

Server→client requests (roots/list, sampling/createMessage, elicitation/create) stay in k_l2_only_methods (validate accepts them as spec-known) but are NOT routed by the server (the server *sends* them via the new primitive, it doesn't *receive* them). The new `mcp_server_request_client` lets a host send them. So method_table's "routed" set = 15 (T1) ; "known-not-routed" = roots/list, sampling/createMessage, elicitation/create (3, server-originated, sent via request_client) + the 4 list_changed + logging/message (5, server-originated notifications, sent via notify_client) + cancelled/progress moved into routed-as-notification... 

To keep the table honest and simple: 
- `k_mcp_server_methods` = the 15 routed requests (T1) — route_request handles [0..14) and index 14 (notifications/initialized) is the notify-consumed one.
- Add `notifications/cancelled`, `notifications/progress` to a **new** `k_mcp_server_notifications[]` consumed by mcp_server_notify (init + these 2 = 3 known notifications).
- `k_l2_only_methods` (still in validate.c) = the 8 server-originated names: roots/list, sampling/createMessage, elicitation/create, notifications/tools|resources|prompts/list_changed, notifications/resources/updated, logging/message. These pass L2 (spec-known) and are sent by the server via request_client/notify_client, never received+routed.

So mcp_method_known loops k_mcp_server_methods (15) + k_mcp_server_notifications (2 new + initialized counted in methods) + k_l2_only_methods (8). Total still 25 known names (matches the original P2 "25-method" table). Honest.

- [ ] **Step 1: Write failing test (T2 portion, appended)**

In test_advanced_methods.c add:
1. **cancelled consumed**: build a NOTIFICATION `notifications/cancelled` with params `{"requestId": 1.0, "reason":"x"}`, feed via mcp_server_dispatch? No — dispatch rejects non-request. Feed via `mcp_server_notify(ctx, srv, sess, notif)`. Assert returns MCP_OK and c_notifications_total incremented (read via mcp_server_counters).
2. **progress consumed**: same for `notifications/progress` `{"progressToken":"t","progress":0.5,"total":1.0}`. OK + counter.
3. **request_client push**: `mcp_server_request_client(ctx, srv, "sampling/createMessage", params_obj)` → MCP_OK; then `mcp_server_outbox_pop` returns a non-NULL message whose kind==MCP_MSG_REQUEST, method=="sampling/createMessage", id_number>=1.0. Second call → next id (counter increments). NULL params with a valid method → still builds (params optional); NULL method → INVALID_ARGUMENT.
4. **request_client NOMEM guard**: NULL server → INVALID_ARGUMENT.

- [ ] **Step 2: Run, expect FAIL**

`ctest --test-dir build -R test_advanced_methods --output-on-failure` → FAIL (request_client undeclared).

- [ ] **Step 3: Implement mcp_server_notify extension + mcp_server_request_client**

In dispatcher.c `mcp_server_notify`, after the initialized check, add:
```c
    // cancelled / progress are accepted silently (client -> server).
    if (method != NULL &&
        (strcmp(method, "notifications/cancelled") == 0 ||
         strcmp(method, "notifications/progress") == 0)) {
        dlogf_srv(ctx, srv, MCP_LOG_DEBUG, "event=notification method=%s", method);
    }
```
(counter++ already happens unconditionally below.)

Implement `mcp_server_request_client` in dispatcher.c (or server.c; it touches outbox which is in server.c — but it builds a message, so dispatcher.c near route is fine; outbox access via srv fields):
```c
mcp_status_t mcp_server_request_client(mcp_context_t *ctx, mcp_server_t *srv,
                                       const char *method, mcp_json_value_t *params) {
    if (srv == NULL || method == NULL) return MCP_ERR_INVALID_ARGUMENT;
    mcp_message_t *req = mcp_request_new_number_id(ctx, srv->next_server_id, method, params);
    if (req == NULL) return MCP_ERR_NOMEM;   // builder retained params on fail -> no leak
    srv->next_server_id += 1.0;
    // push onto outbox (growable), mirroring notify_client's append path
    if (srv->n_outbox == srv->cap_outbox) {
        size_t ncap = srv->cap_outbox == 0 ? 4 : srv->cap_outbox * 2;
        mcp_message_t **no = srv_realloc(ctx, srv->outbox, ncap * sizeof(*no));
        if (no == NULL) { mcp_message_destroy(ctx, req); return MCP_ERR_NOMEM; }
        srv->outbox = no; srv->cap_outbox = ncap;
    }
    srv->outbox[srv->n_outbox++] = req;
    return MCP_OK;
}
```
NOTE: builder TAKES params on OK and retains on NULL, so on NOMEM the params are already consumed by the builder — no leak. This is the message.c caller-retains-on-NULL contract already verified in Phase 1 review.

Declare in server.h with Doxygen: `mcp_status_t mcp_server_request_client(mcp_context_t *ctx, mcp_server_t *server, const char *method, mcp_json_value_t *params);` — document: TAKES params on OK, retains on NOMEM; appends a number-id request to the outbox drained by serve loops; server id counter is monotonic per-server; use for roots/list, sampling/createMessage, elicitation/create and any server-originated request.

- [ ] **Step 4: validate.c + method_table.h updates**

Add `notifications/cancelled`, `notifications/progress` to mcp_method_known (via a new k_mcp_server_notifications[] in method_table.h, or just append to k_l2_only_methods in validate.c — simpler: keep them in k_l2_only_methods since they're "known but not request-routed"). Actually to be honest, cancelled/progress ARE consumed (by notify), so they belong in the routed-as-notification set. Add `k_mcp_server_notifications[] = {"notifications/initialized","notifications/cancelled","notifications/progress"}` to method_table.h with a count macro; mcp_method_known loops it. route_request ignores them (they're NOTIFICATION kind). mcp_server_notify consumes all 3 (initialized sets flag; cancelled/progress count+log).

k_l2_only_methods (validate.c) now = 8 server-originated names: roots/list, sampling/createMessage, elicitation/create, notifications/tools/list_changed, notifications/resources/list_changed, notifications/resources/updated, notifications/prompts/list_changed, logging/message. These pass L2 so a host *receiving* them from a client is spec-valid but this server build sends (not receives) them.

- [ ] **Step 5: Run test, expect PASS; full ctest; commit**

`ctest --test-dir build -R test_advanced_methods` PASS; full `ctest` 43/43. Commit:
```
feat(server): consume cancelled/progress notifications; add mcp_server_request_client

- mcp_server_notify now accepts notifications/cancelled and /progress silently
  (counter + DEBUG log) alongside initialized
- new mcp_server_request_client pushes a server-originated number-id request
  (roots/list, sampling/createMessage, elicitation/create) onto the outbox
- method table: known notifications (3) + routed requests (15) + server-originated
  (8) = 25 spec-known names, all honest
```

---

## Chunk 3: Task T3 — docs + CHANGELOG closeout

### Task 3: Document Phase 5

**Files:**
- Modify: `docs/module-reference.md` (server.h section: request_client row; validate.h L2 line; dispatcher.h note)
- Modify: `CHANGELOG.md` ([Unreleased] Added)

- [ ] **Step 1: module-reference.md**

server.h section: add row `mcp_server_request_client(ctx, server, method, params)` — TAKES params on OK/retains on NOMEM, appends number-id request to outbox (drained by serve loops); for roots/list, sampling/createMessage, elicitation/create. Note list_changed ×4 + logging/message flow via mcp_server_notify_client. Note setLevel/subscribe/unsubscribe/templates/list now routed (per-server log floor + subscription set).

validate.h L2 line: update "shared 15-method server table + 3 known notifications + 8 server-originated (k_l2_only); request hitting an unrouted server-originated name -> -32601".

- [ ] **Step 2: CHANGELOG [Unreleased] Added**

Bullets: "Advanced protocol methods: route logging/setLevel (per-server log floor), resources/subscribe|unsubscribe (per-server subscription set), resources/templates/list (honest empty); consume notifications/cancelled|progress; add mcp_server_request_client push primitive for roots/list, sampling/createMessage, elicitation/create; method table now covers all 25 spec-known names."

- [ ] **Step 3: Doxygen regen (0 warnings) + triple gates + commit + cleanup**

Run doxygen (0 warnings, expect new group page for the advanced API is not needed — it's server.h which is already grouped). Triple gates gcc/clang/ASan+UBSan with -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON, all 43/43. Remove build dirs. Commit docs.

```
docs: cover Phase 5 advanced protocol methods
```

## Execution
Use superpowers:executing-plans (no subagents in this harness) — execute T1→T2→T3 in this session, batching with review checkpoints. TDD: test first, confirm fail, implement, confirm pass, commit per task.
