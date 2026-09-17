# Full-Repository Architecture & Quality Audit — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce `docs/audit/2026-09-17-analysis.md` — a complete, evidence-grounded audit of mcpkit-c (memory safety, ownership, concurrency, layering, API consistency, build drift, docs, test coverage) plus a prioritized remediation plan — without modifying any source code.

**Architecture:** Sequential deep review (approach B): the executing engineer reads the whole repository in dependency order, records findings with evidence, then runs three build configurations (baseline, ASan+UBSan, socket) as the empirical gate, consolidates findings with severity IDs, and writes one report file whose §6 feeds a future writing-plans remediation cycle.

**Tech Stack:** C23, CMake ≥ 3.28, ctest, GCC/Clang sanitizers; Markdown for the report.

**Spec:** `docs/superpowers/specs/2026-09-17-full-repo-architecture-audit-design.md`

**Working tree:** Run in a dedicated worktree per the brainstorming skill (or on master — the plan creates one new file and commits it; no source edits are allowed by spec).

**Ground rules (from spec, repeated for the executor):**
- ZERO source/test/example/tool/cmake changes. The only file created or modified is `docs/audit/2026-09-17-analysis.md` (and its git commit).
- Every Critical/High finding carries empirical evidence (verbatim command + output excerpt) or is explicitly marked `[INFERENCE]` with the reason.
- Failing builds or tests are findings, not blockers: record and continue.

---

## Chunk 1: Setup & Baseline

### Task 1: Audit workspace & baseline build

**Files:**
- Create: `docs/audit/` (directory)
- Read-only: `CMakeLists.txt`, `cmake/MCPKitOptions.cmake`, `cmake/MCPKitSanitizers.cmake`

- [ ] **Step 1: Create the audit directory and a scratch findings file**

```bash
mkdir -p docs/audit
# Scratch file for raw notes (NOT the deliverable; delete before commit).
touch docs/audit/NOTES.tmp
```

Purpose: one scratch file for raw findings as they are discovered; the final deliverable is assembled in Task 8.

- [ ] **Step 2: Verify build options exist as documented**

Read `cmake/MCPKitOptions.cmake`. Confirm these options are declared:
`MCPKIT_BUILD_TESTS`, `MCPKIT_BUILD_EXAMPLES`, `MCPKIT_BUILD_TOOLS`,
`MCPKIT_BUILD_HTTP`, `MCPKIT_BUILD_FUZZ`, `MCPKIT_ENABLE_ASAN`,
`MCPKIT_ENABLE_UBSAN`, `MCPKIT_ENABLE_TSAN`, `MCPKIT_BUILD_SHARED`,
`MCPKIT_BUILD_SOCKET`.
Record any missing declaration in `docs/audit/NOTES.tmp` (candidate finding: build-doc drift).

Expected: a list of declared vs. used options written to NOTES.tmp.

- [ ] **Step 3: Baseline release build + full test suite**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Record in `docs/audit/NOTES.tmp` (exact text for report §2):
- The exact commands run (verbatim).
- Build result (success/fail + first error lines if fail).
- Test summary line: `X tests passed, Y failed` plus the names of any failures.

Expected: at minimum, `ctest` summary recorded. If build fails, record failure output and still proceed to Task 2 (reading does not require a build).

- [ ] **Step 4: Commit nothing yet; verify no source files touched**

```bash
git status --short
```

Expected: only `docs/audit/NOTES.tmp` (untracked) and no modified tracked files. If anything else changed, stop and revert — the audit must not modify sources.

---

## Chunk 2: Contract & Source Deep Read

### Task 2: Read public API contract

**Files:**
- Read-only: `include/mcpkit/**/*.h` (all public headers)

- [ ] **Step 1: Enumerate and read every public header**

List headers with:

```bash
find include/mcpkit -name '*.h' | sort
```

Read each header in full. While reading, note in `docs/audit/NOTES.tmp`:
- API style violations (functions not following `mcp_xxx_create/init/destroy/set/get/add/remove/start/stop`, or returning bare `-1` / non-`mcp_status_t` errors).
- Ownership claims in `@return`/`@brief` that contradict the README rule ("on error the caller retains ownership" vs. what the header says).
- Doxygen `@file`/`@brief` contract gaps (missing or wrong).

Expected: NOTES.tmp has a per-header list of issues (or "clean" per header).

### Task 3: Read implementation layers in dependency order

**Files:**
- Read-only: `src/core/**`, `src/logging/**`, `src/json/**`, `src/protocol/**`, `src/server/**`, `src/client/**`, `src/transport/**`, `src/runtime/**`, `src/apps/**`, `src/plugin/**`

- [ ] **Step 1: Read core/logging/json/protocol**

Read in this order, recording findings per the dimension checklist (memory safety, ownership, layering, API consistency):
1. `src/core/version.c`, `src/core/error.c`, `src/core/allocator.c`, `src/core/context.c`
2. `src/logging/logger.c`
3. `src/json/builtin.c`, `src/json/api.c`, `src/json/schema.c`
4. `src/protocol/message.c`, `src/protocol/initialize.c`, `src/protocol/validate.c`

Specific checks while reading:
- Every `mcp_*_create/destroy` pair: on which paths is memory freed? Is the failure path's ownership transfer documented and implemented?
- `ctx == NULL` fallback: which functions assume non-NULL context after accepting NULL?
- Layering: does any file in `core`, `logging`, `json`, `protocol` include or reference an outward-layer symbol (server/client/apps type or function, HTTP types, `pthread_`)? Any hit is a finding.

- [ ] **Step 2: Read server layer**

Files: `src/server/server.c`, `src/server/registry.c`, `src/server/session.c`, `src/server/dispatcher.c`, `src/server/internals.h`.

dispatcher.c is the largest file (27 KB) — read it in 2–3 passes. Checks:
- Session state: which fields are read/written inside `mcp_server_dispatch` or the serve loop, and can the threadpool executor's callbacks touch them concurrently? Record any path where a tool handler callback could read session state the dispatcher is mutating.
- Request-id tracking per session: verify the lookup is O(n) bounded and error paths free allocations.
- Recent fix `4165628` split a chained `object_set` in `mcp_client_complete`; check the analogous pattern in dispatcher (clone-then-pass) is correct after `6b4d8af` (completion provider arg clone): verify the cloned object is freed on every error return.

- [ ] **Step 3: Read client + transport**
Files: `src/client/client.c`, `src/transport/transport.c`, `src/transport/stdio.c`, `src/transport/http.c`, `src/transport/streamable_http.c`, `src/transport/socket.c`, `src/transport/internals.h`.

Checks:
- stdio framing: 4 MB cap — verify the cap is enforced on every path (read loop, not just one branch).
- http/streamable_http: buffer growth, session map lifecycle (add/destroy while streaming), SSE GET path.
- socket.c: newly added (recent) — verify file descriptor lifecycle, non-blocking error handling, no leaked connections on error.
- Layering: transport must not reference server/apps types beyond the core interface seams.

- [ ] **Step 4: Read runtime + apps + plugin**

Files: `src/runtime/executor.c`, `src/runtime/sync.c`, `src/runtime/threadpool.c`, `src/runtime/timer.c`, `src/runtime/loop.c`, `src/apps/csp.c`, `src/apps/ui.c`, `src/plugin/plugin.c`.

Checks:
- threadpool: task enqueue after `stop`, worker join vs. destroy order, task context capture lifetime.
- timer/loop: timer wheel monotonicity, loop stop reentrancy.
- plugin (recent fix `a5fd2b5`): global registry mutex — verify the lock is held across register/lookup/unregister and no lock-order inversion with other globals.
- apps/ui (recent fix `ff1b788`): verify the split `object_set` chain — each set returns status that must be checked; no leak of a staged JSON value if the second set fails.
- Layering: apps must not use `pthread_` or include transport headers.

Expected: NOTES.tmp now has per-file, per-dimension findings with `file:line` anchors.

### Task 4: Read tests, examples, tools, cmake, docs

**Files:**
- Read-only: `tests/**`, `examples/**`, `tools/mcpkit-cli/**`, `cmake/**`, `README.md`, `docs/quickstart.md`, `docs/module-reference.md`, `docs/architecture/overview.md`, `CHANGELOG.md`

- [ ] **Step 1: Map test coverage**

List every test binary and its target source area:

```bash
ls tests/unit tests/acceptance tests/fuzz
grep -rn "add_test" tests/CMakeLists.txt
```

- `shared` build, `plugin` registry: expected gaps — confirm and note why (no test target exists).
- socket transport and Streamable HTTP: dedicated tests `test_socket`, `test_http`/`test_http_serve` exist but are option-gated (default OFF) — confirm they run under the Task 6 gate build; a gate build where they silently don't run is a finding.
- Any unit area with zero test references → finding (Medium, untested critical path).

- [ ] **Step 2: Cross-check docs vs. reality**
- README "Examples" table lists 7 examples; `examples/` has 9 subdirectories —
  confirm the count and record which are missing from the table (expected:
  `prompt-server` and `socket-server`).
- README build-options table vs. `cmake/MCPKitOptions.cmake` declared options —
  expected omissions: `MCPKIT_BUILD_SHARED`, `MCPKIT_BUILD_SOCKET`,
  `MCPKIT_ENABLE_TSAN`; verify each.
- `docs/architecture/overview.md` claims ("dispatcher is single-threaded", "threadpool main thread waits each iteration") vs. `src/server/dispatcher.c` + `src/runtime/threadpool.c` actual behavior.
- `CHANGELOG.md` "Unreleased" phase list vs. what actually shipped in recent commits.
- `docs/module-reference.md` function lists vs. actual headers.


- [ ] **Step 3: Read examples & CLI for misuse**
Skim each example's main() for ownership misuse of the library API (the examples are the reference usage — misuse there propagates). Check `tools/mcpkit-cli` for pipe/process lifecycle issues (zombie children, unclosed fds on error).

Expected: NOTES.tmp complete — every dimension from the spec has entries (findings or "verified clean").

---

## Chunk 3: Empirical Sanitizer & Feature Gates

### Task 5: ASan/UBSan gate

- [ ] **Step 1: Build and run the full suite under sanitizers**

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Record verbatim in NOTES.tmp: commands, build result, test summary, and the full ASan/UBSan report for any failure (that report text is the finding's evidence).

- [ ] **Step 2: Exercise the threadpool example under ASan**

```bash
# Confirm the example binary name first:
ls build-asan/examples/
./build-asan/examples/threadpool-server   # adjust name if different; run until it exits
```

If the serve loop never exits on its own, run with a bounded input:
`echo -n '' | timeout 10 ./build-asan/examples/threadpool-server` (empty stdin EOF should terminate cleanly). Record ASan output at exit. Any leak/UBSan report = Critical/High finding with this exact repro command.

- [ ] **Step 3: Fuzz corpus under ASan (if built)**
Fuzz drivers read from stdin; no committed corpus files exist in `tests/fuzz`
(confirm with `ls tests/fuzz`). Feed each driver synthetic input — valid
envelopes, malformed JSON, oversized lines — e.g.:
```bash
cmake -S . -B build-asan -DMCPKIT_BUILD_FUZZ=ON -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON
cmake --build build-asan -j   # all fuzz targets: fuzz_json_stdin, fuzz_schema_stdin, fuzz_message_stdin, fuzz_http_stdin (HTTP-gated)
```

Run the drivers and record any crash report (full ASan text) as evidence for a Critical finding.

Expected: NOTES.tmp now holds the sanitizer evidence section (verbatim command + output per gate, or "clean" statements).

### Task 6: Socket + HTTP build gate

- [ ] **Step 1: Build with socket transport**

```bash
cmake -S . -B build-socket -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_HTTP=ON
cmake --build build-socket -j
```
Both options are declared in `cmake/MCPKitOptions.cmake` (`MCPKIT_BUILD_SOCKET` default ON, `MCPKIT_BUILD_HTTP` default OFF) — verify in Task 1 Step 2. An undeclared cache variable used in `if()` simply evaluates falsy and the branch is skipped with no warning; if a gate build ever succeeds WITHOUT socket code, that silent-skip is a Critical build-system finding. Record which case occurred.

- [ ] **Step 2: Confirm socket/http tests run under the gate build**

```bash
ctest --test-dir build-socket --output-on-failure
```
Record summary. Gated tests `test_socket` and `test_http`/`test_http_serve` DO exist (gated on `MCPKIT_BUILD_SOCKET`/`MCPKIT_BUILD_HTTP` in `tests/CMakeLists.txt`) — confirm they run here and pass. If any expected test does not run, that is a §4/§5 finding.

Expected: NOTES.tmp holds the socket/HTTP gate evidence.

---

## Chunk 4: Report Authoring

### Task 7: Consolidate & classify findings

- [ ] **Step 1: Deduplicate and assign IDs**

From NOTES.tmp, build the finding list:
- Number A-001, A-002, … in report order.
- Group by severity per the spec definitions:
  - **Critical**: crash / memory corruption / data race, production-reachable.
  - **High**: ownership or layering violation; API contract breach.
  - **Medium**: boundary bug, untested critical path, doc/behavior mismatch.
  - **Low**: style/naming/minor.
  - **Info**: observations.
- For each finding: `ID | file:line | evidence | risk | recommendation`.
  Evidence = code excerpt (with line numbers) OR verbatim command+output. If a Critical/High has no empirical proof, mark it `[INFERENCE]` and state why it cannot be demonstrated.

- [ ] **Step 2: Draft the remediation plan (§6)**

Order P0 (blocks everything: crashes, races, silent build-ignore) → P1 (High) → P2 (Medium/Low). Each §6 item:
- References one or more §3 finding IDs.
- Names the files to change.
- States an independent, checkable acceptance criterion (e.g. "A-004: after fix, `ctest --test-dir build-asan` reports 0 ASan failures in test X" — not "improve cleanup").
P0/P1 items must be specific enough that a later writing-plans pass can turn them directly into TDD steps.

- [ ] **Step 3: Draft §1–§5**

- §1 Executive summary: total counts by severity, top-3 risks in one line each.
- §2 Methodology: the 7 dimensions, the read order, and every empirical command with its observed result (from Tasks 1, 5, 6 NOTES entries, verbatim).
- §3 Findings: the classified list from Step 1.
- §4 Coverage matrix: module → covered (test name) / uncovered + reason.
- §5 Drift list: every doc/build mismatch from Task 4.

### Task 8: Write the report and commit

**Files:**
- Create: `docs/audit/2026-09-17-analysis.md`
- Delete: `docs/audit/NOTES.tmp` (scratch only; never committed)

- [ ] **Step 1: Assemble `docs/audit/2026-09-17-analysis.md`**

Structure (exact section order per spec):

```markdown
# mcpkit-c Architecture & Code-Quality Audit — 2026-09-17

## §1 Executive summary
## §2 Methodology
## §3 Findings
### Critical
### High
### Medium
### Low
### Info
## §4 Test coverage matrix
## §5 Documentation & build drift
## §6 Remediation plan
### P0 — blocking
### P1 — high
### P2 — medium/low
```

Each §3 finding format:

```markdown
#### A-00N — <short title>  [Critical|High|Medium|Low|Info]
- Location: `path:line`
- Evidence: <code excerpt with line numbers, OR: command + output excerpt, OR `[INFERENCE]: <reason>`>
- Risk: <what breaks, when>
- Recommendation: <concrete fix, names symbols>
```

§6 item format:

```markdown
- [P0] <title> (A-00X, A-00Y)
  - Files: `src/...`, `include/mcpkit/...`
  - Acceptance: <mechanically checkable statement>
```

- [ ] **Step 2: Verify report stop-criteria checklist**

Check each spec stop criterion before committing:
1. Every source/test/example/tool/build/doc file read (task checkboxes Tasks 2–4 all complete).
2. §2 contains baseline + ASan/UBSan + socket gate commands AND observed results.
3. Every Critical/High finding in §3 has evidence or an explicit `[INFERENCE]` tag; §6 covers every Critical/High/Medium finding ID.
4. No `[TODO]`/placeholder text remains in the report.

Run:

```bash
grep -n "TODO\|TBD\|placeholder" docs/audit/2026-09-17-analysis.md   # expected: no matches
awk '/^#### A-/{n++} END{print n" findings"}' docs/audit/2026-09-17-analysis.md
```

- [ ] **Step 3: Remove scratch, commit the report**

```bash
rm docs/audit/NOTES.tmp
git add docs/audit/2026-09-17-analysis.md
git commit -m "docs: full-repo architecture & quality audit 2026-09-17"
git log --oneline -1
```

Expected: one new committed file `docs/audit/2026-09-17-analysis.md`; `git status` clean; no source files modified.

- [ ] **Step 4: Clean build directories (optional, keeps worktree light)**

```bash
rm -rf build build-asan build-socket
```

(Safe: these are untracked build dirs; `git status` must still be clean afterwards.)

---

## Notes for the executor

- **Never** edit `src/`, `include/`, `tests/`, `examples/`, `tools/`, `cmake/` during this plan. If a fix is tempting mid-read, write it into NOTES.tmp as a §6 candidate instead.
- If a sanitizer build takes >15 min, run `ctest` in the background and continue reading; re-check results before Task 7.
- Evidence excerpts should be ≤ 20 lines each; reference `file:line` ranges rather than pasting whole functions.
- If the repo has uncommitted user changes (verify with `git status` at Task 1 Step 4), record them in §2 as "pre-existing dirty state" and proceed; do not touch them.
