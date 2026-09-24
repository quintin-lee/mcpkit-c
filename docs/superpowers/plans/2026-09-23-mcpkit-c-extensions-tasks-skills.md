# Tasks, Skills & OpenTelemetry Trace Context Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Implement the official MCP Tasks extension (`io.modelcontextprotocol/tasks`, SEP-2663), Skills extension (`io.modelcontextprotocol/skills`, SEP-2640), and OpenTelemetry W3C Trace Context propagation (`_meta.traceparent`, SEP-414) in pure C99 with zero external dependencies.

**Architecture:**
- **Trace Context (SEP-414):** Lightweight helpers in `include/mcpkit/core/trace.h` and `src/core/trace.c` to parse and format W3C `traceparent` (`version-trace_id-parent_id-flags`), `tracestate`, and `baggage` to/from request/response `_meta` dictionaries.
- **Tasks Extension (SEP-2663):** In-memory task state machine in `include/mcpkit/protocol/tasks.h` and `src/protocol/tasks.c` managing task lifecycles (`working`, `input_required`, `completed`, `failed`, `cancelled`), input request/response bindings, and TTL tracking; wired into `dispatcher.c` routes `tasks/get`, `tasks/update`, and `tasks/cancel`.
- **Skills Extension (SEP-2640):** Skills registry in `include/mcpkit/protocol/skills.h` and `src/protocol/skills.c` exposing `SKILL.md` descriptors, frontmatter metadata, and resource manifests; wired into `dispatcher.c` routes `skills/list` and `skills/get`.
- **Server Discovery & Client Drivers:** `server/discover` dynamically advertises `capabilities.extensions` when tasks and/or skills are registered; `include/mcpkit/client/client.h` and `src/client/client.c` provide client helper methods for polling tasks and discovering skills.

**Tech Stack:** C99 / C23, GCC / Clang, CMake, Zero-dependency pure C core.

---

## File Map

| File | Role |
|---|---|
| `include/mcpkit/core/trace.h` | Declarations for W3C Trace Context struct and `_meta` injection/extraction helpers (SEP-414) |
| `src/core/trace.c` | Implementations for `mcp_trace_extract_from_meta` and `mcp_trace_inject_into_meta` |
| `include/mcpkit/protocol/tasks.h` | Declarations for `mcp_task_status_t`, `mcp_task_desc_t`, and `mcp_task_mgr_t` API (SEP-2663) |
| `src/protocol/tasks.c` | Implementation of `mcp_task_mgr_t` task store, lifecycle transitions, and JSON serialization |
| `include/mcpkit/protocol/skills.h` | Declarations for `mcp_skill_t`, `mcp_skill_registry_t` API (SEP-2640) |
| `src/protocol/skills.c` | Implementation of `mcp_skill_registry_t` and manifest formatting |
| `src/method_table.h` | Add `tasks/get`, `tasks/update`, `tasks/cancel`, `skills/list`, `skills/get` to server methods, and `notifications/tasks` to notifications |
| `src/protocol/validate.c` | L3 parameter validation rules for `tasks/*` and `skills/*` |
| `include/mcpkit/server/server.h` | Add server-level enablement API: `mcp_server_enable_tasks`, `mcp_server_enable_skills` |
| `src/server/internals.h` | Add `task_mgr` and `skill_reg` pointers to `struct mcp_server` |
| `src/server/server.c` | Lifecycle initialization, destruction, and getters for task manager and skills registry |
| `src/server/dispatcher.c` | Routing handlers for `tasks/get`, `tasks/update`, `tasks/cancel`, `skills/list`, `skills/get`, and capability advertisement in `route_server_discover` |
| `include/mcpkit/client/client.h` | Client APIs for tasks polling and skills discovery |
| `src/client/client.c` | Implementation of client helper functions |
| `tests/unit/test_trace_context.c` | Unit tests for W3C trace context extraction/injection |
| `tests/unit/test_tasks_extension.c` | Unit & integration tests for Tasks extension |
| `tests/unit/test_skills_extension.c` | Unit & integration tests for Skills extension |
| `tests/CMakeLists.txt` | Register new unit tests |
| `CHANGELOG.md` | Document added extensions and protocol compliance updates |

---

## Standing Conventions (DO NOT VIOLATE)

- **Pure C99/C23, zero external dependencies**: Only standard C library and internal `mcpkit` modules.
- **Context first argument**: `mcp_context_t *ctx` must be the first parameter in all public APIs. NULL ctx defaults to standard allocator.
- **Memory ownership rules**:
  - Container-owns-on-OK, caller-retains-on-ERR.
  - JSON objects consume taken values via `mcp_json_object_set_take` or `mcp_json_array_append_take`.
  - Borrowed pointers must be documented as `const` and valid while the owner is alive.
- **Error handling**: Return `mcp_status_t` (`MCP_OK` on success, `MCP_ERR_*` on failure).
- **Quality Gates**: All tests must compile cleanly with `-Wall -Wextra -Werror` and pass under `ctest`.

---

## Task 1: W3C Trace Context Propagation (SEP-414)

### Description
Implement OpenTelemetry W3C Trace Context (`traceparent`, `tracestate`, `baggage`) extraction and injection for JSON-RPC request and response `_meta` dictionaries per SEP-414.

**Files:**
- Create: `include/mcpkit/core/trace.h`
- Create: `src/core/trace.c`
- Create: `tests/unit/test_trace_context.c`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/core/trace.h`**

```c
#ifndef MCPKIT_CORE_TRACE_H
#define MCPKIT_CORE_TRACE_H

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_trace_context {
    char traceparent[64];
    char tracestate[256];
    char baggage[256];
} mcp_trace_context_t;

mcp_status_t mcp_trace_extract_from_meta(mcp_context_t *ctx,
                                         const mcp_json_value_t *meta,
                                         mcp_trace_context_t *out);

mcp_status_t mcp_trace_inject_into_meta(mcp_context_t *ctx,
                                        mcp_json_value_t *meta,
                                        const mcp_trace_context_t *trace);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_TRACE_H */
```

- [x] **Step 2: Write failing test `tests/unit/test_trace_context.c`**

```c
#include "mcpkit/core/trace.h"
#include "mcpkit/json/json.h"
#include "test_check.h"
#include <string.h>

int main(void) {
    mcp_context_t *ctx = NULL;
    mcp_json_value_t *meta = mcp_json_object_new(ctx);
    CHECK(meta != NULL);

    mcp_json_value_t *tp = mcp_json_string_new(ctx, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    mcp_json_object_set_take(ctx, meta, "traceparent", tp);

    mcp_trace_context_t trace;
    memset(&trace, 0, sizeof(trace));
    CHECK(mcp_trace_extract_from_meta(ctx, meta, &trace) == MCP_OK);
    CHECK(strcmp(trace.traceparent, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") == 0);

    mcp_json_value_t *out_meta = mcp_json_object_new(ctx);
    CHECK(mcp_trace_inject_into_meta(ctx, out_meta, &trace) == MCP_OK);

    const mcp_json_value_t *v = mcp_json_object_get(ctx, out_meta, "traceparent");
    CHECK(v != NULL);
    const char *str = NULL;
    CHECK(mcp_json_string_value(ctx, v, &str) == MCP_OK);
    CHECK(strcmp(str, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") == 0);

    mcp_json_destroy(ctx, meta);
    mcp_json_destroy(ctx, out_meta);
    return 0;
}
```

- [x] **Step 3: Register test in `tests/CMakeLists.txt` and verify compile failure**

Add `test_trace_context` to `tests/CMakeLists.txt`, run `cmake --build build --target test_trace_context` and verify unresolved symbols.

- [x] **Step 4: Implement `src/core/trace.c`**

Implement `mcp_trace_extract_from_meta` and `mcp_trace_inject_into_meta` with safe bounded string copy (`strncpy` / `snprintf`) and null validation.

- [x] **Step 5: Run tests and verify PASS**

Run `ctest --test-dir build -R test_trace_context --output-on-failure`.

- [x] **Step 6: Git commit**

```bash
git add include/mcpkit/core/trace.h src/core/trace.c tests/unit/test_trace_context.c tests/CMakeLists.txt
git commit -m "feat(core): add W3C trace context extraction and injection for _meta (SEP-414)"
```

---

## Task 2: Tasks Extension Core Data Structures (SEP-2663)

### Description
Implement the core in-memory state manager for MCP Tasks (`io.modelcontextprotocol/tasks`). A task tracks state transitions (`working` -> `input_required` -> `completed` / `failed` / `cancelled`), stores intermediate `inputRequests`, terminal `result` or `error`, status message, `ttlMs`, and `pollIntervalMs`.

**Files:**
- Create: `include/mcpkit/protocol/tasks.h`
- Create: `src/protocol/tasks.c`
- Create: `tests/unit/test_tasks_extension.c`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/protocol/tasks.h`**

```c
#ifndef MCPKIT_PROTOCOL_TASKS_H
#define MCPKIT_PROTOCOL_TASKS_H

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mcp_task_status {
    MCP_TASK_STATUS_WORKING = 0,
    MCP_TASK_STATUS_INPUT_REQUIRED,
    MCP_TASK_STATUS_COMPLETED,
    MCP_TASK_STATUS_FAILED,
    MCP_TASK_STATUS_CANCELLED,
} mcp_task_status_t;

typedef struct mcp_task_desc {
    char task_id[64];
    mcp_task_status_t status;
    uint64_t ttl_ms;
    uint64_t poll_interval_ms;
    char status_message[256];
    const mcp_json_value_t *result;
    const mcp_json_value_t *error;
    const mcp_json_value_t *input_requests;
} mcp_task_desc_t;

typedef struct mcp_task_mgr mcp_task_mgr_t;

mcp_task_mgr_t *mcp_task_mgr_new(mcp_context_t *ctx);
void mcp_task_mgr_free(mcp_context_t *ctx, mcp_task_mgr_t *mgr);

mcp_status_t mcp_task_mgr_create(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                 const char *task_id, uint64_t ttl_ms,
                                 uint64_t poll_interval_ms);

mcp_status_t mcp_task_mgr_set_status(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                     const char *task_id, mcp_task_status_t status,
                                     const char *msg);

mcp_status_t mcp_task_mgr_set_result(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                     const char *task_id, mcp_json_value_t *result);

mcp_status_t mcp_task_mgr_set_error(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                    const char *task_id, mcp_json_value_t *error);

mcp_status_t mcp_task_mgr_set_input_requests(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                             const char *task_id, mcp_json_value_t *input_requests);

mcp_status_t mcp_task_mgr_get(mcp_context_t *ctx, const mcp_task_mgr_t *mgr,
                              const char *task_id, mcp_task_desc_t *out_desc);

mcp_status_t mcp_task_mgr_cancel(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                 const char *task_id);

mcp_json_value_t *mcp_task_desc_to_json(mcp_context_t *ctx, const mcp_task_desc_t *desc);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_TASKS_H */
```

- [x] **Step 2: Write failing test in `tests/unit/test_tasks_extension.c`**

Test task lifecycle:
1. Create task `task-1001` with `poll_interval_ms = 1000`.
2. Inspect initial status == `working`.
3. Transition to `input_required` with `inputRequests` JSON object.
4. Transition to `completed` with `result` JSON object.
5. Serialize to JSON via `mcp_task_desc_to_json` and verify `taskId`, `status`, `pollIntervalMs`, `result` fields match spec.
6. Verify terminal state does not allow invalid transitions.

- [x] **Step 3: Implement `src/protocol/tasks.c`**

Implement `mcp_task_mgr_t` backing array/list with allocator abstraction, proper cleanup on `free`, and `mcp_task_desc_to_json` formatter.

- [x] **Step 4: Build and test**

Run `ctest --test-dir build -R test_tasks_extension --output-on-failure`.

- [x] **Step 5: Git commit**

```bash
git add include/mcpkit/protocol/tasks.h src/protocol/tasks.c tests/unit/test_tasks_extension.c tests/CMakeLists.txt
git commit -m "feat(tasks): implement in-memory MCP Tasks lifecycle manager (SEP-2663)"
```

---

## Task 3: Tasks Extension Server & Dispatcher Integration

### Description
Wire Tasks extension into `struct mcp_server`, `method_table.h`, `validate.c`, and `dispatcher.c`.
Expose `tasks/get`, `tasks/update`, and `tasks/cancel` RPC routes.

**Files:**
- Modify: `src/method_table.h`
- Modify: `src/protocol/validate.c`
- Modify: `include/mcpkit/server/server.h`
- Modify: `src/server/internals.h`
- Modify: `src/server/server.c`
- Modify: `src/server/dispatcher.c`
- Modify: `tests/unit/test_tasks_extension.c`

- [x] **Step 1: Update `src/method_table.h`**
  - Add `"tasks/get"`, `"tasks/update"`, `"tasks/cancel"` to `k_mcp_server_methods`. Update `MCP_SERVER_METHOD_COUNT` (17 -> 20).
  - Add `"notifications/tasks"` to `k_mcp_server_notifications`. Update `MCP_SERVER_NOTIFICATION_COUNT` (10 -> 11).

- [x] **Step 2: Update `src/protocol/validate.c`**
  - In L3 params validation, enforce string `"taskId"` parameter for `tasks/get`, `tasks/update`, and `tasks/cancel`.

- [x] **Step 3: Add Tasks management to `mcp_server_t`**
  - In `include/mcpkit/server/server.h`:
    ```c
    mcp_status_t mcp_server_enable_tasks(mcp_context_t *ctx, mcp_server_t *srv);
    mcp_task_mgr_t *mcp_server_get_task_mgr(mcp_server_t *srv);
    ```
  - In `src/server/internals.h`: add `mcp_task_mgr_t *task_mgr;`.
  - In `src/server/server.c`: initialize `task_mgr = NULL` in `mcp_server_create`, destroy in `mcp_server_destroy`.

- [x] **Step 4: Implement dispatcher routes in `src/server/dispatcher.c`**
  - Add `route_tasks_get`, `route_tasks_update`, `route_tasks_cancel`.
  - When `tasks/get` is called: find task by `taskId`; if not found, return `-32602` Invalid Params; serialize task descriptor and return `mcp_response_ok_new`.
  - When `tasks/update` is called: update input responses, acknowledge with `{ "accepted": true }`.
  - When `tasks/cancel` is called: call `mcp_task_mgr_cancel`, acknowledge with `{ "cancelled": true }`.
  - In `route_server_discover`: if `srv->task_mgr != NULL`, advertise `"io.modelcontextprotocol/tasks": {}` in `capabilities.extensions`.

- [x] **Step 5: Add dispatcher end-to-end test cases in `test_tasks_extension.c`**
  - Call `server/discover` and verify extension capability advertised.
  - Dispatch JSON-RPC `tasks/get` and verify proper response format.

- [x] **Step 6: Build, run tests, and commit**

```bash
git add src/method_table.h src/protocol/validate.c include/mcpkit/server/server.h src/server/internals.h src/server/server.c src/server/dispatcher.c tests/unit/test_tasks_extension.c
git commit -m "feat(server): wire Tasks extension routes tasks/get, tasks/update, tasks/cancel (SEP-2663)"
```

---

## Task 4: Skills Extension Core Data Structures (SEP-2640)

### Description
Implement the core registry for MCP Skills (`io.modelcontextprotocol/skills`). Each skill descriptor contains `name`, `description`, `uri` (`skill://<name>/SKILL.md`), `frontmatter` JSON object, and a file resources manifest (list of `{ uri, digest, size }`).

**Files:**
- Create: `include/mcpkit/protocol/skills.h`
- Create: `src/protocol/skills.c`
- Create: `tests/unit/test_skills_extension.c`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write header `include/mcpkit/protocol/skills.h`**

```c
#ifndef MCPKIT_PROTOCOL_SKILLS_H
#define MCPKIT_PROTOCOL_SKILLS_H

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_skill_resource {
    char *uri;
    char *digest;
    size_t size;
} mcp_skill_resource_t;

typedef struct mcp_skill {
    char *name;
    char *description;
    char *uri;
    mcp_json_value_t *frontmatter;
    mcp_skill_resource_t *resources;
    size_t n_resources;
} mcp_skill_t;

typedef struct mcp_skill_registry mcp_skill_registry_t;

mcp_skill_registry_t *mcp_skill_registry_new(mcp_context_t *ctx);
void mcp_skill_registry_free(mcp_context_t *ctx, mcp_skill_registry_t *reg);

mcp_status_t mcp_skill_registry_add(mcp_context_t *ctx, mcp_skill_registry_t *reg,
                                    const char *name, const char *description,
                                    const char *uri, mcp_json_value_t *frontmatter);

mcp_status_t mcp_skill_add_resource(mcp_context_t *ctx, mcp_skill_registry_t *reg,
                                    const char *name, const char *res_uri,
                                    const char *digest, size_t size);

const mcp_skill_t *mcp_skill_registry_find(const mcp_skill_registry_t *reg,
                                          const char *name_or_uri);

size_t mcp_skill_registry_count(const mcp_skill_registry_t *reg);
const mcp_skill_t *mcp_skill_registry_get_at(const mcp_skill_registry_t *reg, size_t index);

mcp_json_value_t *mcp_skill_to_json(mcp_context_t *ctx, const mcp_skill_t *skill);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_SKILLS_H */
```

- [x] **Step 2: Write failing test in `tests/unit/test_skills_extension.c`**
  - Register skill `"code-review"` with description and frontmatter object.
  - Add resources `skill://code-review/SKILL.md` and `skill://code-review/checklist.md`.
  - Lookup by name and by URI.
  - Verify JSON structure matches SEP-2640 spec.

- [x] **Step 3: Implement `src/protocol/skills.c`**
  - Implement dynamic list with string duplication, frontmatter cloning/ownership transfer, and resource addition.

- [x] **Step 4: Build and test**

Run `ctest --test-dir build -R test_skills_extension --output-on-failure`.

- [x] **Step 5: Git commit**

```bash
git add include/mcpkit/protocol/skills.h src/protocol/skills.c tests/unit/test_skills_extension.c tests/CMakeLists.txt
git commit -m "feat(skills): implement MCP Skills extension registry and manifest serialization (SEP-2640)"
```

---

## Task 5: Skills Extension Server & Dispatcher Integration

### Description
Wire Skills extension into `mcp_server_t`, `method_table.h`, `validate.c`, and `dispatcher.c`.
Expose `skills/list` and `skills/get` RPC routes with pagination and `CacheableResult` (`ttlMs`/`cacheScope`).

**Files:**
- Modify: `src/method_table.h`
- Modify: `src/protocol/validate.c`
- Modify: `include/mcpkit/server/server.h`
- Modify: `src/server/internals.h`
- Modify: `src/server/server.c`
- Modify: `src/server/dispatcher.c`
- Modify: `tests/unit/test_skills_extension.c`

- [x] **Step 1: Update `src/method_table.h`**
  - Add `"skills/list"`, `"skills/get"` to `k_mcp_server_methods`. Update `MCP_SERVER_METHOD_COUNT` (20 -> 22).

- [x] **Step 2: Update `src/protocol/validate.c`**
  - In L3 params validation, enforce either string `"name"` or `"uri"` for `skills/get`.

- [x] **Step 3: Add Skills registry to `mcp_server_t`**
  - In `include/mcpkit/server/server.h`:
    ```c
    mcp_status_t mcp_server_enable_skills(mcp_context_t *ctx, mcp_server_t *srv);
    mcp_skill_registry_t *mcp_server_get_skill_registry(mcp_server_t *srv);
    ```
  - In `src/server/internals.h`: add `mcp_skill_registry_t *skill_reg;`.
  - In `src/server/server.c`: initialize and free.

- [x] **Step 4: Implement dispatcher routes in `src/server/dispatcher.c`**
  - Add `route_skills_list`: returns paginated skills array with `CacheableResult` decoration (`decorate_result(ctx, srv, result, true)`).
  - Add `route_skills_get`: lookup skill by name or URI; return formatted skill descriptor.
  - In `route_server_discover`: if `srv->skill_reg != NULL`, advertise `"io.modelcontextprotocol/skills": {}` in `capabilities.extensions`.

- [x] **Step 5: Run tests and verify PASS**

Run `ctest --test-dir build -R test_skills_extension --output-on-failure`.

- [x] **Step 6: Git commit**

```bash
git add src/method_table.h src/protocol/validate.c include/mcpkit/server/server.h src/server/internals.h src/server/server.c src/server/dispatcher.c tests/unit/test_skills_extension.c
git commit -m "feat(server): wire Skills extension routes skills/list and skills/get (SEP-2640)"
```

---

## Task 6: Client-Side Extension Helper APIs

### Description
Provide high-level client helper functions in `include/mcpkit/client/client.h` and `src/client/client.c` for calling `tasks/get`, `tasks/update`, `tasks/cancel`, `skills/list`, and `skills/get`.

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `src/client/client.c`
- Modify: `tests/unit/test_client.c`

- [x] **Step 1: Add declarations in `include/mcpkit/client/client.h`**

```c
mcp_status_t mcp_client_tasks_get(mcp_context_t *ctx, mcp_client_t *c,
                                  const char *task_id, mcp_message_t **out_resp);

mcp_status_t mcp_client_tasks_update(mcp_context_t *ctx, mcp_client_t *c,
                                     const char *task_id, mcp_json_value_t *input_responses,
                                     mcp_message_t **out_resp);

mcp_status_t mcp_client_tasks_cancel(mcp_context_t *ctx, mcp_client_t *c,
                                     const char *task_id, mcp_message_t **out_resp);

mcp_status_t mcp_client_skills_list(mcp_context_t *ctx, mcp_client_t *c,
                                    const char *cursor, mcp_message_t **out_resp);

mcp_status_t mcp_client_skills_get(mcp_context_t *ctx, mcp_client_t *c,
                                   const char *name_or_uri, mcp_message_t **out_resp);
```

- [x] **Step 2: Implement in `src/client/client.c`**
  - Build JSON-RPC request message with appropriate params dictionary and invoke `mcp_client_send_request()`.

- [x] **Step 3: Add client unit test cases in `tests/unit/test_client.c`**
  - Verify client request construction and response parsing for Tasks and Skills methods.

- [x] **Step 4: Build, run tests, and commit**

```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_client.c
git commit -m "feat(client): add client helper functions for Tasks and Skills extensions"
```

---

## Task 7: Comprehensive Integration & Triple-Gate Verification

### Description
Run the complete test suite across all compiler and sanitizer gates. Verify zero memory leaks under ASan/Valgrind and zero compiler warnings.

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `docs/module-reference.md`

- [x] **Step 1: Run complete test suite**

Run `ctest --test-dir build -j1 --output-on-failure`. Expected: 100% PASS.

- [x] **Step 2: ASan + UBSan gate check**

Configure build with `-DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON` and re-run all test targets.

- [x] **Step 3: Update documentation and Changelog**
  - Add documentation entries for `mcpkit/core/trace.h`, `mcpkit/protocol/tasks.h`, and `mcpkit/protocol/skills.h` in `docs/module-reference.md`.
  - Add `Tasks Extension (SEP-2663)`, `Skills Extension (SEP-2640)`, and `Trace Context (SEP-414)` entries to `CHANGELOG.md`.

- [x] **Step 4: Git commit**

```bash
git add docs/module-reference.md CHANGELOG.md
git commit -m "docs(changelog): document Tasks, Skills, and Trace Context extension implementations"
```
