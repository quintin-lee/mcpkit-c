# Resilience Phase 3: Observability Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give hosts structured logs, server counters, and a tracing hook with zero new dependencies.

**Architecture:** A `mcp_logger_logf` formatter (256B stack buffer, truncation-safe) over the existing sink; 4 atomic counters on `mcp_server_t` (C11 stdatomic, dispatcher already runs on workers); a begin/end tracer hook with monotonic duration. Log call sites only on error/lifecycle paths (no per-request success spam beyond DEBUG).

**Tech Stack:** C23, C11 stdatomic, CLOCK_MONOTONIC (already used by timer.c).

---

## Chunk 1: logf + call sites + counters + tracer

### Task 1: `mcp_logger_logf` formatter

**Files:**
- Modify: `include/mcpkit/logging/logger.h`
- Modify: `src/logging/logger.c`
- Test: `tests/unit/test_logger.c` (append cases)

- [ ] **Step 1: Write the failing test.** Append to `tests/unit/test_logger.c`: `mcp_logger_logf(lg, MCP_LOG_INFO, "%s=%d", "k", 7)` reaches mem_sink as `"k=7"`; 300-char format string truncates (sink gets ≤255 chars + NUL).
- [ ] **Step 2: Run test to verify it fails.** Run: `cmake --build build && ./build/tests/test_logger`. Expected: FAIL (implicit decl / link error).
- [ ] **Step 3: Implement.** logger.h decl + Doxygen (document 256B cap + truncation + NULL-logger/message no-op + varargs). logger.c impl with `vsnprintf` into `char buf[256]`, `<stdarg.h>`.
- [ ] **Step 4: Verify.** Rebuild + `./build/tests/test_logger` PASS.
- [ ] **Step 5: Commit.** `git add include/mcpkit/logging/logger.h src/logging/logger.c tests/unit/test_logger.c && git commit -m "feat(logging): add mcp_logger_logf formatter"`

### Task 2: Log call sites (error/lifecycle only)

**Files:**
- Modify: `src/server/dispatcher.c` (validation fail, gate reject, dup id, handler-error isError, NOMEM fallbacks → WARN/ERROR via `mcp_context_logger(ctx)`)
- Modify: `src/runtime/loop.c`, `src/transport/stdio.c` (serve start/stop INFO, parse-fail WARN, shutdown CANCELLED INFO)
- Modify: `src/transport/socket.c`, `src/transport/stdio.c` (recv TIMEOUT → DEBUG; keep quiet, timeouts are routine)

Convention: `mcp_logger_logf(mcp_context_logger(ctx), LEVEL, "k=v ...")` flat `k=v` format. Every call site NULL-safe (NULL ctx → NULL logger → no-op). No per-request success log above DEBUG.

- [ ] **Step 1-4:** Add sites one file at a time, rebuild, `ctest --test-dir build` 40/40 after each file.
- [ ] **Step 5: Commit.** `git commit -m "feat(observability): log error and lifecycle paths"`

### Task 3: Server counters (atomic)

**Files:**
- Modify: `include/mcpkit/server/server.h` (snapshot struct + accessor decl)
- Modify: `src/server/internals.h` (4× `atomic_ullong` fields)
- Modify: `src/server/dispatcher.c` (increment: requests_total on every dispatch, requests_error on error response, notifications_total in notify, tools_called in route_tools_call success)
- Modify: `src/server/server.c` (zero-init in create; atomics need no destroy)

```c
typedef struct mcp_server_counters { uint64_t requests_total, requests_error, notifications_total, tools_called; } mcp_server_counters_t;
mcp_status_t mcp_server_counters(mcp_context_t *ctx, const mcp_server_t *srv, mcp_server_counters_t *out);
```

- [ ] **Step 1-4:** Implement + verify counts via new test (Task 5).
- [ ] **Step 5: Commit.** `git commit -m "feat(server): add atomic dispatch counters"`

### Task 4: Tracer hook (begin/end + duration)

**Files:**
- Modify: `include/mcpkit/server/server.h` (hook type + setter)
- Modify: `src/server/internals.h` (hook fields), `src/server/dispatcher.c` (invoke)

```c
typedef enum { MCP_TRACE_BEGIN, MCP_TRACE_END } mcp_trace_event_t;
typedef void (*mcp_trace_fn)(mcp_context_t *ctx, mcp_trace_event_t ev, const char *method, int status, uint64_t duration_ns, void *userdata);
mcp_status_t mcp_server_set_tracer(mcp_context_t *ctx, mcp_server_t *srv, mcp_trace_fn fn_or_null, void *userdata);
```

Rules: BEGIN fires after gate checks pass (only for routed requests); END fires with status (0 ok / rpc code) + `CLOCK_MONOTONIC` delta. Hook is borrowed, must be thread-safe under threadpool (document). NULL hook = zero overhead (branch check).

- [ ] **Step 1-4:** Implement + verify via new test (Task 5).
- [ ] **Step 5: Commit.** `git commit -m "feat(server): add request tracer hook"`

---

## Chunk 2: Tests + gates

### Task 5: `tests/unit/test_observability.c`

**Files:**
- Create: `tests/unit/test_observability.c`
- Modify: `tests/CMakeLists.txt` (unconditional entry)

Cases: mem-sink captures dispatch error log (bad method → WARN line contains `method=`); counters (init+notify+call+failed call → exact snapshot values); tracer (BEGIN/END pair, END duration_ns > 0, status matches); NULL-hook zero overhead (dispatch works, no crash).

- [ ] **Step 1:** Write test file (uses counting allocator + tmpfile-pair loopback pattern from test_shutdown.c).
- [ ] **Step 2:** Build, run `./build/tests/test_observability` → PASS.
- [ ] **Step 3:** Full `ctest` → 41/41.
- [ ] **Step 4: Commit.** `git commit -m "test(observability): logs, counters, tracer hook"`

### Task 6: Triple gates + cleanup

- [ ] gcc Debug 41/41 → clang 41/41 → ASan+UBSan 41/41 (verify libasan/libubsan linked) → `rm -rf build build-*` → `git status` clean.

---

## Decisions (locked)

1. No mutex in logger: sink thread-safety is host's job under threadpool (documented in logger.h @file + tracer docs). Keeps core zero-dep.
2. No JSON-structured logging: flat `k=v` in message text (YAGNI; hosts parse or attach JSON sinks themselves).
3. No per-request INFO success logs: DEBUG only. Error/lifecycle paths are WARN/INFO/ERROR.
4. Counters live on server (not ctx): they describe dispatch activity; atomics because loop.c dispatches on workers.
