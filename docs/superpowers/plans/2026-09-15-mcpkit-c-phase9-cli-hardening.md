# Phase 9 CLI + Hardening Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver `mcpkit-cli` (`inspect`/`call`/`validate`/`test` over spawned stdio servers) + JSON fuzz corpus + wired sanitizer options + install/CPack packaging, proven by inspecting the P3/P7 example services.

**Architecture:** CLI lives in `tools/mcpkit-cli/` (POSIX `fork`/`exec` + 2 pipes + `fdopen`, tools-only code so Core stays pure C23); it drives the public client API (`client.h` + `stdio.h`) exactly like `examples/client`. Hardening wires the dead `MCPKIT_ENABLE_*`/`MCPKIT_BUILD_FUZZ` options to real compile flags via `cmake/MCPKitSanitizers.cmake`. YAGNI cuts (record in commit): `run` subcommand (bare exec adds nothing — run the binary directly), libFuzzer harness (deterministic corpus only), dynamic plugins, `mcp-codegen`.

**Tech Stack:** C23, CMake + CTest, POSIX spawn (tools only), CPack TGZ.

---

## Chunk 1: Contracts

### `tools/mcpkit-cli/main.c` (single file, `main(argc,argv)` dispatch)

```c
// Usage: mcpkit-cli inspect <server-bin> [--apps]
//   spawn server, initialize, print tools/list + resources/list result JSON.
// Usage: mcpkit-cli call <server-bin> <tool> [args-json]
//   initialize, tools/call, print result JSON. args default "{}".
// Usage: mcpkit-cli validate <file>  (one JSON-RPC message per line)
//   parse + mcp_message_validate with one shared idset; print "<line>: OK" or
//   "<line>: <rpc-code>"; exit nonzero iff any line invalid.
// Usage: mcpkit-cli test <server-bin>
//   smoke: initialize -> ping -> tools/list non-empty; silent on success,
//   "FAIL: <reason>" on stderr + exit 1 otherwise.
```

Spawn contract (static in main.c): `fork`, child `dup2` pipes to stdin/stdout then `execl(server-bin)`; parent `fdopen`s both ends, returns `FILE *in/out + pid`; all failures `MCP_ERR_IO`; `spawn_wait` closes streams + `waitpid`, nonzero exit -> `MCP_ERR_IO`. Errors print `mcpkit-cli: <msg>` to stderr, exit 1. Result JSON printed via `mcp_json_serialize` + `\n`, freed with `mcp_json_free_string`.

### Hardening contracts

- `cmake/MCPKitSanitizers.cmake`: function `mcpkit_apply_sanitizers(target)`; if `MCPKIT_ENABLE_ASAN` append `-fsanitize=address`, UBSAN adds `undefined`, TSAN adds `thread` (ASan+TSan together = fatal error). Applied to `mcpkit_core`, every test/executable, and `mcpkit-cli`. Root `CMakeLists.txt` includes it.
- `tests/fuzz/test_json_fuzz.c` (always built): deterministic corpus — 40 mutated JSON snippets (truncations, bad escapes, lone surrogates, deep nesting to 200, `1e999`, NUL bytes): each must either parse to a DOM that re-serializes and re-parses to equal text, or return NULL with `MCP_ERR_PROTOCOL`/`MCP_ERR_NOMEM`; asserts never crash/hang. `MCPKIT_BUILD_FUZZ` gates a `fuzz_json_stdin.c` harness (reads all stdin, parses, exits 0) for future libFuzzer use.
- Packaging: `cmake_install` rules (`install(TARGETS mcpkit_core mcpkit-cli ...)` + headers directory) + `include(CPack)` TGZ in root CMakeLists, package name `mcpkit`.
- New option `MCPKIT_BUILD_TOOLS` (default ON) gates `tools/`; CLI registered in `tools/CMakeLists.txt`.

### Ownership

Client/transport objects caller-owned, destroyed in reverse (client -> transport -> streams -> wait). Parsed CLI JSON values follow container-owns-on-OK. No new public headers (CLI is a binary, not a library).

---

## Chunk 2: Tasks T1–T5

### Task 1: spawn + `inspect`

**Files:**
- Create: `tools/mcpkit-cli/main.c`, `tools/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add_subdirectory tools), `cmake/MCPKitOptions.cmake` (+BUILD_TOOLS), `tests/CMakeLists.txt` (+2 add_test)

- [ ] **Step 1: Write the CLI skeleton + spawn, prove it compiles**
- [ ] **Step 2: Implement `inspect`** (init -> list tools + resources -> print)
- [ ] **Step 3: Prove anticipated gap** — `add_test(cli_inspect examples/minimal-server)` fails pre-build; wire `tools/CMakeLists.txt` (`add_executable(mcpkit-cli main.c)`, link `mcpkit_core`), root `add_subdirectory`
- [ ] **Step 4: Run** `mcpkit-cli inspect ./build/examples/minimal-server`, expect echo tool listed, RC=0
- [ ] **Step 5: Commit** `feat(tools): add mcpkit-cli spawn and inspect`

### Task 2: `call` + `validate` + `test`

**Files:** Modify `tools/mcpkit-cli/main.c`, `tests/CMakeLists.txt`

- [ ] **Step 1: Implement `call`** (args-json parse, `mcp_client_call_tool`, print result)
- [ ] **Step 2: Implement `validate`** (per-line parse+validate, shared idset, exit code)
- [ ] **Step 3: Implement `test`** (init->ping->list smoke)
- [ ] **Step 4: Verify** — call echo on minimal-server prints text; validate good file RC=0 / bad file RC=1 with `-32600`; test RC=0
- [ ] **Step 5: Commit** `feat(tools): add call, validate and test subcommands`

### Task 3: fuzz + sanitizer wiring + packaging

**Files:**
- Create: `tests/fuzz/test_json_fuzz.c`, `tests/fuzz/fuzz_json_stdin.c`, `cmake/MCPKitSanitizers.cmake`
- Modify: `tests/CMakeLists.txt`, `CMakeLists.txt` (include sanitizers + install + CPack), `cmake/MCPKitOptions.cmake` (doc only, options exist)

- [ ] **Step 1: Write corpus test** (40 cases, no-crash + roundtrip-or-NULL asserts)
- [ ] **Step 2: Prove it runs** — register in tests/CMakeLists, 29/29
- [ ] **Step 3: Wire sanitizers** — `MCPKitSanitizers.cmake`, apply to core+tests+tools+examples; verify `-DMCPKIT_ENABLE_ASAN=ON` actually links libasan (`ldd` check — Phase 2 lesson: never trust the flag blindly)
- [ ] **Step 4: Packaging** — install rules + `include(CPack)`; `cpack -G TGZ` produces tarball (build into temp dir, don't commit artifact)
- [ ] **Step 5: Commit** `feat(hardening): wire sanitizers, fuzz corpus and packaging`

### Task 4: acceptance proof (P3/P7 services)

- [ ] **Step 1:** `mcpkit-cli inspect ./build/examples/minimal-server` → echo tool present
- [ ] **Step 2:** `mcpkit-cli inspect ./build/examples/apps-host` → get_system_status present
- [ ] **Step 3:** `mcpkit-cli call ... get_system_status '{}'` on apps-host → result carries `_meta.ui.resourceUri`
- [ ] **Step 4:** `mcpkit-cli test` on both → RC=0
- [ ] **Step 5: Commit** `test(cli): prove inspect/call against P3 and P7 services` (ctest entries only — no new impl)

### Task 5: gates + CHANGELOG + merge

- [ ] **Step 1:** CHANGELOG Phase 9 entry
- [ ] **Step 2:** gcc 29/29, clang 29/29 from clean configures
- [ ] **Step 3:** `-DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON` 29/29 with ldd libasan proof
- [ ] **Step 4:** `rm -rf build*`, status clean, commit `chore: log phase 9 cli-hardening`
- [ ] **Step 5:** checkout master, merge, post-merge 29/29, delete branch
