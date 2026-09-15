# Phase 6 Runtime (executor + timer + loop) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `mcp_executor_t` (sync + threadpool backends), one-shot `mcp_timer_t`, and `mcp_loop_t` (transport→queue→executor→send pump) so the same dispatcher use case runs on both executors.

**Architecture:** Copy the Phase 4 transport pattern exactly: public ops table (`mcp_executor_ops_t` with `submit`/`wait`) + generic `mcp_executor_create` + per-backend constructors (`mcp_sync_executor_create`, `mcp_threadpool_create`) returning `mcp_executor_t *`. Threadpool uses C23 `<threads.h>` only (no pthread types in public headers, spec §27). Queue stays single-threaded; thread-safety contract is documented, not retrofitted.

**Tech Stack:** C23, CMake + CTest, assert-style unit tests, `<threads.h>` + `<time.h>` (CLOCK_MONOTONIC), gcc 16 / clang 22, ASan/UBSan + TSan attempt.

---

## Chunk 1: Header contracts (write these verbatim in T1–T4)

Thread-safety contract (documented in `executor.h`, enforced by tests, NOT by locks in server code):

```text
- Server object must be read-only after setup (no add/remove during execution).
- One mcp_session_t per thread; never share a session across threads
  (session holds initialized flag + client strings + dup-id set).
- mcp_queue_t is NOT thread-safe: only the pump thread pushes/pops it.
- The ctx allocator must be thread-safe (default malloc is; the counting
  test allocator is single-threaded only).
```

`include/mcpkit/runtime/task.h`:

```c
#ifndef MCPKIT_RUNTIME_TASK_H
#define MCPKIT_RUNTIME_TASK_H

typedef struct mcp_context mcp_context_t;

// Task body. fn/arg must outlive the executor wait/destroy that drains them.
typedef void (*mcp_task_fn)(mcp_context_t *ctx, void *arg);

#endif
```

`include/mcpkit/runtime/executor.h`:

```c
#ifndef MCPKIT_RUNTIME_EXECUTOR_H
#define MCPKIT_RUNTIME_EXECUTOR_H

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

typedef struct mcp_executor_ops {
    mcp_status_t (*submit)(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn, void *arg);
    mcp_status_t (*wait)(mcp_context_t *ctx, mcp_executor_t *ex);
    // Backend teardown, called once by mcp_executor_destroy before the shell
    // is freed. Transport has no such op because stdio piggybacks on stop();
    // the executor has no start/stop, so teardown needs its own slot.
    // (sync backend: no-op; threadpool: stop-workers + join + free pool.)
    void (*destroy_backend)(mcp_context_t *ctx, mcp_executor_t *ex);
} mcp_executor_ops_t;

mcp_executor_t *mcp_executor_create(mcp_context_t *ctx, const mcp_executor_ops_t *ops,
                                    void *backend);
void mcp_executor_destroy(mcp_context_t *ctx, mcp_executor_t *ex);
// Backend pointer from create, for ops callbacks; NULL executor yields NULL.
void *mcp_executor_backend(mcp_context_t *ctx, const mcp_executor_t *ex);
// submit: fn/arg borrowed until wait/destroy drains them. NULL fn -> INVALID_ARGUMENT.
// wait: blocks until all submitted tasks finished. Idempotent.
mcp_status_t mcp_executor_submit(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn,
                                 void *arg);
mcp_status_t mcp_executor_wait(mcp_context_t *ctx, mcp_executor_t *ex);

#endif
```

`include/mcpkit/runtime/sync.h`:

```c
#ifndef MCPKIT_RUNTIME_SYNC_H
#define MCPKIT_RUNTIME_SYNC_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

// Inline executor: submit runs fn immediately on the caller thread,
// wait is a no-op returning MCP_OK. Never NULL on success.
mcp_executor_t *mcp_sync_executor_create(mcp_context_t *ctx);

#endif
```

`include/mcpkit/runtime/threadpool.h`:

```c
#ifndef MCPKIT_RUNTIME_THREADPOOL_H
#define MCPKIT_RUNTIME_THREADPOOL_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

// Fixed worker pool. thread_count==0 returns NULL (explicitness over clamping).
// destroy drains pending tasks, then joins workers.
mcp_executor_t *mcp_threadpool_create(mcp_context_t *ctx, size_t thread_count);

#endif
```

`include/mcpkit/runtime/timer.h`:

```c
#ifndef MCPKIT_RUNTIME_TIMER_H
#define MCPKIT_RUNTIME_TIMER_H

#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_timer mcp_timer_t;

mcp_timer_t *mcp_timer_create(mcp_context_t *ctx);
void mcp_timer_destroy(mcp_context_t *ctx, mcp_timer_t *t);
// Schedule fn to fire after delay_ms via CLOCK_MONOTONIC. delay 0 fires on
// the next poll. Returns MCP_OK; never fails except NULL args / NOMEM.
mcp_status_t mcp_timer_schedule(mcp_context_t *ctx, mcp_timer_t *t, uint64_t delay_ms,
                                mcp_task_fn fn, void *arg);
// Cancel all pending entries for (fn, arg). Returns count cancelled.
size_t mcp_timer_cancel(mcp_context_t *ctx, mcp_timer_t *t, mcp_task_fn fn, void *arg);
// Run all due callbacks inline. Returns MCP_OK, *fired_out gets the count
// (optional, may be NULL).
mcp_status_t mcp_timer_poll(mcp_context_t *ctx, mcp_timer_t *t, size_t *fired_out);

#endif
```

`include/mcpkit/runtime/loop.h`:

```c
#ifndef MCPKIT_RUNTIME_LOOP_H
#define MCPKIT_RUNTIME_LOOP_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_executor mcp_executor_t;
typedef struct mcp_loop mcp_loop_t;

// Owns one session + one queue. Does NOT own server/transport/executor.
mcp_loop_t *mcp_loop_create(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t,
                            mcp_executor_t *ex);
void mcp_loop_destroy(mcp_context_t *ctx, mcp_loop_t *loop);
// One pump step: recv line -> parse -> notify | queue+submit+wait+send.
// Transport EOF (recv ERR_IO, cf. mcp_stdio_serve) surfaces as NOT_FOUND,
// matching process_one empty-queue vocabulary. Parse failure -> sends -32700,
// returns MCP_OK.
mcp_status_t mcp_loop_run_once(mcp_context_t *ctx, mcp_loop_t *loop);
// run_once until EOF (NOT_FOUND -> OK). Any other error aborts and propagates.
mcp_status_t mcp_loop_run_until_eof(mcp_context_t *ctx, mcp_loop_t *loop);

#endif
```

File layout at end of phase:

```text
include/mcpkit/runtime/{task,executor,sync,threadpool,timer,loop}.h
src/runtime/{internals.h,executor.c,sync.c,threadpool.c,timer.c,loop.c}
tests/unit/{test_executor,test_timer,test_loop}.c
examples/threadpool-server/main.c
```

`src/runtime/internals.h` holds private structs + `rt_malloc/rt_free/rt_strdup` helpers
(same NULL-ctx-fallback idiom as transport.c: `mcp_context_allocator(ctx)->malloc_fn`,
NULL ctx -> `mcp_default_allocator()`).

---

## Chunk 2: Tasks T1–T5

### Task 1: executor core + sync backend + test

**Files:**
- Create: `include/mcpkit/runtime/task.h`, `include/mcpkit/runtime/executor.h`,
  `include/mcpkit/runtime/sync.h`, `src/runtime/internals.h`,
  `src/runtime/executor.c`, `src/runtime/sync.c`, `tests/unit/test_executor.c`
- Modify: `CMakeLists.txt` (add `src/runtime/executor.c src/runtime/sync.c`),
  `include/mcpkit/mcpkit.h` (+3 includes), `tests/CMakeLists.txt` (+1 test)

- [ ] **Step 1: Write headers + internals + executor.c/sync.c per Chunk 1**
  - `executor.c`: generic create (NULL ops/backend checks like transport.c),
    destroy (free backend? NO — backend owned by backend constructor; generic
    destroy frees only the shell... wait, transport precedent: check transport.c
    first. If `mcp_transport_destroy` frees backend, mirror it. GROUND before writing).
  - `sync.c`: backend = tiny struct (or NULL backend with ops-only? transport
    stdio used a real backend; sync needs no state — use NULL backend, document).
    submit: `fn(ctx, arg); return MCP_OK`. wait: return MCP_OK.
- [ ] **Step 2: Configure, expect link FAIL (test missing)**
- [ ] **Step 3: Write test_executor.c (sync half only for now)**
  - Counter task: `fn` increments `*(int *)arg`. Submit 3, wait, assert 3.
  - NULL fn -> INVALID_ARGUMENT; NULL executor -> INVALID_ARGUMENT (decide:
    dispatchers NULL-check like transport.c — verify transport.c does).
  - Order: sync runs inline — assert counter==1 immediately after submit-before-wait.
  - wait idempotent: call twice, both OK.
- [ ] **Step 4: Build + ctest, expect new test green (22/22)**
- [ ] **Step 5: Commit** `feat(runtime): add executor core with inline sync backend`

### Task 2: threadpool backend + shared dispatcher proof

**Files:**
- Create: `include/mcpkit/runtime/threadpool.h`, `src/runtime/threadpool.c`
- Modify: `CMakeLists.txt` (+1 source), `mcpkit.h` (+1 include),
  `tests/unit/test_executor.c` (+ threadpool cases)

- [ ] **Step 1: Implement threadpool.c**
  - Private: bounded/unbounded task queue (linked list, fine), `mtx_t`,
    `cnd_t`, `stop` flag, `pending` + `active` counters, `thrd_t *workers`.
  - create: starts workers immediately; any `thrd_create` failure ->
    set stop, join started, free all, return NULL.
  - submit: lock, push, `cnd_signal`, unlock. NULL fn -> INVALID_ARGUMENT
    (check BEFORE locking? check first, no lock needed).
  - wait: lock, `while (pending + active > 0) cnd_wait`, unlock.
  - destroy: lock, stop=true, `cnd_broadcast`, unlock, join all, free
    remaining tasks WITHOUT running them? or drain-then-join? DECISION:
    drain-then-join is friendlier but destroy-while-tasks-pending in tests
    is usually a bug; simplest predictable: destroy runs remaining tasks
    inline on the calling thread after join? NO — surprising. Choose:
    destroy discards pending (document: call wait first), joins workers.
    Hmm, leak risk if tasks own memory... tasks are fn/arg borrowed, so
    discard is safe by contract. DOCUMENT in threadpool.h: "destroy discards
    pending tasks; call mcp_executor_wait first." Update Chunk 1 header text
    when writing (one extra comment line, allowed).
  - Worker: lock, while !stop && empty -> cnd_wait; if stop && empty -> exit;
    pop, active++, unlock, run, lock, active--, if 0 -> cnd_broadcast, unlock.
- [ ] **Step 2: Extend test_executor.c (threadpool half)**
  - `mcp_threadpool_create(ctx, 0)` returns NULL.
  - N=32 increment tasks on 4 threads + wait -> counter==32.
  - Slow-task isolation: submit sleep-task (50ms via `thrd_sleep`) + 4 fast
    increments; wait; assert fast ones all ran (counter==4) — proves no
    head-of-line blocking. (Deterministic: any interleaving passes.)
  - Shared dispatcher use case (THE acceptance): one server + echo tool
    (copy echo-handler pattern from test_dispatcher.c — READ it first),
    8 jobs each with OWN session (`mcp_server_create_session`) + own
    `tools/call` request (distinct numeric ids 1..8) calling
    `mcp_server_dispatch` directly (NOT via shared queue — queue is not
    thread-safe by contract). wait, assert all 8 responses OK with echo text.
    Run the SAME 8-job function against sync executor too (jobs run inline).
    One static helper `run_dispatch_jobs(ex, &ok_count)` used for both.
  - destroy-without-wait smoke: submit 2 tasks, destroy immediately, assert
    no hang (test completes). Do NOT assert counter ( racy by design).
- [ ] **Step 3: Build + ctest, expect 22/22 green**
- [ ] **Step 4: Commit** `feat(runtime): add threadpool executor backend`

### Task 3: timer + test

**Files:**
- Create: `include/mcpkit/runtime/timer.h`, `src/runtime/timer.c`,
  `tests/unit/test_timer.c`
- Modify: `CMakeLists.txt`, `mcpkit.h`, `tests/CMakeLists.txt`

- [ ] **Step 1: Implement timer.c**
  - Sorted singly-linked list by `due_ns` (CLOCK_MONOTONIC via
    `clock_gettime`; POTENTIAL GOTCHA: need `_POSIX_C_SOURCE >= 199309L`
    or `_DEFAULT_SOURCE` for the prototype with `-std=c2x --strict`?
    glibc exposes it with `__STDC_VERSION__ >= 199901L`? VERIFY at build;
    if prototype missing, `#define _POSIX_C_SOURCE 200809L` at top of
    timer.c BEFORE includes. Deterministic, no flakiness.)
  - `now_ns()` helper: `ts.tv_sec * 1e9 + ts.tv_nsec`, saturating add for
    `due = now + delay_ms * 1e6` (overflow: delay near UINT64_MAX ->
    saturate to UINT64_MAX, document).
  - poll: pop all with `due <= now`, run inline in deadline order.
- [ ] **Step 2: Write test_timer.c (fully deterministic, NO sleeps)**
  - delay 0 fires on first poll (fired==1, counter==1).
  - future timer (delay 60000) does NOT fire (fired==0).
  - ordering: schedule 0ms B then 0ms A? Same due -> FIFO (assert A,B order
    via recorded sequence). Plus 60000ms C never fires.
  - cancel: schedule X(0ms) + Y(0ms), cancel Y -> poll fires only X (fired==1).
  - cancel missing returns 0.
  - destroy with pending entries: no leak (ASan gate covers).
- [ ] **Step 3: Build + ctest, expect 23/23**
- [ ] **Step 4: Commit** `feat(runtime): add monotonic one-shot timer`

### Task 4: loop + threadpool-server example

**Files:**
- Create: `include/mcpkit/runtime/loop.h`, `src/runtime/loop.c`,
  `tests/unit/test_loop.c`, `examples/threadpool-server/main.c`,
  `examples/CMakeLists.txt` entry (check existing file first — Phase 4/5
  added stdio-server/client entries; append same pattern)
- Modify: `CMakeLists.txt`, `mcpkit.h`, `tests/CMakeLists.txt`

- [ ] **Step 1: Implement loop.c**
  - create: NULL-check server/t; executor NULL -> INVALID? create returns
    NULL on any NULL (mirror transport/server create NULL discipline).
    Creates own session (`mcp_server_create_session`) + queue.
  - run_once job: file-static `loop_job_fn(ctx, arg)` where arg =
    `{loop, resp_out, status_out}` + a `done` cnd? NO — simpler: submit then
    `mcp_executor_wait` (loop is the only submitter, so wait drains exactly
    our job), job writes resp into struct. No per-job signalling needed.
    Job body: `mcp_server_process_one(ctx, loop->server, loop->queue, &resp)`.
  - run_once flow: `recv` -> EOF (OK + NULL line? VERIFY stdio recv EOF
    contract in src/transport/stdio.c BEFORE writing — Phase 4 summary says
    "recv: EOF->OK break", confirm line_out NULL) -> return NOT_FOUND.
    line -> `mcp_json_parse` NULL -> build -32700 response via message API,
    send, return OK. kind==NOTIFICATION -> notify, return OK.
    kind==REQUEST -> queue_push + submit + wait + send resp (NULL resp can
    only happen on NOMEM with resp NULL -> return NOMEM; send failure ->
    return send status).
  - run_until_eof: loop run_once; NOT_FOUND -> return OK; else propagate.
- [ ] **Step 2: Write test_loop.c (tmpfile pair, mirror test_stdio_loopback.c)**
  - Script: initialize -> notifications/initialized -> tools/call echo ->
    EOF. Assert: init result has 2025-06-18, echo text correct, run_until_eof
    returns OK, output has exactly 2 lines (notification silent).
  - Gating proof: tools/call BEFORE initialized -> run_once returns OK but
    line is -32600 error (dispatch-level, proves loop surfaces it).
- [ ] **Step 3: Write examples/threadpool-server/main.c**
  - Real stdin/stdout stdio transport + server(echo + slow tool sleeping
    200ms) + threadpool(4) + loop_run_until_eof. Minimal arg parsing: none
    (fixed 4 threads) — YAGNI.
  - Verify via shell pipe: init + notify + call echo, expect result + rc 0.
- [ ] **Step 4: Build + ctest, expect 24/24; run example pipe, expect rc 0**
- [ ] **Step 5: Commit** `feat(runtime): add executor-driven serve loop and example`

### Task 5: gates + CHANGELOG + cleanup

- [ ] **Step 1: CHANGELOG Phase 6 entry** (one line, same style as Phase 4/5)
- [ ] **Step 2: Triple gates from clean configures, expect 24/24 each**
  - gcc: `cmake -S . -B build ... && cmake --build build && ctest`
  - clang: `CC=clang ... build-clang`
  - ASan+UBSan: manual `-fsanitize=address,undefined` (MCPKIT_ENABLE_ASAN is
    NOT a real option — pass via CFLAGS, verify libasan with `ldd`)
  - TSan: `cmake -S . -B build-tsan -DCMAKE_C_FLAGS="-fsanitize=thread"`;
    if configure/build fails (toolchain lacks tsan) record skip reason,
    NOT a failure. Run ONLY test_executor under TSan at minimum; full ctest
    if green. (TSan + ASan cannot combine — separate build dir.)
- [ ] **Step 3: `rm -rf build build-clang build-asan build-tsan`, confirm
  `git status` clean except intended files**
- [ ] **Step 4: Commit** `chore: log phase 6 runtime`
- [ ] **Step 5: Report: branch `phase6-runtime`, 6 commits, gates evidence**
