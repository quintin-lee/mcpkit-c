# Resilience Phase 2: Graceful Shutdown Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A host can request shutdown (from a signal handler or any thread); serve loops finish the in-flight request, send its response, destroy the session, and return MCP_ERR_CANCELLED.

**Architecture:** Process-wide `volatile sig_atomic_t` flag in core layer (`shutdown.h`/`shutdown.c`, zero-dep, async-signal-safe set/query/clear). Both serve loops (`mcp_loop_run`, `mcp_stdio_serve`) check the flag at loop top and after every recv return (including TIMEOUT, which keeps its existing break semantics). Library installs NO signal handlers; `examples/socket-server` wires SIGTERM/SIGINT as the reference host.

**Tech Stack:** C23, POSIX signals (host-side only), existing ctest harness.

---

## Chunk 1: Shutdown flag (core)

### Task 1: `shutdown.h` + `shutdown.c`

**Files:**
- Create: `include/mcpkit/core/shutdown.h`
- Create: `src/core/shutdown.c`
- Modify: `CMakeLists.txt` (add `src/core/shutdown.c` to core sources)
- Modify: `include/mcpkit/mcpkit.h` (add `#include "mcpkit/core/shutdown.h"` in core block)

API (exact):

```c
void mcp_request_shutdown(void);        // async-signal-safe: sets the flag
bool mcp_shutdown_requested(void);      // returns flag != 0
void mcp_shutdown_clear(void);          // clears the flag
```

- [ ] **Step 1: Write header** with `@file` contract doc at line 1 (per repo convention): process-wide flag, library never installs handlers, host installs SIGTERM/SIGINT handler that calls `mcp_request_shutdown()`, `volatile sig_atomic_t` so the set is async-signal-safe. Needs `#include <stdbool.h>`.
- [ ] **Step 2: Write source**: `static volatile sig_atomic_t g_shutdown = 0;` + three one-line functions. Needs `#include <signal.h>` for `sig_atomic_t`.
- [ ] **Step 3: Wire CMake + umbrella**, build: `cmake -S . -B build -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j`. Expected: 0 errors.
- [ ] **Step 4: Commit** `feat(core): add process-wide shutdown flag`

## Chunk 2: Loop integration

### Task 2: `mcp_loop_run` + `mcp_stdio_serve` check the flag

**Files:**
- Modify: `src/runtime/loop.c`
- Modify: `src/transport/stdio.c`
- Modify: `include/mcpkit/runtime/loop.h` (document CANCELLED return + host handler contract)
- Modify: `include/mcpkit/transport/stdio.h` (same doc addition)

Semantics (exact, both loops):
1. No entry-clear: the flag lifecycle is fully host-visible. A request made before a run takes effect immediately; a host starting a fresh run after a cancelled one calls `mcp_shutdown_clear()` first.
2. Top of each iteration: `if (mcp_shutdown_requested()) { status = MCP_ERR_CANCELLED; break; }`
3. After recv: if `st == MCP_ERR_TIMEOUT` and flag set → CANCELLED/break; if TIMEOUT and flag clear → keep existing behavior (break with TIMEOUT).
4. In-flight request runs to completion (dispatch + send) before the next top-of-loop check sees the flag — this IS the drain. Session destroyed on all exits (already the case).

- [ ] **Step 1: Edit `src/runtime/loop.c`**: add `#include "mcpkit/core/shutdown.h"`, clear on entry, top-of-loop check, TIMEOUT+flag check. Keep all other paths byte-identical.
- [ ] **Step 2: Edit `src/transport/stdio.c`**: same three insertions.
- [ ] **Step 3: Update both headers' docs** (`@return` gains MCP_ERR_CANCELLED; note host must install its own SIGTERM/SIGINT handler; note finite recv timeout needed for prompt wakeup).
- [ ] **Step 4: Build + full ctest** (39/39). Expected: PASS.
- [ ] **Step 5: Commit** `feat(runtime): graceful shutdown checks in serve loops`

### Task 3: Reference host wiring in `examples/socket-server`

**Files:**
- Modify: `examples/socket-server/main.c`

- [ ] **Step 1: Add** `#include <signal.h>`, static handler `on_term(int sig){ (void)sig; mcp_request_shutdown(); }`, install `signal(SIGTERM, on_term); signal(SIGINT, on_term);` before serving. Library stays handler-free.
- [ ] **Step 2: Manual verify**: run server, send init, `kill -TERM <pid>` → process exits promptly (rc 0/1, NOT 143-by-signal... note: default-terminate would be 143; with handler + CANCELLED return path the main returns normally).
- [ ] **Step 3: Commit** `feat(examples): wire SIGTERM/SIGINT to shutdown in socket-server`

## Chunk 3: Tests + gates

### Task 4: `tests/unit/test_shutdown.c`

**Files:**
- Create: `tests/unit/test_shutdown.c`
- Modify: `tests/CMakeLists.txt` (unconditional entry, uses fake transport + loop + stdio serve)

Cases (use existing fake-transport pattern from `test_client.c`, CHECK macro from `test_check.h`):
1. `stop_before_run`: request stop, run `mcp_loop_run` with 1-request script → returns CANCELLED, script unconsumed, server session count 0.
2. `drain_in_flight`: tool handler calls `mcp_request_shutdown()` then returns echo; script has init/notify + 2 calls → first call response sent, loop returns CANCELLED, second call never dispatched (handler invocation count == 1), session count 0.
3. `flag_cleared_on_entry`: after case 2, run again without requesting → normal completion (proves entry-clear).
4. `stdio_serve_drain`: same as 2 but through `mcp_stdio_serve`.
5. `real_signal`: handler for SIGUSR1 → `mcp_request_shutdown()`; tool handler does `raise(SIGUSR1)`; loop returns CANCELLED with response sent. (Synchronous raise = deterministic, no threads.)

- [ ] **Step 1: Write test file.**
- [ ] **Step 2: Build + run**: `./build/tests/test_shutdown`. Expected: exit 0.
- [ ] **Step 3: Full ctest** 40/40.
- [ ] **Step 4: Commit** `test(shutdown): drain, pre-stop, entry-clear, real-signal cases`

### Task 5: Triple gates + cleanup

- [ ] **Step 1: gcc** clean-configure 40/40.
- [ ] **Step 2: clang** clean-configure 40/40.
- [ ] **Step 3: ASan+UBSan** clean-configure 40/40 (+ ldd proof).
- [ ] **Step 4: `rm -rf build build-*`**, `git status` clean.
- [ ] **Step 5: Commit** anything remaining (should be nothing; gates only).
