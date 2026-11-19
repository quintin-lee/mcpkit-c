# Design: Full-Repository Architecture & Health Checkup of mcpkit-c (Round 3)

- **Date**: 2026-11-19
- **Status**: Draft (pending human partner approval)
- **Topic**: Full-Repository Architecture, Memory Safety, Usability & Engineering Baseline Audit
- **Output Artifact**: `docs/audit/2026-11-19-analysis.md`
- **Baseline Git Commit**: `c2d55a6` (master), `VERSION 0.1.0`

---

## 1. Intent and Why

Since the last comprehensive checkup on 2026-10-09 (commit `21fd21`), the `mcpkit-c` codebase has undergone substantial expansion:
- Total lines of C code grew to ~27,700 lines across 9 modules (`core`, `json`, `protocol`, `server`, `client`, `transport`, `runtime`, `apps`, `plugin`).
- Test suite expanded from 56 to 87 tests under CTest.
- Complex new capabilities landed:
  - High-concurrency unified asynchronous server (`src/transport/async_server.c`, 1306 lines) multiplexing HTTP POST, SSE, and RFC 6455 WebSocket over a non-blocking Reactor (`epoll` / POSIX `poll`).
  - Dispatcher modularization: split the former monolithic `dispatcher.c` into `src/server/routes.c` (1471 lines), `server.c`, `session.c`, and `method_table.h`.
  - Heavyweight authentication pipeline: `src/core/auth.c` expanded to 1539 lines, incorporating RFC 8693 token exchange, RFC 9207 CIMDS, RFC 9728 AS auto-discovery, and SEP-2243 policy gates.
  - MCP protocol version negotiation matrix expanded to cover 2024-11-05, 2025-06-18, 2025-11-25, and 2026-07-28.

Given this rapid feature landing and structural restructuring, a third comprehensive audit (Round 3) is needed to verify whether architectural principles remain uncompromised, memory safety contracts are rigorously held, public API ergonomics remain cohesive, and past historical debts have been cleared.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
1. **Empirical Gate Verification**: Execute and record real output from automated test gates across build configurations:
   - Release default build (87/87 tests).
   - Sanitizer verification: AddressSanitizer (ASan) + UndefinedBehaviorSanitizer (UBSan).
   - Compiler hygiene: verify `-Wall -Wextra` compile cleanly.
   - Fuzz driver smoke verification.
2. **Historical Debt Closure**: Verify whether the findings from `2026-09-17-analysis.md` and `2026-10-09-analysis.md` (e.g. A-003 `module-reference.md` drift, A-008 dead CMake options, A-013 changelog status, default-allow permissions, and god-file trends) were resolved or still persist.
3. **Architecture & Layering Discipline**: Inspect the `#include` graph and call graph across modules to verify that the strict dependency layering (`apps → server/client → protocol → core`) is maintained and that `core` adheres to the zero-external-dependency rule.
4. **Memory Safety & Concurrency Contracts**: Conduct deep-dive review of dynamic memory lifecycle and ownership transfer semantics (`create` vs. `add_*` ownership, error cleanup paths) in high-risk modules (`auth.c`, `async_server.c`, `routes.c`, `reactor.c`).
5. **API Ergonomics & Usability**: Audit the public headers in `include/mcpkit/**` from the perspective of an external C developer (naming consistency, `mcp_status_t` return contract, context usage, cognitive load, documentation accuracy).
6. **Deliverable**: Produce a structured Markdown report `docs/audit/2026-11-19-analysis.md` containing severity-rated findings (Critical / High / Medium / Low / Info) and a prioritized, actionable remediation roadmap.

### Non-Goals
1. **No Code Modification During Audit**: The audit is strictly read-only. No source files, build files, or documentation will be modified during the audit (remediation is tracked as a subsequent, separate implementation plan).
2. **No Performance Benchmarking**: The audit does not perform throughput or latency profiling.
3. **No Network-Dependent Remote Testing**: Only local and loopback acceptance/fuzz tests are run.

### Anti-Goals
1. **Speculative / Unverified Assertions**: Every reported bug or vulnerability must be supported by code citations (file and line numbers) or reproducible command outputs.
2. **Superficial Linter Output**: The audit must not simply dump compiler warnings or linter output; it must evaluate high-level design, ownership semantics, and API ergonomics.

---

## 3. Constraints

1. **Language & Standard**: C23 (`-std=c2x`), GCC >= 13 or Clang >= 17, CMake >= 3.28.
2. **Zero-Dependency Core**: Core library and protocol layer must depend strictly on standard libc.
3. **Execution Mode**: Sandboxed bash execution for test gates; no destructive file writes outside `docs/audit/2026-11-19-analysis.md` and temporary build directories.

---

## 4. Approaches Considered

### Approach A: Exhaustive Bottom-Up Review
- **Description**: Read all 38+ headers and 40+ source files line by line from bottom to top (`core` up to `apps`), followed by full gate execution and documentation comparison.
- **Trade-off**: Maximally exhaustive, but consumes excessive effort on mature, stable modules that have not changed since prior audits, diluting review depth on recently added high-risk subsystems.

### Approach B: Dual-Track (Global Empirical Gates + Risk-Driven Deep-Dive) — **[SELECTED]**
- **Description**:
  1. Run global empirical gates (Release, ASan/UBSan, Fuzz, `-Wall -Wextra`) across the entire repository to catch regressions globally.
  2. Systematically check off historical debt items from the 09-17 and 10-09 reports.
  3. Conduct targeted line-by-line review of the newly expanded high-risk modules (`src/core/auth.c`, `src/transport/async_server.c`, `src/runtime/reactor.c`, `src/server/routes.c`).
  4. Perform public API usability review across all public headers in `include/mcpkit/`.
- **Trade-off**: High efficiency and maximum depth on areas with the highest risk of concurrency, memory leaks, and architectural erosion, backed by automated gates for 100% codebase coverage.
- **Decision**: User approved Approach B.

---

## 5. Audit Execution Plan & Methodology

### Phase 1: Global Empirical Gates
1. Run Release build and full CTest suite (`ctest --test-dir build`).
2. Run ASan + UBSan build (`build-asan`) with test execution under AddressSanitizer.
3. Run Fuzz drivers (`fuzz_json_stdin`, schema, message, and http variants) on valid and malformed inputs.
4. Run compiler syntax and warning check (`gcc -std=c2x -Wall -Wextra -fsyntax-only`) across key modules.

### Phase 2: Historical Debt Verification
1. Verify dead CMake options: confirm removal of `MCPKIT_BUILD_APPS/CLIENT/SERVER` from `MCPKitOptions.cmake`.
2. Verify compiler flags: confirm `-Wall -Wextra` adoption in `CMakeLists.txt`.
3. Verify documentation drift: check `docs/module-reference.md` against current headers (`dispatcher.h`, `auth.h`, `async_server.h`, etc.).
4. Verify README examples & options tables against actual files in `examples/` and CMake options.
5. Verify `CHANGELOG.md` versioning and release sections.

### Phase 3: Deep-Dive Architectural & Safety Review
1. **Layering & Dependencies**:
   - Check `#include` directives across `src/` and `include/` to confirm no forbidden dependencies (`core -> third-party`, `protocol -> HTTP`, `apps -> pthread`).
   - Check if `src/core/auth.c` (1539 lines) leaks protocol or transport concerns into `core`.
2. **Memory Safety & Ownership**:
   - Deep review of `src/core/auth.c`: memory lifecycle of tokens, discovery cache, and base64 buffers.
   - Deep review of `src/transport/async_server.c`: lifecycle of `async_conn_t`, session map, outbox queue, WebSocket frame buffers, and cleanup upon disconnection.
   - Deep review of `src/runtime/reactor.c`: fd registration, event loop cancellation, timer wheel cleanup.
   - Deep review of `src/server/routes.c`: JSON-RPC dispatch, request context lifecycle, ownership of parameters and responses.
3. **API Ergonomics & Usability**:
   - Review top-level header `include/mcpkit/mcpkit.h` and umbrella includes.
   - Verify `mcp_status_t` error handling consistency across all public APIs.
   - Evaluate client and server startup/teardown boilerplate from a consumer perspective.
   - Verify examples in `examples/` for clarity, idiomatic usage, and correctness.

### Phase 4: Synthesis & Reporting
- Compile findings with exact code line references, severity ratings (Critical, High, Medium, Low, Info), and verified reproduction steps.
- Formulate a prioritized remediation roadmap (P0 Immediate, P1 Near-term, P2 Polish/Docs).
- Write and commit `docs/audit/2026-11-19-analysis.md`.

---

## 6. What is Left to the Builder

- Exact ordering of test runs within the empirical gate phase.
- Exact script commands used to check for forbidden `#include` patterns.
- Detailed formatting and section layout of `docs/audit/2026-11-19-analysis.md` (following the established conventions in `2026-09-17-analysis.md` and `2026-10-09-analysis.md`).
