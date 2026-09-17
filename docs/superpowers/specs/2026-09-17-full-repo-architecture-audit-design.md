# Design: Full-Repository Architecture & Code-Quality Audit of mcpkit-c

Date: 2026-09-17
Status: Approved (user confirmed at design gate)
Approach: B — sequential deep review by the main agent, empirically verified.

## Goal

Produce a complete, evidence-grounded audit report of the mcpkit-c
repository covering memory safety, ownership semantics, concurrency,
layering discipline, API consistency, build-system drift, documentation
accuracy, and test coverage. The report includes a prioritized remediation
plan. No source code is modified during the audit.

## Scope

Entire repository:

| Area | Contents |
|---|---|
| Public API | `include/mcpkit/**` (contract source of truth) |
| Implementation | `src/**` (all layers: core, logging, json, protocol, server, client, transport, runtime, apps, plugin) |
| Tests | `tests/unit`, `tests/acceptance`, `tests/fuzz` |
| Examples | `examples/**` (all seven example programs) |
| Tools | `tools/mcpkit-cli/**` |
| Build | `CMakeLists.txt`, `cmake/**`, options and sanitizer presets |
| Docs | `README.md`, `docs/**`, `CHANGELOG.md` |

Non-goals:
- No code changes during the audit (remediation is a separate task after report sign-off).
- No TSan-only analysis pass beyond what the threadpool exercise triggers; ASan/UBSan are the primary sanitizer gate.
- No performance benchmarking.

## Audit dimensions

1. **Memory safety & ownership**
   - Double-free, use-after-free, leaks on error paths.
   - Ownership transfer boundaries: `create()` → caller owns;
     `mcp_server_add_*()` → server owns; success vs. error retention rules.
   - `ctx == NULL` fallback allocator paths.
2. **Concurrency**
   - Dispatcher single-threaded invariant; threadpool executor task lifecycle.
   - Timer/event loop interaction with the serve loop.
   - Plugin global registry mutex (recent fix `a5fd2b5`).
   - Session-state races under interleaved sessions.
3. **Layering**
   - `core ← protocol ← server/client ← apps`; transport/runtime/json depend
     inward only.
   - `#include` graph and type-name references checked bidirectionally.
   - Forbidden edges: `core → third-party`, `protocol → HTTP`, `apps → pthread`.
4. **API consistency**
   - `mcp_xxx_create/init/destroy/set/get/add/remove/start/stop` naming.
   - `mcp_status_t`-only error returns (no bare `-1`).
   - Doxygen `@file/@brief` contract comments vs. actual behavior.
5. **Build drift**
   - `MCPKIT_BUILD_SOCKET` usage in `CMakeLists.txt` vs. declaration in
     `cmake/MCPKitOptions.cmake` (suspected gap — verify).
   - README build-options table vs. actual CMake options.
   - Shared-library / pkg-config / CMake package-config install paths.
6. **Documentation accuracy**
   - README / quickstart / module-reference / architecture-overview claims vs.
     implementation (feature lists, examples table, CLI usage).
7. **Test coverage gaps**
   - Which source areas have no unit/acceptance protection
     (e.g. socket transport, shared build, fuzz corpus scope).

## Execution sequence (sequential, main agent)

1. Read `include/mcpkit/**` — establish the public contract.
2. Read `src/**` layer by layer in dependency order:
   core → logging → json → protocol → server → client → transport →
   runtime → apps → plugin. Record findings inline.
3. Read `tests/**`, `examples/**`, `tools/**`, `cmake/**`, `docs/**`.
4. Empirical verification:
   - Baseline: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` + full build + `ctest`.
   - Sanitizer gate: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON` + `ctest` + run `threadpool-server` and `fuzz` targets.
   - Socket gate: build with `-DMCPKIT_BUILD_SOCKET=ON` (or equivalent) and exercise the socket path if a test/example exists.
   - Any failing build/test is itself recorded as a finding.
5. Consolidate findings, deduplicate, assign severity and IDs.

## Deliverable

Single file: `docs/audit/2026-09-17-analysis.md`, structure:

```text
§1 Executive summary          — statistics, top risks
§2 Methodology                — dimensions, order, exact commands + observed results
§3 Findings                   — grouped by severity (Critical/High/Medium/Low/Info)
   each: ID (A-001…) | file:line | evidence (code excerpt or command output)
   | risk | recommendation
§4 Test coverage matrix       — module → covered / uncovered / why
§5 Documentation & build drift — discrete list
§6 Remediation plan           — P0 → P1 → P2, each item references §3 IDs,
   with independent acceptance criteria, ready to feed writing-plans
```

### Severity definitions

| Level | Definition |
|---|---|
| Critical | Crash / memory corruption / data race on a production-reachable path |
| High | Ownership or layering violation; API contract breach (consumer misuse yields error) |
| Medium | Boundary bug, untested critical path, or doc/behavior mismatch |
| Low | Style, naming, minor cleanup |
| Info | Observation or suggestion |

### Evidence rules

- Every Critical/High finding MUST carry empirical evidence (reproducible
  command + output excerpt, e.g. ASan report or failing test) OR be marked
  `[INFERENCE]` with the reason it cannot be empirically demonstrated.
- Empirical commands are quoted verbatim in §2 so the report is reproducible.

## Constraints

- Zero code changes during the audit; §6 is a plan only. Remediation runs as
  a separate task after report approval.
- The report is committed to git under `docs/audit/`.
- Stop criteria (all required):
  1. Every source/test/example/tool/build/doc file read at least once.
  2. Baseline + ASan/UBSan + socket builds executed; results (pass or fail)
     recorded in §2.
  3. All Critical/High findings have evidence; §6 covers every Critical/High/Medium.
  4. Report committed to git.

## Out of scope / explicit non-requirements

- No performance tuning analysis.
- No dependency introduction or new-feature design.
- No rewriting of examples or docs beyond the drift list in §5.
