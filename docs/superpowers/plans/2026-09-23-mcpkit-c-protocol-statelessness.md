# Protocol Statelessness + _meta + server/discover Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `_meta` bidirectional support, `resultType="complete"` on all success responses, host-configurable `ttlMs`/`cacheScope` on list responses, and a `server/discover` capability endpoint to the MCP protocol layer.

**Architecture:** Three layers change: (1) `message.h/c` adds two result-decorator functions and a `mcp_message_meta` accessor; (2) `session.h/c` + `internals.h` add a `client_meta` field; (3) `server.h/c` adds two setter APIs and three new struct fields; `dispatcher.c` adds a `decorate_result` internal helper called from every success route, plus a new `route_server_discover`; `method_table.h` and `validate.c` gain the `server/discover` entry. All changes are additive; no existing API signature changes.

**Tech Stack:** C23, mcpkit_core static lib, existing JSON DOM + allocator infrastructure.

---

## File Map

| File | Role |
|---|---|
| `include/mcpkit/protocol/message.h` | Add `mcp_message_meta`, `mcp_result_inject_result_type`, `mcp_result_inject_meta` declarations |
| `src/protocol/message.c` | Implement the three functions above |
| `include/mcpkit/server/session.h` | Add `mcp_session_client_meta` declaration |
| `src/server/internals.h` | Add `client_meta` field to `struct mcp_session`; add 3 fields to `struct mcp_server` (`list_ttl_ms`, `list_cache_scope`, `response_meta`) |
| `src/server/session.c` | Init `client_meta=NULL` in `session_new`; destroy in `session_free`; implement `mcp_session_client_meta` |
| `include/mcpkit/server/server.h` | Add `mcp_server_set_list_cache`, `mcp_server_set_response_meta` declarations |
| `src/server/server.c` | Implement both setters; add `free_response_meta` to destroy path; init new fields in create |
| `src/method_table.h` | Add `"server/discover"` to `k_mcp_server_methods`; sync `_Static_assert` count 15→16 |
| `src/protocol/validate.c` | L3: add no-param rule for `server/discover` |
| `src/server/dispatcher.c` | Add `decorate_result` helper; call it from 8 success routes; add `route_server_discover`; update `route_initialize` to clone `mcp_message_meta` into `s->client_meta`; wire `server/discover` into `route_request` chain |
| `tests/unit/test_protocol_statelessness.c` | New TDD test (10 cases) |
| `tests/CMakeLists.txt` | Register `test_protocol_statelessness` |
| `docs/module-reference.md` | Add new API lines |
| `CHANGELOG.md` | Add [Unreleased] bullet |

---

## Standing Conventions (do not change)

- ctx first arg; NULL ctx = libc + builtin default
- container-owns-on-OK / caller-retains-on-ERR
- `mcp_json_object_set_take` always-consumes-val (success or fail)
- borrowed const-ptr getters
- free-with-same-ctx
- RPC codes -32700/-32600/-32601/-32602/-32603
- C23, zero-dep core
- Triple gate: gcc/clang/ASan+UBSan with `-DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON` + ldd verify
- Work on master directly; conventional commit per logical unit; rm build dirs; tree clean

---

## Chunk 1: message layer + session storage + server setters

### Task 1: Add `mcp_message_meta`, `mcp_result_inject_result_type`, `mcp_result_inject_meta`

**Files:**
- Modify: `include/mcpkit/protocol/message.h` (add 3 declarations after `mcp_message_error_text`)
- Modify: `src/protocol/message.c` (implement all 3)

- [ ] **Step 1: Add declarations to `message.h`**

Insert after the `mcp_message_error_text` declaration block (before the `/* --- Builders --- */` section):

```c
/**
 * @brief Returns the top-level `"_meta"` field of the request.
 *
 * The client may carry extension metadata in a top-level `_meta` object
 * on any JSON-RPC request. This accessor returns a BORROWED pointer valid
 * while `msg` is alive; NULL if the key is absent or the value is not an
 * object.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed `_meta` value, or NULL.
 */
const mcp_json_value_t *mcp_message_meta(mcp_context_t *ctx, const mcp_message_t *msg);
```

Insert in the Builders section (after `mcp_response_err_new`, before `mcp_message_serialize`):

```c
/**
 * @brief Injects `"resultType": "complete"` into a result object.
 *
 * Signals to the client that this response is a complete result, not a
 * partial/streaming fragment. No allocation is made (the string literal
 * is static).
 *
 * @param ctx     Context.
 * @param result  Result object to decorate (must be non-NULL and must be
 *                a JSON object; NOMEM/INVALID_ARGUMENT otherwise).
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_INVALID_ARGUMENT if `result` is NULL or not an object.
 */
mcp_status_t mcp_result_inject_result_type(mcp_context_t *ctx, mcp_json_value_t *result);

/**
 * @brief Injects a `_meta` object into a result object.
 *
 * @param ctx     Context.
 * @param result  Result object to decorate (must be a JSON object).
 * @param meta    Meta value; caller-owned. On success ownership transfers
 *                to `result` (destroyed with the result). On failure the
 *                caller retains ownership of `meta`.
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_INVALID_ARGUMENT if `result` is not an object.
 */
mcp_status_t mcp_result_inject_meta(mcp_context_t *ctx, mcp_json_value_t *result,
                                     mcp_json_value_t *meta);
```

- [ ] **Step 2: Implement `mcp_message_meta` in `message.c`**

Add after the existing accessor implementations (after `mcp_message_error_text` impl):

```c
const mcp_json_value_t *mcp_message_meta(mcp_context_t *ctx, const mcp_message_t *msg) {
    if (msg == NULL) {
        return NULL;
    }
    const mcp_json_value_t *v = mcp_json_object_get(ctx, msg->dom, "_meta");
    if (v == NULL || mcp_json_type(ctx, v) != MCP_JSON_OBJECT) {
        return NULL;
    }
    return v;
}
```

- [ ] **Step 3: Implement `mcp_result_inject_result_type` in `message.c`**

Add before `mcp_message_serialize`:

```c
mcp_status_t mcp_result_inject_result_type(mcp_context_t *ctx, mcp_json_value_t *result) {
    if (result == NULL || mcp_json_type(ctx, result) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *s = mcp_json_string_new(ctx, "complete");
    if (s == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, result, "resultType", s) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}
```

- [ ] **Step 4: Implement `mcp_result_inject_meta` in `message.c`**

```c
mcp_status_t mcp_result_inject_meta(mcp_context_t *ctx, mcp_json_value_t *result,
                                     mcp_json_value_t *meta) {
    if (result == NULL || mcp_json_type(ctx, result) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (meta == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_json_object_set_take(ctx, result, "_meta", meta) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}
```

- [ ] **Step 5: Build verify 0 errors**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j4
```
Expected: 0 errors, 0 warnings.

- [ ] **Step 6: Commit**

```bash
git add include/mcpkit/protocol/message.h src/protocol/message.c
git commit -m "feat(protocol): add mcp_message_meta accessor and result decorator helpers"
```

---

### Task 2: Add `client_meta` to session + `mcp_session_client_meta` accessor

**Files:**
- Modify: `src/server/internals.h` (add `client_meta` field to `struct mcp_session`)
- Modify: `include/mcpkit/server/session.h` (add declaration)
- Modify: `src/server/session.c` (init + destroy + accessor impl)

- [ ] **Step 1: Add field to `internals.h` `struct mcp_session`**

After `mcp_idset_t *ids;`:

```c
// Deep clone of the "_meta" field carried in the initialize request.
// NULL if the client did not send one. Destroyed with the session.
mcp_json_value_t *client_meta;
```

Also add the forward declaration near the top of `internals.h` (after existing includes):

```c
typedef struct mcp_json_value mcp_json_value_t;
```

- [ ] **Step 2: Add `mcp_session_client_meta` declaration to `session.h`**

Insert after the existing `mcp_session_grants` declaration:

```c
/**
 * @brief Returns the `_meta` object stored on the session from the
 *        initialize request, or NULL if the client did not send one.
 *
 * The returned pointer is BORROWED; valid while the session is alive.
 *
 * @param ctx     Context.
 * @param session Session to inspect; NULL returns NULL.
 * @return Borrowed `_meta` value, or NULL.
 */
const mcp_json_value_t *mcp_session_client_meta(mcp_context_t *ctx,
                                                 const mcp_session_t *session);
```

Add the forward declaration at the top of `session.h` (before the first function):

```c
typedef struct mcp_json_value mcp_json_value_t;
```

- [ ] **Step 3: Update `session_new` and `session_free` in `session.c`**

In `session_new`, the `memset(s, 0, ...)` already zero-initializes `client_meta = NULL`.
No change needed there.

In `session_free`, before `srv_free(ctx, s->client_name)`:

```c
mcp_json_destroy(ctx, s->client_meta);
```

- [ ] **Step 4: Implement `mcp_session_client_meta` in `session.c`**

Add at end of file (before the `#endif` equivalent — just append):

```c
const mcp_json_value_t *mcp_session_client_meta(mcp_context_t *ctx,
                                                 const mcp_session_t *session) {
    (void)ctx;
    if (session == NULL) {
        return NULL;
    }
    return session->client_meta;
}
```

- [ ] **Step 5: Build verify 0 errors**

```bash
cmake --build build -j4
```
Expected: 0 errors.

- [ ] **Step 6: Commit**

```bash
git add src/server/internals.h include/mcpkit/server/session.h src/server/session.c
git commit -m "feat(server): add client_meta field to session and mcp_session_client_meta accessor"
```

---

### Task 3: Add server setter fields + `mcp_server_set_list_cache` / `mcp_server_set_response_meta`

**Files:**
- Modify: `src/server/internals.h` (add 3 fields to `struct mcp_server`)
- Modify: `include/mcpkit/server/server.h` (add 2 declarations)
- Modify: `src/server/server.c` (implement setters + init + destroy)

- [ ] **Step 1: Add fields to `struct mcp_server` in `internals.h`**

After `pthread_mutex_t subscribed_lock;`:

```c
// List-response cache parameters; 0/NULL = omit from list responses.
// Set via mcp_server_set_list_cache; overridable; no lock needed
// (read in dispatch threads, written by host before serving starts).
uint64_t list_ttl_ms;
char *list_cache_scope;
// Host-supplied _meta object injected into list/discover responses.
// Cloned at set time; NULL = no _meta injection.
mcp_json_value_t *response_meta;
```

Add `<stdint.h>` to the includes at the top of `internals.h` if not already present (it is).

- [ ] **Step 2: Add declarations to `server.h`**

Insert after the existing `mcp_server_set_tracer` declaration:

```c
/**
 * @brief Configures the `ttlMs` and `cacheScope` fields carried in
 *        list-type responses (tools/list, resources/list, prompts/list,
 *        completion/list, server/discover).
 *
 * Passing `ttl_ms == 0` omits `ttlMs`; passing `cache_scope_or_null == NULL`
 * omits `cacheScope`. Multiple calls overwrite the previous values.
 *
 * @param ctx             Context.
 * @param srv             Server (non-NULL required).
 * @param ttl_ms          Cache TTL in milliseconds; 0 = omit.
 * @param cache_scope_or_null  Cache scope string; NULL = omit.
 * @return MCP_OK; MCP_ERR_INVALID_ARGUMENT if srv is NULL.
 */
mcp_status_t mcp_server_set_list_cache(mcp_context_t *ctx, mcp_server_t *srv,
                                       uint64_t ttl_ms, const char *cache_scope_or_null);

/**
 * @brief Sets the `_meta` object injected into list-type and discover
 *        responses by the server.
 *
 * `meta_json` is caller-owned; on success ownership transfers to the server
 * (the server clones it for each response, so the host may keep its
 * original). On failure the caller retains ownership.
 * Pass NULL to clear (subsequent responses will not carry `_meta`).
 *
 * @param ctx        Context.
 * @param srv        Server (non-NULL required).
 * @param meta_json  Meta object; may be NULL to clear.
 * @return MCP_OK; MCP_ERR_NOMEM on clone failure;
 *         MCP_ERR_INVALID_ARGUMENT if srv is NULL.
 */
mcp_status_t mcp_server_set_response_meta(mcp_context_t *ctx, mcp_server_t *srv,
                                           mcp_json_value_t *meta_json);
```

Add `#include <stdint.h>` to `server.h` if not already present.

- [ ] **Step 3: Implement setters + init + destroy in `server.c`**

In `mcp_server_create`, after `srv->next_server_id = 1000.0;`:

```c
srv->list_ttl_ms = 0;
srv->list_cache_scope = NULL;
srv->response_meta = NULL;
```

Before `mcp_server_destroy`'s `srv_free(ctx, srv->name)`:

```c
mcp_json_destroy(ctx, srv->response_meta);
srv_free(ctx, srv->list_cache_scope);
```

Add the two setter implementations (after `mcp_server_set_tracer` in `server.c`):

```c
mcp_status_t mcp_server_set_list_cache(mcp_context_t *ctx, mcp_server_t *srv,
                                       uint64_t ttl_ms, const char *cache_scope_or_null) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    srv->list_ttl_ms = ttl_ms;
    char *scope_copy = (cache_scope_or_null != NULL) ? srv_strdup(ctx, cache_scope_or_null) : NULL;
    if (cache_scope_or_null != NULL && scope_copy == NULL) {
        return MCP_ERR_NOMEM;
    }
    srv_free(ctx, srv->list_cache_scope);
    srv->list_cache_scope = scope_copy;
    return MCP_OK;
}

mcp_status_t mcp_server_set_response_meta(mcp_context_t *ctx, mcp_server_t *srv,
                                           mcp_json_value_t *meta_json) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *clone = (meta_json != NULL) ? mcp_json_clone(ctx, meta_json) : NULL;
    if (meta_json != NULL && clone == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_json_destroy(ctx, srv->response_meta);
    srv->response_meta = clone;
    return MCP_OK;
}
```

- [ ] **Step 4: Build verify 0 errors**

```bash
cmake --build build -j4
```
Expected: 0 errors.

- [ ] **Step 5: Commit**

```bash
git add src/server/internals.h include/mcpkit/server/server.h src/server/server.c
git commit -m "feat(server): add mcp_server_set_list_cache and mcp_server_set_response_meta"
```

---

## Chunk 2: dispatcher routing + server/discover + decorate_result

### Task 4: Add `server/discover` to method table + L3 validation

**Files:**
- Modify: `src/method_table.h`
- Modify: `src/protocol/validate.c`

- [ ] **Step 1: Update `method_table.h`**

Add `"server/discover"` as the 16th entry in `k_mcp_server_methods` (before the closing `};`):

```c
static const char *const k_mcp_server_methods[] = {
    "initialize",
    "ping",
    "tools/list",
    "tools/call",
    "resources/list",
    "resources/read",
    "prompts/list",
    "prompts/get",
    "completion/list",
    "completion/complete",
    "notifications/initialized",
    "logging/setLevel",
    "resources/subscribe",
    "resources/unsubscribe",
    "resources/templates/list",
    "server/discover",
};

#define MCP_SERVER_METHOD_COUNT 16
```

Update the `_Static_assert` to `== 16`.

- [ ] **Step 2: Add L3 no-param rule in `validate.c`**

Find the L3 section where `resources/templates/list` is handled (it has no required params). Add a parallel entry for `server/discover` immediately after it:

```c
if (strcmp(method, "server/discover") == 0) {
    // No required params; params may be absent.
    return MCP_OK;
}
```

(Adapt to the actual code structure — check how `resources/templates/list` is handled and mirror it exactly.)

- [ ] **Step 3: Build verify 0 errors**

```bash
cmake --build build -j4
```

- [ ] **Step 4: Commit**

```bash
git add src/method_table.h src/protocol/validate.c
git commit -m "feat(protocol): register server/discover in method table and L3 validation"
```

---

### Task 5: Add `decorate_result` helper + `route_server_discover` + wire into all routes

**Files:**
- Modify: `src/server/dispatcher.c`

This is the largest task. The pattern:

1. Add `#include "mcpkit/protocol/message.h"` if not already present (it is, for `mcp_message_*` accessors).
2. Add `#include "mcpkit/protocol/initialize.h"` for `MCP_PROTOCOL_VERSION_LATEST`.

- [ ] **Step 1: Add `decorate_result` static helper**

Insert before `route_request` (after `route_advanced`):

```c
// Internal helper: decorates a result object before mcp_response_ok_new.
// - is_list=true:  inject resultType + ttlMs + cacheScope + _meta
// - is_list=false: inject resultType only
// NOMEM: caller must destroy result and return NULL from the route.
static mcp_status_t decorate_result(mcp_context_t *ctx, mcp_server_t *srv,
                                     mcp_json_value_t *result, bool is_list) {
    if (mcp_result_inject_result_type(ctx, result) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    if (!is_list) {
        return MCP_OK;
    }
    if (srv->list_ttl_ms != 0) {
        mcp_json_value_t *ttl = mcp_json_number_new(ctx, (double)srv->list_ttl_ms);
        if (ttl == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "ttlMs", ttl) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }
    if (srv->list_cache_scope != NULL) {
        mcp_json_value_t *scope = mcp_json_string_new(ctx, srv->list_cache_scope);
        if (scope == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "cacheScope", scope) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }
    if (srv->response_meta != NULL) {
        mcp_json_value_t *meta_clone = mcp_json_clone(ctx, srv->response_meta);
        if (meta_clone == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_result_inject_meta(ctx, result, meta_clone) != MCP_OK) {
            // meta_clone destroyed by set_take on failure (caller retains on ERR per contract);
            // actually set_take destroys meta on failure, so just propagate NOMEM.
            return MCP_ERR_NOMEM;
        }
    }
    return MCP_OK;
}
```

> **Note on `mcp_result_inject_meta` failure**: per the §1 contract, `mcp_result_inject_meta`
> calls `mcp_json_object_set_take` internally; on set_take failure the value is destroyed by
> set_take itself, so `meta_clone` is already freed. Do NOT add a second `mcp_json_destroy`.

- [ ] **Step 2: Add `route_server_discover`**

Insert before `route_request`:

```c
static mcp_message_t *route_server_discover(mcp_context_t *ctx, mcp_server_t *srv,
                                             const mcp_message_t *req) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }
    // serverName
    mcp_json_value_t *v = mcp_json_string_new(ctx, srv->name);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "serverName", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    // serverVersion
    v = mcp_json_string_new(ctx, srv->version);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "serverVersion", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    // protocolVersion (static string, no allocation for the string itself)
    v = mcp_json_string_new(ctx, MCP_PROTOCOL_VERSION_LATEST);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "protocolVersion", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    // counts
    v = mcp_json_number_new(ctx, (double)srv->n_tools);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "toolsCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    v = mcp_json_number_new(ctx, (double)srv->n_resources);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "resourcesCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    v = mcp_json_number_new(ctx, (double)srv->n_prompts);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "promptsCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    // cache params (omit when 0/NULL)
    if (srv->list_ttl_ms != 0) {
        v = mcp_json_number_new(ctx, (double)srv->list_ttl_ms);
        if (v == NULL || mcp_json_object_set_take(ctx, result, "listTtlMs", v) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if (srv->list_cache_scope != NULL) {
        v = mcp_json_string_new(ctx, srv->list_cache_scope);
        if (v == NULL || mcp_json_object_set_take(ctx, result, "listCacheScope", v) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    // capability flags
    v = mcp_json_bool_new(ctx, true);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "supportsStateless", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    v = mcp_json_bool_new(ctx, true);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "supportsMeta", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    // decorate (resultType + _meta if configured)
    if (decorate_result(ctx, srv, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}
```

> **P3 comment note**: The per-step `mcp_json_object_set_take` pattern above is the same
> ownership discipline used throughout dispatcher.c. Each step destroys `result` on NOMEM
> because set_take has already consumed `v` (success or fail). This is deliberate and matches
> the 5d23b51 refactor pattern.

- [ ] **Step 3: Wire `server/discover` into `route_request`**

In `route_request`, after the `route_advanced` call (which handles `logging/setLevel`,
`resources/subscribe`, `resources/unsubscribe`, `resources/templates/list`), add:

```c
if (strcmp(method, "server/discover") == 0) {
    return route_server_discover(ctx, srv, req);
}
```

Place it BEFORE the `dlogf_srv` unknown_method fallback.

- [ ] **Step 4: Update `route_initialize` to clone `_meta` into `s->client_meta`**

In `route_initialize`, after the `clientInfo` handling block and before the
`mcp_initialize_result_new` call, add:

```c
// Clone the client's _meta into the session (best-effort; NOMEM is not fatal).
const mcp_json_value_t *req_meta = mcp_message_meta(ctx, req);
if (req_meta != NULL) {
    mcp_json_value_t *meta_clone = mcp_json_clone(ctx, req_meta);
    if (meta_clone != NULL) {
        mcp_json_destroy(ctx, s->client_meta);
        s->client_meta = meta_clone;
    }
    // NOMEM: keep existing client_meta (or NULL); do not abort initialize.
}
```

- [ ] **Step 5: Add `decorate_result` calls to all 8 success routes**

For each of the following routes, find the last `mcp_json_object_set_take` call that
builds the result object, then insert `decorate_result` before the `mcp_response_ok_new` call.
On `decorate_result` failure: destroy `result` and return NULL (matching the existing NOMEM pattern).

**route_tools_list** (after the `set_next_cursor` / visibility loop, before `mcp_response_ok_new`):

```c
if (decorate_result(ctx, srv, result, true) != MCP_OK) {
    mcp_json_destroy(ctx, result);
    return NULL;
}
```

**route_resources_list**: same pattern, `is_list=true`.

**route_prompts_list**: same pattern, `is_list=true`.

**route_completion_list**: same pattern, `is_list=true`.

**route_ping** (the inline empty-object route at `k_mcp_server_methods[1]`):
Replace the inline body with:

```c
if (strcmp(method, k_mcp_server_methods[1]) == 0) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }
    if (decorate_result(ctx, srv, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}
```

**route_tools_call** (before `mcp_response_ok_new` at line ~363): add `decorate_result(ctx, srv, result, false)` with the same NOMEM guard.

**route_resources_read** (before `mcp_response_ok_new` at line ~445): same, `is_list=false`.

**route_prompts_get** (before `mcp_response_ok_new` at line ~532): same, `is_list=false`.

**route_completion_complete**: There are two success paths (matched provider at line ~620, no-match at line ~639). Add `decorate_result(ctx, srv, result, false)` before each `mcp_response_ok_new` call in both paths.

**route_advanced** (the three advanced routes: `logging/setLevel`, `resources/subscribe`, `resources/unsubscribe`, `resources/templates/list`): add `decorate_result(ctx, srv, result, false)` before each `mcp_response_ok_new`. These are NOT list-type, so `is_list=false`.

> **Note**: `route_advanced` returns early with `NULL` for methods it doesn't handle.
> Only add `decorate_result` to the specific `return` sites that build a result object.

- [ ] **Step 6: Build verify 0 errors**

```bash
cmake --build build -j4
```
Expected: 0 errors, 0 warnings.

- [ ] **Step 7: Run ctest to confirm no regressions**

```bash
ctest --test-dir build --output-on-failure -E "test_socket_serve"
```
Expected: all existing tests pass (46 passing, test_socket_serve excluded).

- [ ] **Step 8: Commit**

```bash
git add src/server/dispatcher.c
git commit -m "feat(server): add decorate_result, route_server_discover, and _meta session storage"
```

---

## Chunk 3: TDD test + docs + changelog + gates

### Task 6: Write `test_protocol_statelessness.c` (TDD — write failing test first, then verify it passes)

**Files:**
- Create: `tests/unit/test_protocol_statelessness.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test file**

```c
/**
 * @file test_protocol_statelessness.c
 *
 * TDD test for _meta bidirectional support, resultType injection,
 * host-configurable ttlMs/cacheScope, and server/discover.
 */
#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"
#include "mcpkit/server/dispatcher.h"

static mcp_json_value_t *echo_handler(mcp_context_t *ctx, mcp_session_t *s,
                                       const mcp_json_value_t *args, void *ud,
                                       mcp_json_value_t **out) {
    (void)s; (void)ud;
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    if (content == NULL) {
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *block = mcp_json_object_new(ctx);
    mcp_json_value_t *type = mcp_json_string_new(ctx, "text");
    mcp_json_value_t *text = mcp_json_string_new(ctx, "ok");
    if (block == NULL || type == NULL || text == NULL) {
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, block, "type", type) != MCP_OK ||
        mcp_json_object_set_take(ctx, block, "text", text) != MCP_OK) {
        mcp_json_destroy(ctx, block);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_array_append(ctx, content, block) != MCP_OK) {
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *out = result;
    return MCP_OK;
}

static mcp_json_value_t *make_echo_tool_schema(mcp_context_t *ctx) {
    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_json_value_t *type_str = mcp_json_string_new(ctx, "object");
    if (schema == NULL || type_str == NULL) {
        mcp_json_destroy(ctx, schema);
        mcp_json_destroy(ctx, type_str);
        return NULL;
    }
    mcp_json_object_set_take(ctx, schema, "type", type_str);
    return schema;
}

// Helper: initialize a server with one tool + one session, return server/session/ctx.
// Caller must destroy all three.
static int setup_ctx_server(mcp_context_t **ctx_out, mcp_server_t **srv_out,
                             mcp_session_t *sess_out_ptr, mcp_session_t **sess_out) {
    *ctx_out = mcp_context_create(NULL);
    if (*ctx_out == NULL) return 1;
    mcp_context_t *ctx = *ctx_out;

    *srv_out = mcp_server_create(ctx, "test-srv", "0.1.0");
    if (*srv_out == NULL) return 2;
    mcp_server_t *srv = *srv_out;

    mcp_json_value_t *schema = make_echo_tool_schema(ctx);
    mcp_tool_t *tool = mcp_tool_new(ctx, "echo", "echo tool", schema, echo_handler, NULL);
    if (tool == NULL) return 3;
    mcp_server_add_tool(ctx, srv, tool);

    *sess_out = mcp_server_create_session(ctx, srv);
    if (*sess_out == NULL) return 4;
    mcp_session_t *sess = *sess_out;
    mcp_session_set_apps_host(ctx, sess, false);
    // Mark initialized manually (bypass the initialize RPC for test simplicity)
    // The dispatcher checks session->initialized; we use the public setter if available,
    // otherwise we call mcp_server_dispatch with an initialize first.
    return 0;
}

// Helper: build a request message for a given method with optional params.
static mcp_message_t *make_request(mcp_context_t *ctx, const char *method,
                                    mcp_json_value_t *params) {
    return mcp_request_new_string_id(ctx, "1", method, params);
}

int main(void) {
    // ── Case 1: mcp_message_meta returns NULL when no _meta ──────────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_message_t *msg = mcp_request_new_string_id(ctx, "1", "ping", params);
        CHECK(msg != NULL);
        CHECK(mcp_message_meta(ctx, msg) == NULL);
        mcp_message_destroy(ctx, msg);
        mcp_context_destroy(ctx);
    }

    // ── Case 2: mcp_message_meta returns the _meta object when present ───────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_value_t *meta = mcp_json_object_new(ctx);
        mcp_json_value_t *ver = mcp_json_string_new(ctx, "2025-06-18");
        mcp_json_object_set_take(ctx, meta, "protocolVersion", ver);
        mcp_json_object_set_take(ctx, params, "_meta", meta);
        mcp_message_t *msg = mcp_request_new_string_id(ctx, "1", "ping", params);
        CHECK(msg != NULL);
        const mcp_json_value_t *got = mcp_message_meta(ctx, msg);
        // _meta lives at the TOP LEVEL of the JSON-RPC envelope, not inside params.
        // mcp_message_meta reads the DOM root, so build the msg with _meta at root level.
        // For this test, use mcp_message_parse to inject _meta at root level:
        char json[] = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"ping\",\"\
                       _meta\":{\"protocolVersion\":\"2025-06-18\"}}";
        mcp_message_t *msg2 = mcp_message_parse(ctx, json, strlen(json));
        CHECK(msg2 != NULL);
        CHECK(mcp_message_meta(ctx, msg2) != NULL);
        mcp_message_destroy(ctx, msg2);
        mcp_message_destroy(ctx, msg);
        mcp_context_destroy(ctx);
    }

    // ── Case 3: mcp_result_inject_result_type adds resultType="complete" ──────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        CHECK(mcp_result_inject_result_type(ctx, result) == MCP_OK);
        const mcp_json_value_t *rt = mcp_json_object_get(ctx, result, "resultType");
        CHECK(rt != NULL);
        const char *rt_str = NULL;
        CHECK(mcp_json_string_value(ctx, rt, &rt_str) == MCP_OK);
        CHECK(rt_str != NULL && strcmp(rt_str, "complete") == 0);
        mcp_json_destroy(ctx, result);
        mcp_context_destroy(ctx);
    }

    // ── Case 4: mcp_result_inject_meta adds _meta; caller retains on failure ─────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        mcp_json_value_t *meta = mcp_json_object_new(ctx);
        mcp_json_value_t *k = mcp_json_string_new(ctx, "hello");
        mcp_json_object_set_take(ctx, meta, "key", k);
        CHECK(mcp_result_inject_meta(ctx, result, meta) == MCP_OK);
        CHECK(mcp_json_object_get(ctx, result, "_meta") != NULL);
        mcp_json_destroy(ctx, result);

        // Failure path: NULL result
        mcp_json_value_t *meta2 = mcp_json_object_new(ctx);
        CHECK(mcp_result_inject_meta(ctx, NULL, meta2) == MCP_ERR_INVALID_ARGUMENT);
        CHECK(meta2 != NULL); // caller still owns it
        mcp_json_destroy(ctx, meta2);
        mcp_context_destroy(ctx);
    }

    // ── Case 5: server/discover returns capability fields ─────────────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_server_t *srv = mcp_server_create(ctx, "disc-srv", "9.9.9");
        mcp_session_t *sess = mcp_server_create_session(ctx, srv);
        // Initialize the session so dispatch gate passes
        mcp_json_value_t *init_params = mcp_initialize_params_new(ctx, "t", "1");
        mcp_message_t *init_req = mcp_request_new_string_id(ctx, "init", "initialize", init_params);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_req);
        mcp_message_destroy(ctx, init_resp);
        // Notify initialized
        mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
        CHECK(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
        mcp_message_destroy(ctx, init_notif);

        // Now call server/discover
        mcp_message_t *disc_req = mcp_request_new_string_id(ctx, "disc", "server/discover", NULL);
        mcp_message_t *disc_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, disc_req, &disc_resp) == MCP_OK);
        CHECK(disc_resp != NULL);
        const mcp_json_value_t *result = mcp_message_result(ctx, disc_resp);
        CHECK(result != NULL);
        CHECK(mcp_json_object_get(ctx, result, "serverName") != NULL);
        CHECK(mcp_json_object_get(ctx, result, "serverVersion") != NULL);
        CHECK(mcp_json_object_get(ctx, result, "toolsCount") != NULL);
        CHECK(mcp_json_object_get(ctx, result, "supportsStateless") != NULL);
        CHECK(mcp_json_object_get(ctx, result, "resultType") != NULL); // resultType injected
        mcp_message_destroy(ctx, disc_resp);
        mcp_message_destroy(ctx, disc_req);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }

    // ── Case 6: list_cache config appears in tools/list response ───────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_server_t *srv = mcp_server_create(ctx, "cache-srv", "0.1.0");
        mcp_session_t *sess = mcp_server_create_session(ctx, srv);
        mcp_json_value_t *init_params = mcp_initialize_params_new(ctx, "t", "1");
        mcp_message_t *init_req = mcp_request_new_string_id(ctx, "i", "initialize", init_params);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_req);
        mcp_message_destroy(ctx, init_resp);
        mcp_message_t *notif = mcp_initialized_notification_new(ctx);
        CHECK(mcp_server_notify(ctx, srv, sess, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        CHECK(mcp_server_set_list_cache(ctx, srv, 30000, "server") == MCP_OK);

        // Add a tool so tools/list has content
        mcp_json_value_t *schema = make_echo_tool_schema(ctx);
        mcp_tool_t *tool = mcp_tool_new(ctx, "echo", "e", schema, echo_handler, NULL);
        CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

        mcp_message_t *list_req = mcp_request_new_string_id(ctx, "l", "tools/list", NULL);
        mcp_message_t *list_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, list_req, &list_resp) == MCP_OK);
        CHECK(list_resp != NULL);
        const mcp_json_value_t *result = mcp_message_result(ctx, list_resp);
        CHECK(result != NULL);
        CHECK(mcp_json_object_get(ctx, result, "ttlMs") != NULL);
        CHECK(mcp_json_object_get(ctx, result, "cacheScope") != NULL);
        CHECK(mcp_json_object_get(ctx, result, "resultType") != NULL);
        mcp_message_destroy(ctx, list_resp);
        mcp_message_destroy(ctx, list_req);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }

    // ── Case 7: no list_cache config → ttlMs/cacheScope absent ────────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_server_t *srv = mcp_server_create(ctx, "no-cache-srv", "0.1.0");
        mcp_session_t *sess = mcp_server_create_session(ctx, srv);
        mcp_json_value_t *init_params = mcp_initialize_params_new(ctx, "t", "1");
        mcp_message_t *init_req = mcp_request_new_string_id(ctx, "i", "initialize", init_params);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_req);
        mcp_message_destroy(ctx, init_resp);
        mcp_message_t *notif = mcp_initialized_notification_new(ctx);
        CHECK(mcp_server_notify(ctx, srv, sess, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        // No set_list_cache call → ttlMs must be absent
        mcp_json_value_t *schema = make_echo_tool_schema(ctx);
        mcp_tool_t *tool = mcp_tool_new(ctx, "echo", "e", schema, echo_handler, NULL);
        CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

        mcp_message_t *list_req = mcp_request_new_string_id(ctx, "l", "tools/list", NULL);
        mcp_message_t *list_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, list_req, &list_resp) == MCP_OK);
        const mcp_json_value_t *result = mcp_message_result(ctx, list_resp);
        CHECK(result != NULL);
        CHECK(mcp_json_object_get(ctx, result, "ttlMs") == NULL);   // absent
        CHECK(mcp_json_object_get(ctx, result, "resultType") != NULL); // still present
        mcp_message_destroy(ctx, list_resp);
        mcp_message_destroy(ctx, list_req);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }

    // ── Case 8: response_meta injected into list response ─────────────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_server_t *srv = mcp_server_create(ctx, "meta-srv", "0.1.0");
        mcp_session_t *sess = mcp_server_create_session(ctx, srv);
        mcp_json_value_t *init_params = mcp_initialize_params_new(ctx, "t", "1");
        mcp_message_t *init_req = mcp_request_new_string_id(ctx, "i", "initialize", init_params);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_req);
        mcp_message_destroy(ctx, init_resp);
        mcp_message_t *notif = mcp_initialized_notification_new(ctx);
        CHECK(mcp_server_notify(ctx, srv, sess, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        mcp_json_value_t *meta = mcp_json_object_new(ctx);
        mcp_json_value_t *trace = mcp_json_string_new(ctx, "test-trace");
        mcp_json_object_set_take(ctx, meta, "traceId", trace);
        CHECK(mcp_server_set_response_meta(ctx, srv, meta) == MCP_OK);

        mcp_json_value_t *schema = make_echo_tool_schema(ctx);
        mcp_tool_t *tool = mcp_tool_new(ctx, "echo", "e", schema, echo_handler, NULL);
        CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

        mcp_message_t *list_req = mcp_request_new_string_id(ctx, "l", "tools/list", NULL);
        mcp_message_t *list_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, list_req, &list_resp) == MCP_OK);
        const mcp_json_value_t *result = mcp_message_result(ctx, list_resp);
        CHECK(result != NULL);
        CHECK(mcp_json_object_get(ctx, result, "_meta") != NULL);
        mcp_message_destroy(ctx, list_resp);
        mcp_message_destroy(ctx, list_req);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }

    // ── Case 9: session stores client_meta from initialize _meta ──────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_server_t *srv = mcp_server_create(ctx, "smeta-srv", "0.1.0");
        mcp_session_t *sess = mcp_server_create_session(ctx, srv);

        // Build an initialize request that carries _meta at root level via parse
        const char *json =
            "{\"jsonrpc\":\"2.0\",\"id\":\"init1\",\"method\":\"initialize\","
            "\"params\":{\"protocolVersion\":\"2025-06-18\"},"
            "\"_meta\":{\"clientId\":\"client-abc\",\"capabilities\":{\"tools\":true}}}";
        mcp_message_t *init_req = mcp_message_parse(ctx, json, strlen(json));
        CHECK(init_req != NULL);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_req);
        mcp_message_destroy(ctx, init_resp);
        mcp_message_t *notif = mcp_initialized_notification_new(ctx);
        CHECK(mcp_server_notify(ctx, srv, sess, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        const mcp_json_value_t *client_meta = mcp_session_client_meta(ctx, sess);
        CHECK(client_meta != NULL);
        const mcp_json_value_t *cid = mcp_json_object_get(ctx, client_meta, "clientId");
        CHECK(cid != NULL);
        const char *cid_str = NULL;
        CHECK(mcp_json_string_value(ctx, cid, &cid_str) == MCP_OK);
        CHECK(cid_str != NULL && strcmp(cid_str, "client-abc") == 0);

        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }

    // ── Case 10: ping response has resultType but no ttlMs ─────────────────────────
    {
        mcp_context_t *ctx = mcp_context_create(NULL);
        mcp_server_t *srv = mcp_server_create(ctx, "ping-srv", "0.1.0");
        mcp_session_t *sess = mcp_server_create_session(ctx, srv);
        mcp_json_value_t *init_params = mcp_initialize_params_new(ctx, "t", "1");
        mcp_message_t *init_req = mcp_request_new_string_id(ctx, "i", "initialize", init_params);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_req);
        mcp_message_destroy(ctx, init_resp);
        mcp_message_t *notif = mcp_initialized_notification_new(ctx);
        CHECK(mcp_server_notify(ctx, srv, sess, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        CHECK(mcp_server_set_list_cache(ctx, srv, 5000, "server") == MCP_OK);

        mcp_message_t *ping_req = mcp_request_new_string_id(ctx, "p", "ping", NULL);
        mcp_message_t *ping_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, sess, ping_req, &ping_resp) == MCP_OK);
        const mcp_json_value_t *result = mcp_message_result(ctx, ping_resp);
        CHECK(result != NULL);
        CHECK(mcp_json_object_get(ctx, result, "resultType") != NULL); // present
        CHECK(mcp_json_object_get(ctx, result, "ttlMs") == NULL);      // absent (not a list route)
        mcp_message_destroy(ctx, ping_resp);
        mcp_message_destroy(ctx, ping_req);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }

    return 0;
}
```

- [ ] **Step 2: Register test in `tests/CMakeLists.txt`**

Add (unconditional, same as other protocol tests):

```cmake
add_executable(test_protocol_statelessness unit/test_protocol_statelessness.c)
target_link_libraries(test_protocol_statelessness mcpkit_core)
add_test(NAME test_protocol_statelessness COMMAND test_protocol_statelessness)
```

- [ ] **Step 3: Build and run the test (TDD — it may fail first if implementation is incomplete)**

```bash
cmake --build build -j4 && ctest --test-dir build -R test_protocol_statelessness --output-on-failure
```

If it fails, fix the implementation until it passes. Then run the full suite:

```bash
ctest --test-dir build --output-on-failure -E "test_socket_serve"
```
Expected: all 47 tests pass (46 existing + 1 new), test_socket_serve excluded.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_protocol_statelessness.c tests/CMakeLists.txt
git commit -m "test(protocol): prove _meta bidirectional, resultType, ttlMs/cacheScope, and server/discover"
```

---

### Task 7: Docs + CHANGELOG + doxygen + triple gates + cleanup

**Files:**
- Modify: `docs/module-reference.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update `docs/module-reference.md`**

Add to the `message.h` section:
```
- `mcp_message_meta(ctx, msg)` — returns top-level `_meta` object (borrowed), or NULL
- `mcp_result_inject_result_type(ctx, result)` — adds `"resultType":"complete"` to a result object
- `mcp_result_inject_meta(ctx, result, meta)` — injects caller-owned `_meta` into result (set_take semantics)
```

Add to the `session.h` section:
```
- `mcp_session_client_meta(ctx, sess)` — returns the client `_meta` stored from initialize (borrowed), or NULL
```

Add to the `server.h` section:
```
- `mcp_server_set_list_cache(ctx, srv, ttl_ms, scope_or_null)` — configure ttlMs/cacheScope for list responses
- `mcp_server_set_response_meta(ctx, srv, meta_json)` — configure _meta injected into list/discover responses
```

Add to the `dispatcher.h` section:
```
- `server/discover` — capability discovery RPC; returns serverName, serverVersion, protocolVersion,
  toolsCount/resourcesCount/promptsCount, listTtlMs/listCacheScope (when configured),
  supportsStateless:true, supportsMeta:true, resultType:"complete"
```

- [ ] **Step 2: Update `CHANGELOG.md` [Unreleased] Added**

Add one bullet:
```
- Protocol statelessness: `_meta` bidirectional support (`mcp_message_meta`, `mcp_result_inject_meta`,
  `mcp_session_client_meta`), `resultType="complete"` on all success responses, host-configurable
  `ttlMs`/`cacheScope` on list responses (`mcp_server_set_list_cache`), `_meta` injection on
  list/discover responses (`mcp_server_set_response_meta`), and `server/discover` capability
  endpoint.
```

- [ ] **Step 3: Doxygen 0-warning verify**

```bash
doxygen Doxyfile 2>&1 | grep -c "warning:" ; rm -rf docs/api
```
Expected: 0 warnings.

- [ ] **Step 4: Triple gates**

```bash
# gcc
cmake -S . -B build-gcc -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && \
cmake --build build-gcc -j4 && \
ctest --test-dir build-gcc --output-on-failure -E "test_socket_serve"

# clang
cmake -S . -B build-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
    -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && \
cmake --build build-clang -j4 && \
ctest --test-dir build-clang --output-on-failure -E "test_socket_serve"

# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON \
    -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && \
cmake --build build-asan -j4 && \
ctest --test-dir build-asan --output-on-failure -E "test_socket_serve" && \
ldd build-asan/tools/mcpkit-cli | grep -E "libasan|libubsan"
```
Expected: all three 47/47 (excl test_socket_serve); ldd shows libasan.so.8 + libubsan.so.1.

- [ ] **Step 5: Remove all build dirs + commit**

```bash
rm -rf build-gcc build-clang build-asan
git add -A
git commit -m "docs: cover protocol statelessness and server/discover in module reference and changelog"
```

- [ ] **Step 6: Verify tree clean**

```bash
git status --porcelain
```
Expected: no output (clean tree).
