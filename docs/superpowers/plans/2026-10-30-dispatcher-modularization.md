# 服务端分发引擎 `dispatcher.c` 模块化解耦实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple the 1648-line God-file `src/server/dispatcher.c` into three clean, single-responsibility modules: `queue.c` (message queue), `routes.c` (concrete method route handlers), and a streamlined `dispatcher.c` (core pipeline orchestration and notification sink), maintaining 100% public API compatibility and zero compiler warnings under `-Wall -Wextra`.

**Architecture:**
- **Task 1: Internal Seams & Method Table Hygiene** — Update `src/server/internals.h` to declare cross-file route entrypoint `mcp_server_route_request` and `dlogf_srv`; update `src/server/dispatcher.c` to drop `static` on `dlogf_srv` to match external linkage; annotate method tables in `src/method_table.h` with `__attribute__((unused))` to prevent `-Wunused-const-variable` warnings when split.
- **Task 2: Extract Session Queue Subsystem (`src/server/queue.c`)** — Move `struct mcp_queue`, `queue_grow`, `mcp_queue_*`, and `mcp_server_process_one` verbatim from `dispatcher.c` into a dedicated `src/server/queue.c` module; remove `struct mcp_queue` from `internals.h`; register `src/server/queue.c` in `CMakeLists.txt`.
- **Task 3: Extract Method Route Handlers Subsystem (`src/server/routes.c`)** — Move all 22 method route handlers and decorators (`decorate_result`, `tool_error_result`, `find_*`, `page_offset`, etc.) into `routes.c` under `mcp_server_route_request`; register `src/server/routes.c` in `CMakeLists.txt`.
- **Task 4: Streamline Core Dispatcher Engine (`src/server/dispatcher.c`)** — Refactor `dispatcher.c` into a concise pipeline orchestrator (~230 lines) handling L1/L2/L3 validation, tracing, session status/permissions, error accounting, and notification consumption.
- **Task 5: Full Regression Testing & Zero-Warning Gate** — Run the full test suite (all 69+ tests) under clean build with `-Wall -Wextra` to verify 100% test pass rate and zero warnings.

**Tech Stack:** C23 (`-std=c2x`), libc, CMake / CTest.

---

## Chunk 1: Header Seams and Queue Subsystem Extraction

### Task 1: Internal Seams & Method Table Hygiene

**Files:**
- Modify: `src/server/internals.h:145-156`
- Modify: `src/server/dispatcher.c:45-50`
- Modify: `src/method_table.h:1-15, 45-50`

- [ ] **Step 1: Declare cross-module functions in `src/server/internals.h`**

Add cross-module function declarations to `src/server/internals.h`:
```c
/* ------------------------------------------------------------------ */
/* Server Route Handlers Seam (routes.c <-> dispatcher.c)             */
/* ------------------------------------------------------------------ */

void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
               const char *fmt, ...);

mcp_message_t *mcp_server_route_request(mcp_context_t *ctx, mcp_server_t *srv,
                                        mcp_session_t *s, const mcp_message_t *req,
                                        const char *method);
```
*(Note: `struct mcp_queue` is retained in `internals.h` during Task 1 to preserve compilation of `dispatcher.c`, and will be removed in Task 2 when `queue.c` is introduced).*

- [ ] **Step 2: Update `dlogf_srv` linkage in `src/server/dispatcher.c`**

In `src/server/dispatcher.c:45`:
Change:
```c
static void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
                       const char *fmt, ...) {
```
To external linkage matching `internals.h`:
```c
void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
               const char *fmt, ...) {
```

- [ ] **Step 3: Update `src/method_table.h` with unused attribute annotations**

In `src/method_table.h`:
Add `__attribute__((unused))` to both `k_mcp_server_methods` and `k_mcp_server_notifications`:
```c
static const char *const k_mcp_server_methods[] __attribute__((unused)) = {
...
};

static const char *const k_mcp_server_notifications[] __attribute__((unused)) = {
...
};
```
And update the header comment to note that `routes.c` and `dispatcher.c` both consume this header.

- [ ] **Step 4: Verify build compiles cleanly**

Run: `cmake --build build -j`
Expected: Build succeeds without errors or warnings.

- [ ] **Step 5: Commit**

```bash
git add src/server/internals.h src/server/dispatcher.c src/method_table.h
git commit -m "refactor(server): declare internal route handler seam and annotate method tables"
```

---

### Task 2: Extract Session Queue Subsystem (`src/server/queue.c`)

**Files:**
- Create: `src/server/queue.c`
- Modify: `src/server/internals.h:5-15, 145-156`
- Modify: `src/server/dispatcher.c:1532-1648`
- Modify: `CMakeLists.txt:55-65`

- [ ] **Step 1: Create `src/server/queue.c` using verbatim queue logic and shared allocators**

Create `src/server/queue.c`:
```c
/**
 * @file queue.c
 * @brief Per-session FIFO message queue and single-step processing.
 */
#define _DEFAULT_SOURCE

#include <string.h>

#include "mcpkit/server/dispatcher.h"
#include "mcpkit/core/context.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "internals.h"

struct mcp_queue {
    mcp_session_t **sessions;
    mcp_message_t **msgs;
    size_t head;
    size_t len;
    size_t cap;
};

mcp_queue_t *mcp_queue_create(mcp_context_t *ctx) {
    mcp_queue_t *q = srv_malloc(ctx, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    memset(q, 0, sizeof(*q));
    return q;
}

void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q) {
    if (q == NULL) {
        return;
    }
    for (size_t i = 0; i < q->len; i++) {
        size_t idx = (q->head + i) % q->cap;
        mcp_message_destroy(ctx, q->msgs[idx]);
    }
    srv_free(ctx, q->sessions);
    srv_free(ctx, q->msgs);
    srv_free(ctx, q);
}

static mcp_status_t queue_grow(mcp_context_t *ctx, mcp_queue_t *q) {
    if (q->len < q->cap && q->head == 0) {
        return MCP_OK;
    }
    if (q->head > 0) {
        for (size_t i = 0; i < q->len; i++) {
            size_t src = (q->head + i) % q->cap;
            if (src != i) {
                q->sessions[i] = q->sessions[src];
                q->msgs[i] = q->msgs[src];
            }
        }
        q->head = 0;
        if (q->len < q->cap) {
            return MCP_OK;
        }
    }
    size_t ncap = q->cap == 0 ? 8 : q->cap * 2;
    mcp_message_t **om = q->msgs;
    mcp_message_t **nm = srv_realloc(ctx, om, ncap * sizeof(*nm));
    if (nm == NULL) {
        return MCP_ERR_NOMEM;
    }
    q->msgs = nm;
    struct mcp_session **os = q->sessions;
    struct mcp_session **ns = srv_realloc(ctx, os, ncap * sizeof(*ns));
    if (ns == NULL) {
        return MCP_ERR_NOMEM;
    }
    q->sessions = ns;
    q->cap = ncap;
    return MCP_OK;
}

mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg) {
    if (q == NULL || session == NULL || msg == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (q->len >= MCP_QUEUE_MAX_LEN) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = queue_grow(ctx, q);
    if (st != MCP_OK) {
        return st;
    }
    size_t idx = (q->head + q->len) % q->cap;
    q->sessions[idx] = session;
    q->msgs[idx] = msg;
    q->len++;
    return MCP_OK;
}

mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out) {
    (void)ctx;
    if (q == NULL || session_out == NULL || msg_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (q->len == 0) {
        return MCP_ERR_NOT_FOUND;
    }
    *session_out = q->sessions[q->head];
    *msg_out = q->msgs[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return MCP_OK;
}

size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q) {
    (void)ctx;
    return q != NULL ? q->len : 0;
}

mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *srv, mcp_queue_t *q,
                                    mcp_message_t **resp_out) {
    if (srv == NULL || q == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;
    mcp_session_t *session = NULL;
    mcp_message_t *msg = NULL;
    mcp_status_t st = mcp_queue_pop(ctx, q, &session, &msg);
    if (st != MCP_OK) {
        return st;
    }
    mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);
    if (kind == MCP_MSG_NOTIFICATION) {
        st = mcp_server_notify(ctx, srv, session, msg);
        mcp_message_destroy(ctx, msg);
        return st;
    }
    if (kind == MCP_MSG_REQUEST) {
        st = mcp_server_dispatch(ctx, srv, session, msg, resp_out);
        mcp_message_destroy(ctx, msg);
        return st;
    }
    mcp_message_destroy(ctx, msg);
    return MCP_ERR_PROTOCOL;
}
```

- [ ] **Step 2: Remove `struct mcp_queue` from `src/server/internals.h` and update docstring**

In `src/server/internals.h`:
Remove `struct mcp_queue`:
```c
struct mcp_queue {
    mcp_session_t **sessions;
    mcp_message_t **msgs;
    size_t head;
    size_t len;
    size_t cap;
};
```
And update line 8 docstring from `mcp_tool/resource/prompt/session/server/queue` to `mcp_tool/resource/prompt/session/server`.

- [ ] **Step 3: Remove queue functions from `src/server/dispatcher.c`**

In `src/server/dispatcher.c`:
Remove the bottom functions (lines 1532–1648):
`mcp_queue_create`, `mcp_queue_destroy`, `queue_grow`, `mcp_queue_push`, `mcp_queue_pop`, `mcp_queue_size`, `mcp_server_process_one`.

- [ ] **Step 4: Add `src/server/queue.c` to `CMakeLists.txt`**

In `CMakeLists.txt`:
```cmake
set(MCPKIT_CORE_SOURCES
  ...
  src/server/server.c
  src/server/registry.c
  src/server/session.c
  src/server/dispatcher.c
  src/server/queue.c
  ...
)
```

- [ ] **Step 5: Build and test**

Run: `cmake --build build -j && ctest --test-dir build -R "test_dispatcher" --output-on-failure`
Expected: Build succeeds without warnings, `test_dispatcher` passes 100%.

- [ ] **Step 6: Commit**

```bash
git add src/server/queue.c src/server/internals.h src/server/dispatcher.c CMakeLists.txt
git commit -m "refactor(server): extract queue subsystem into src/server/queue.c"
```

---

## Chunk 2: Extract Routes Subsystem & Streamline Dispatcher Engine

### Task 3: Extract Method Route Handlers Subsystem (`src/server/routes.c`)

**Files:**
- Create: `src/server/routes.c`
- Modify: `CMakeLists.txt:55-65`

- [ ] **Step 1: Create `src/server/routes.c`**

Create `src/server/routes.c` containing:
- Inclusions:
  ```c
  #define _DEFAULT_SOURCE
  #include <stdio.h>
  #include <string.h>

  #include "internals.h"
  #include "../method_table.h"
  #include "mcpkit/core/context.h"
  #include "mcpkit/json/array.h"
  #include "mcpkit/json/object.h"
  #include "mcpkit/json/schema.h"
  #include "mcpkit/protocol/initialize.h"
  #include "mcpkit/protocol/message.h"
  #include "mcpkit/protocol/mrtr.h"
  #include "mcpkit/server/server.h"
  #include "mcpkit/server/session.h"
  ```
- Private static helper `err_resp`:
  ```c
  static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code,
                                 const char *text) {
      return mcp_response_err_new(ctx, req, code, text, NULL);
  }
  ```
- Helpers: `page_offset`, `set_next_cursor`, `set_string`, `get_string`, `find_tool`, `find_resource`, `find_prompt`, `decorate_result`, `tool_error_result`.
- Route implementations: `route_server_discover`, `route_initialize`, `route_tools_list`, `route_tools_call`, `route_resources_list`, `route_resources_read`, `route_prompts_list`, `route_prompts_get`, `route_completion_list`, `route_completion_complete`, `route_advanced`, `route_subscriptions_listen`, `route_tasks_get`, `route_tasks_update`, `route_tasks_cancel`, `route_skills_list`, `route_skills_get`.
- Exported entrypoint `mcp_server_route_request`:
  ```c
  mcp_message_t *mcp_server_route_request(mcp_context_t *ctx, mcp_server_t *srv,
                                          mcp_session_t *s, const mcp_message_t *req,
                                          const char *method) {
      if (strcmp(method, k_mcp_server_methods[0]) == 0) return route_initialize(ctx, srv, s, req);
      if (strcmp(method, k_mcp_server_methods[1]) == 0) {
          mcp_json_value_t *result = mcp_json_object_new(ctx);
          if (result == NULL) return NULL;
          if (decorate_result(ctx, srv, result, false) != MCP_OK) {
              mcp_json_destroy(ctx, result);
              return NULL;
          }
          mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
          if (resp == NULL) mcp_json_destroy(ctx, result);
          return resp;
      }
      if (strcmp(method, k_mcp_server_methods[2]) == 0) return route_tools_list(ctx, srv, s, req);
      if (strcmp(method, k_mcp_server_methods[3]) == 0) return route_tools_call(ctx, srv, s, req);
      if (strcmp(method, k_mcp_server_methods[4]) == 0) return route_resources_list(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[5]) == 0) return route_resources_read(ctx, srv, s, req);
      if (strcmp(method, k_mcp_server_methods[6]) == 0) return route_prompts_list(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[7]) == 0) return route_prompts_get(ctx, srv, s, req);
      if (strcmp(method, k_mcp_server_methods[8]) == 0) return route_completion_list(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[9]) == 0) return route_completion_complete(ctx, srv, s, req);
      mcp_message_t *adv = route_advanced(ctx, srv, s, req, method);
      if (adv != NULL) return adv;
      if (strcmp(method, k_mcp_server_methods[15]) == 0) return route_server_discover(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[16]) == 0) return route_subscriptions_listen(ctx, srv, s, req);
      if (strcmp(method, k_mcp_server_methods[17]) == 0) return route_tasks_get(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[18]) == 0) return route_tasks_update(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[19]) == 0) return route_tasks_cancel(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[20]) == 0) return route_skills_list(ctx, srv, req);
      if (strcmp(method, k_mcp_server_methods[21]) == 0) return route_skills_get(ctx, srv, req);
      dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=unknown_method method=%s", method);
      return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "unknown method");
  }
  ```

- [ ] **Step 2: Add `src/server/routes.c` to `CMakeLists.txt`**

In `CMakeLists.txt`:
```cmake
set(MCPKIT_CORE_SOURCES
  ...
  src/server/server.c
  src/server/registry.c
  src/server/session.c
  src/server/dispatcher.c
  src/server/routes.c
  src/server/queue.c
  ...
)
```

---

### Task 4: Streamline Core Dispatcher Engine (`src/server/dispatcher.c`)

**Files:**
- Modify: `src/server/dispatcher.c`

- [ ] **Step 1: Refactor `src/server/dispatcher.c`**

Replace `src/server/dispatcher.c` with the streamlined pipeline engine:
```c
/**
 * @file dispatcher.c
 *
 * Request dispatch pipeline: validate (L1/L2/L3) -> session gate ->
 * per-session duplicate-id check -> route delegation -> tracing.
 */
#define _DEFAULT_SOURCE

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "internals.h"
#include "../method_table.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/object.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code,
                               const char *text) {
    return mcp_response_err_new(ctx, req, code, text, NULL);
}

void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
               const char *fmt, ...) {
    if (level < (mcp_log_level_t)atomic_load(&srv->log_floor)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    mcp_logger_t *lg = mcp_context_logger(ctx);
    if (lg == NULL) {
        va_end(ap);
        return;
    }
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mcp_logger_log(lg, level, buf);
}

static uint64_t trace_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void trace_begin(mcp_context_t *ctx, mcp_server_t *srv, const char *method) {
    if (srv->tracer != NULL) {
        srv->tracer(ctx, MCP_TRACE_BEGIN, method, 0, 0, srv->tracer_ud);
    }
}

static void trace_end(mcp_context_t *ctx, mcp_server_t *srv, const char *method,
                      uint64_t t0, int status) {
    if (srv->tracer != NULL) {
        srv->tracer(ctx, MCP_TRACE_END, method, status, trace_now() - t0, srv->tracer_ud);
    }
}

mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session,
                                 const mcp_message_t *req, mcp_message_t **resp_out) {
    if (srv == NULL || session == NULL || req == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;
    if (mcp_message_kind(ctx, req) != MCP_MSG_REQUEST) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    atomic_fetch_add(&srv->c_requests_total, 1);
    int code = 0;
    if (mcp_message_validate(ctx, req, &code) != MCP_OK) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=invalid_request code=%d", code);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, code, "invalid request");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    const char *method = mcp_message_method(ctx, req);
    if (method == NULL) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=missing_method");
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "missing method");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    const mcp_json_value_t *req_meta = mcp_message_meta(ctx, req);
    if (req_meta != NULL) {
        const mcp_json_value_t *lv = mcp_json_object_get(ctx, req_meta, "io.modelcontextprotocol/logLevel");
        if (lv == NULL) {
            lv = mcp_json_object_get(ctx, req_meta, "logLevel");
        }
        const char *lvl = NULL;
        if (lv != NULL && mcp_json_string_value(ctx, lv, &lvl) == MCP_OK && lvl != NULL) {
            if (strcmp(lvl, "debug") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_DEBUG);
            } else if (strcmp(lvl, "info") == 0 || strcmp(lvl, "notice") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_INFO);
            } else if (strcmp(lvl, "warning") == 0 || strcmp(lvl, "warn") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_WARN);
            } else if (strcmp(lvl, "error") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_ERROR);
            }
        }
    }
    bool is_stateless = (req_meta != NULL);
    bool is_discover = (strcmp(method, "server/discover") == 0);
    if (!session->initialized && strcmp(method, "initialize") != 0 && !is_discover && !is_stateless) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=uninitialized method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "session not initialized");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    bool is_dup = false;
    mcp_status_t id_st = mcp_idset_contains_msg(ctx, session->seen_ids, req, &is_dup);
    if (id_st != MCP_OK) {
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "id tracking failed");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    if (is_dup) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=duplicate_id method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "duplicate request id");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    id_st = mcp_idset_add_msg(ctx, session->seen_ids, req);
    if (id_st != MCP_OK) {
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "id tracking failed");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    uint64_t t0 = trace_now();
    trace_begin(ctx, srv, method);
    mcp_message_t *resp = mcp_server_route_request(ctx, srv, session, req, method);
    if (resp == NULL) {
        dlogf_srv(ctx, srv, MCP_LOG_ERROR, "event=route_nomem method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        trace_end(ctx, srv, method, t0, MCP_RPC_INTERNAL_ERROR);
        return MCP_ERR_NOMEM;
    }
    int ecode = 0;
    int end_status = mcp_message_error_code(ctx, resp, &ecode) == MCP_OK ? ecode : 0;
    if (end_status != 0) {
        atomic_fetch_add(&srv->c_requests_error, 1);
    }
    trace_end(ctx, srv, method, t0, end_status);
    *resp_out = resp;
    return MCP_OK;
}

mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session,
                               const mcp_message_t *notif) {
    if (srv == NULL || session == NULL || notif == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_message_kind(ctx, notif) != MCP_MSG_NOTIFICATION) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const char *method = mcp_message_method(ctx, notif);
    if (method == NULL) {
        return MCP_OK;
    }
    if (strcmp(method, "notifications/initialized") == 0) {
        session->initialized = true;
    } else if (strcmp(method, "notifications/cancelled") == 0) {
        session->subscription_active = false;
    }
    bool advanced = false;
    for (int i = 0; i < (int)MCP_SERVER_NOTIFICATION_COUNT; i++) {
        if (strcmp(method, k_mcp_server_notifications[i]) == 0) {
            advanced = true;
            break;
        }
    }
    if (advanced) {
        dlogf_srv(ctx, srv, MCP_LOG_DEBUG, "event=notification method=%s", method);
    }
    atomic_fetch_add(&srv->c_notifications_total, 1);
    return MCP_OK;
}
```

- [ ] **Step 2: Build and verify zero warnings**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
Expected: Build succeeds with zero warnings.

- [ ] **Step 3: Run full CTest suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: All 69 tests pass 100%.

- [ ] **Step 4: Commit**

```bash
git add src/server/routes.c src/server/dispatcher.c CMakeLists.txt
git commit -m "refactor(server): extract routes into routes.c and streamline dispatcher pipeline"
```

---

### Task 5: Full Regression Testing & Zero-Warning Gate

**Files:**
- Test all build presets

- [ ] **Step 1: Test clean build with HTTP enabled**

Run:
```bash
cmake -S . -B build-http -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_HTTP=ON
cmake --build build-http --clean-first -j
ctest --test-dir build-http --output-on-failure
```
Expected: 100% tests pass, zero warnings.

- [ ] **Step 2: Test clean build with ASan / UBSan enabled**

Run:
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_HTTP=ON
cmake --build build-asan --clean-first -j
ctest --test-dir build-asan --output-on-failure
```
Expected: 100% tests pass, zero sanitizer complaints.

- [ ] **Step 3: Clean up temporary test build folders**

Run: `rm -rf build-http build-asan`
Expected: Temporary directories removed.

- [ ] **Step 4: Commit and finalize**

```bash
git status
```
Expected: Working tree clean.
