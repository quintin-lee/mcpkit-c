# Full-Repository Architecture & Health Checkup (Round 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Conduct an empirical, full-repository architecture, memory safety, usability, and engineering baseline health checkup across mcpkit-c (~27.7k lines of C23 code, 87 tests) and produce a formal audit report at `docs/audit/2026-11-19-analysis.md` with prioritized remediation recommendations.

**Architecture:** Execute a dual-track audit (Approach B). First, establish concrete empirical baselines via Release, ASan/UBSan, compiler warning, and Fuzz gates. Second, systematically trace historical debts from prior audits and conduct deep-dive code reviews into recently expanded high-risk subsystems (`src/core/auth.c`, `src/transport/async_server.c`, `src/runtime/reactor.c`, `src/server/routes.c`) and public API ergonomics.

**Tech Stack:** C23, CMake, GCC/Clang, CTest, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), deterministic JSON fuzzers.

**Spec:** [2026-11-19-full-repo-architecture-and-health-checkup-design.md](file:///data/home/quintin/workspace/source/c/mcpkit-c/docs/superpowers/specs/2026-11-19-full-repo-architecture-and-health-checkup-design.md)

## Global Constraints

- **Language Baseline:** C23 (`-std=c2x`), GCC >= 13 or Clang >= 17, CMake >= 3.28.
- **Strict Read-Only Audit:** No modifications to existing production code (`src/**`), public headers (`include/**`), or tests (`tests/**`). Only `docs/audit/2026-11-19-analysis.md` and temporary build directories may be written.
- **Zero-Dependency Core:** Core and protocol layers must strictly link against libc only; any external library (OpenSSL, etc.) must remain behind optional transport adapters.
- **Empirical Evidence Required:** Every reported vulnerability, leak, or architectural deviation must be accompanied by exact file/line references and reproducible command output.

## Review Focus

1. **ASan / Leak on Error Paths in `async_server`**: Connection abrupt teardown or malformed HTTP/WebSocket frame handling in `src/transport/async_server.c` must not leak memory or cause use-after-free.
2. **Buffer and Token Ownership in `auth.c`**: Ingestion and caching of tokens, RFC 8693 token exchange outputs, and Base64 SEP-2243 buffers in `src/core/auth.c` must have unambiguous owner deallocation.
3. **Layering Leakage in `core`**: `src/core/auth.c` and other `src/core/*` files must not include HTTP, transport, or server headers or call higher-layer routines.
4. **Header vs. Implementation Signature Drift**: Public declarations in `include/mcpkit/**` must strictly match implementation signatures and error return types (`mcp_status_t`).
5. **Documentation Drift**: Examples in `README.md` and API listings in `docs/module-reference.md` must accurately reflect actual files in `examples/` and actual function prototypes.

---

### Task 1: Empirical Gates Verification

**Files:**
- Output target: `docs/audit/2026-11-19-analysis.md` (§3 Empirical Gates section)
- Examine: `CMakeLists.txt`, `build/`, `build-asan/`

**Interfaces:**
- Consumes: Existing build directories and CMake presets
- Produces: Empirical pass/fail metrics, warning lists, and sanitizer logs for §3 of the report

- [ ] **Step 1: Run default Release build and test gate**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 87/87 tests PASS. Capture total execution time and test count.

- [ ] **Step 2: Run ASan + UBSan test gate**

Run: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure`
Expected: Verify AddressSanitizer and UndefinedBehaviorSanitizer cleanly pass with zero aborts or leak reports. Record results.

- [ ] **Step 3: Run Fuzz smoke gate**

Run:
```bash
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_FUZZ=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_HTTP=ON && cmake --build build-fuzz -j
{ echo '{"jsonrpc":"2.0","id":1,"method":"ping"}'; echo 'not json at all'; } | ./build-fuzz/tests/fuzz_json_stdin
head -c 5000000 /dev/zero | tr '\0' 'a' | ./build-fuzz/tests/fuzz_json_stdin
```
Expected: Exit code 0 for both valid and malformed/oversized inputs; oversized line rejected with protocol error, no crash.

- [ ] **Step 4: Run compiler syntax and warning sweep (`-Wall -Wextra`)**

Run: `gcc -std=c2x -Wall -Wextra -Iinclude -Isrc -fsyntax-only src/core/*.c src/server/*.c src/transport/*.c src/client/*.c src/runtime/*.c`
Expected: Zero compilation errors; note any warnings generated.

- [ ] **Step 5: Record gate metrics**

Consolidate the command lines and verbatim outputs into a draft table for the report.

---

### Task 2: Historical Debt Verification & Status Matrix

**Files:**
- Read: `docs/audit/2026-09-17-analysis.md`, `docs/audit/2026-10-09-analysis.md`
- Inspect: `cmake/MCPKitOptions.cmake`, `CMakeLists.txt`, `docs/module-reference.md`, `README.md`, `CHANGELOG.md`

**Interfaces:**
- Consumes: Findings A-001 through A-024 from 09-17 audit and §5 of 10-09 report
- Produces: Resolution matrix (Resolved, Regressed, Still Open) for §4 of the report

- [ ] **Step 1: Verify dead CMake options status (A-008)**

Check: inspect `cmake/MCPKitOptions.cmake` and `CMakeLists.txt` for `MCPKIT_BUILD_APPS/CLIENT/SERVER`.
Expected: Verify whether dead options were removed or still exist.

- [ ] **Step 2: Verify `-Wall -Wextra` integration status**

Check: search `CMakeLists.txt` for `add_compile_options(-Wall -Wextra)`.
Expected: Confirm compiler warnings are now enforced by build configuration.

- [ ] **Step 3: Verify documentation sync for `dispatcher` API (A-003) and new modules**

Check: inspect `docs/module-reference.md` around lines describing dispatcher vs. `include/mcpkit/server/dispatcher.h`.
Check if newly added modules (`mrtr.h`, `tasks.h`, `skills.h`, `sse.h`, `async_server.h`, `ws.h`) are documented.
Expected: Document exact line numbers and missing API references.

- [ ] **Step 4: Verify README examples and build options tables (A-010 / A-011)**

Check: Compare entries in `README.md` (lines 60-72 for build options, lines 83-100 for examples) against actual options in `cmake/MCPKitOptions.cmake` and executables in `examples/`.
Expected: Identify any omitted examples (e.g. `mrtr-server`, `socket-server`, `async-server`) or undocumented CMake flags.

- [ ] **Step 5: Verify `CHANGELOG.md` release status (A-013)**

Check: inspect `CHANGELOG.md` to see if entries remain only under `[Unreleased]` while `VERSION` is 0.1.0.
Expected: Record status and whether formal release tag sections exist.

- [ ] **Step 6: Verify default-allow permission model (A-023 / 10-09 finding)**

Check: examine session creation in `src/server/session.c` for default `granted = 0xFFFFFFFF` and whether this behavior is explicitly documented.
Expected: Note security and documentation implications.

---

### Task 3: Layering & Dependency Boundaries Deep-Dive

**Files:**
- Inspect: all files in `src/**` and `include/mcpkit/**`
- Examine: `src/core/auth.c`, `src/server/routes.c`, `src/transport/async_server.c`, `src/runtime/reactor.c`

**Interfaces:**
- Consumes: C source files across all 9 subsystems
- Produces: Layering violation inventory and coupling analysis for §5 of the report

- [ ] **Step 1: Check for forbidden upward `#include` dependencies in `core`**

Run:
```bash
grep -rnE '#include [<"]mcpkit/(protocol|server|client|transport|runtime|apps|plugin)' src/core/ include/mcpkit/core/
```
Expected: 0 matches. Verify `core` does not depend on higher layers.

- [ ] **Step 2: Check for forbidden external dependencies in `core` and `protocol`**

Run:
```bash
grep -rnE '#include [<"](openssl|uv|curl|pthread\.h)' src/core/ src/protocol/ include/mcpkit/core/ include/mcpkit/protocol/
```
Expected: 0 matches. Verify `core` uses only standard C23 / libc primitives (e.g. `<threads.h>` / `<stdatomic.h>` instead of `<pthread.h>`).

- [ ] **Step 3: Analyze coupling of `src/core/auth.c`**

Inspect: `src/core/auth.c` (1539 lines). Trace whether OAuth2 discovery, RFC 8693 token exchange, or SEP-2243 base64 processing introduce protocol-level message envelopes or transport assumptions into `core`.
Expected: Document architectural positioning of auth logic: is it purely core-level crypto/token/metadata or does it cross into protocol parsing?

- [ ] **Step 4: Analyze dispatcher decoupling in `src/server/routes.c`**

Inspect: `src/server/routes.c` (1471 lines) and `src/server/server.c` (703 lines).
Evaluate how methods are registered and dispatched via `src/method_table.h`.
Expected: Assess whether the split resolved the previous "god-file" issue and whether route handlers cleanly separate JSON extraction, domain logic, and serialization.

---

### Task 4: Memory Safety & Ownership Lifecycle Review

**Files:**
- Inspect: `src/core/auth.c`, `src/transport/async_server.c`, `src/transport/ws.c`, `src/runtime/reactor.c`, `src/server/session.c`, `src/server/routes.c`

**Interfaces:**
- Consumes: C source implementations of resource allocation and deallocation
- Produces: Findings list on ownership transfer, error-path leaks, and concurrency safety for §6 of the report

- [ ] **Step 1: Audit ownership transfer in `src/core/auth.c`**

Inspect: functions allocating tokens, CIMDS cache items, and base64 buffers (e.g. `mcp_auth_token_set`, `mcp_auth_pkce_create`, discovery endpoints).
Check: does caller own the returned strings/structures, or does context own them? Are cleanup routines provided and exercised? Are error rollback paths free of leaks?
Expected: Note any ambiguous ownership or missing frees on parsing failures.

- [ ] **Step 2: Audit connection and session lifecycle in `src/transport/async_server.c`**

Inspect: `async_conn_t` allocation, socket buffer resize/drain, outbox linked list/ring buffer, and WebSocket frame fragmentation assembly.
Check:
- When a connection disconnects unexpectedly (e.g. client resets TCP or sends partial frame), are all inflight frames, session pointers, and outbox buffers completely freed?
- How does `test_async_destroy_inflight` and `test_async_outbox_race` exercise these paths?
Expected: Record exact lifecycle paths and any risk of dangling pointers or double frees.

- [ ] **Step 3: Audit Reactor event loop and timer wheel cleanup in `src/runtime/reactor.c`**

Inspect: epoll/poll event handling, fd unregistration, and monotonic timer wheel bucket destruction.
Check: Are timers that expire concurrently with cancellation safely handled without use-after-free?
Expected: Document findings with exact line references.

- [ ] **Step 4: Audit JSON-RPC request and response lifecycle in `src/server/routes.c`**

Inspect: dynamic JSON allocations during tool execution, resource reading, and prompt generation.
Check: does the server guarantee ownership transfer of tool result contents to the response envelope, and does it cleanly free parameters when tool invocation fails?
Expected: Verify adherence to the project rule: "On a successful call the container owns the child value; on error the caller retains it."

---

### Task 5: API Ergonomics & Usability Review

**Files:**
- Inspect: all headers in `include/mcpkit/**`
- Inspect: `examples/minimal-server/main.c`, `examples/stdio-server/main.c`, `examples/async-server/main.c`, `examples/client/main.c`

**Interfaces:**
- Consumes: Public API declarations and example usage code
- Produces: Ergonomics evaluation, naming consistency report, and usability friction points for §7 of the report

- [ ] **Step 1: Check public API naming consistency**

Examine all public functions across headers:
Check whether every function strictly adheres to `mcp_<module>_<verb>` (e.g. `mcp_server_create`, `mcp_client_init`, `mcp_ws_transport_create`).
Check whether every function returns `mcp_status_t` for operations that can fail (no bare `int` or `-1`).
Expected: Catalog any deviations or naming anomalies.

- [ ] **Step 2: Evaluate context parameter conventions**

Check whether every public API consistently takes `mcp_context_t *ctx` as the first argument, and whether `ctx == NULL` correctly defaults to libc `malloc`/`free` and standard logger.
Expected: Identify any functions that deviate from the context-first pattern.

- [ ] **Step 3: Evaluate developer onboarding friction from examples**

Review `examples/minimal-server/main.c`, `examples/stdio-server/main.c`, and `examples/async-server/main.c`.
Assess: How many lines of boilerplate are needed to register a tool and run a server? Are error codes intuitive? Is ownership transfer self-explanatory from the API signatures?
Expected: Highlight strengths and practical developer friction points.

---

### Task 6: Audit Report Synthesis & Prioritized Remediation Roadmap

**Files:**
- Create: `docs/audit/2026-11-19-analysis.md`

**Interfaces:**
- Consumes: Outputs from Tasks 1 through 5
- Produces: Completed Markdown audit report with executive scorecard, findings, and P0-P3 roadmap

- [ ] **Step 1: Structure the audit report document**

Follow the proven structure from `docs/audit/2026-09-17-analysis.md` and `docs/audit/2026-10-09-analysis.md`:
- §1 Executive Summary & Health Scorecard
- §2 Baseline & Methodology
- §3 Empirical Gates & Verification Data
- §4 Historical Audit Debt Closure Check
- §5 Architecture & Layering Discipline
- §6 Memory Safety, Ownership & Concurrency
- §7 API Ergonomics, Usability & Documentation Fidelity
- §8 Prioritized Remediation Roadmap (P0 Critical, P1 High, P2 Medium, P3 Polish)

- [ ] **Step 2: Populate with verified findings and code citations**

Write out every finding with:
- ID (e.g. A3-001, A3-002...)
- Severity (Critical / High / Medium / Low / Info)
- Location (file and exact lines)
- Description of the issue and why it matters
- Concrete evidence (reproduction command or code snippet)

- [ ] **Step 3: Formulate actionable remediation roadmap**

Group recommendations into prioritized tiers:
- P0: Must-fix before next release (memory leaks, crash vectors, severe API breaking changes)
- P1: Architectural hygiene and major debt (decoupling large modules, compiler flags)
- P2: Documentation and API ergonomics (syncing `module-reference.md`, README tables)
- P3: Nice-to-have enhancements

- [ ] **Step 4: Review and finalize `docs/audit/2026-11-19-analysis.md`**

Ensure all markdown links use relative or `file:///` format, all claims have supporting commands, and no unverified speculation exists.
