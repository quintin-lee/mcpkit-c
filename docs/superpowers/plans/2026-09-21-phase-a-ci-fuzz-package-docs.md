# Phase A — CI Fuzz + Packaging Smoke + Docs Count Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers/executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three low-risk, high-ROI production gaps — fuzz harnesses not exercised in CI, a stale test count in the README, and no packaging smoke test — so CI becomes a trustworthy release gate.

**Architecture:** Three independent deliverables, each a single conventional commit:
1. `ci-fuzz` GitHub Actions workflow that configures with `-DMCPKIT_BUILD_FUZZ=ON`, builds the four stdin-fuzz harnesses, and drives each against both valid and invalid inputs generated in-line, asserting clean exit (0) on valid and clean rejection (non-crash) on invalid.
2. Fix the stale "32 unit + acceptance tests" line in `README.md` to reflect the real count.
3. `ci-package` GitHub Actions workflow that builds with CPack, produces a TGZ artifact, extracts it, and runs a smoke `mcpkit-cli inspect` against the packaged layout.

**Tech Stack:** GitHub Actions (ubuntu-latest), CMake/CTest, the existing `mcpkit_core` static lib, CPack (TGZ), the four existing `fuzz_*_stdin.c` harnesses, the existing `mcpkit-cli` tool.

---

## Grounding facts (verified against the tree)

- `tests/CMakeLists.txt` lines 156-167 already register the four `fuzz_*_stdin` executables **only** when `MCPKIT_BUILD_FUZZ=ON`. The deterministic corpus tests (`test_json_fuzz`, `test_schema_fuzz`, `test_message_fuzz`, `test_http_fuzz`) are already plain ctest tests and run in every CI invocation.
- Each `fuzz_*_stdin.c` harness reads all of stdin into a 1 MiB buffer, parses, and returns 0 on clean parse/reject, 1 on internal error. They are built but **never run** by any workflow today.
- `.github/workflows/ci.yml` has a single `build-test` job with a gcc / clang / asan matrix. No `ci-fuzz`, no `ci-package`.
- `README.md:32` says "32 unit + acceptance tests" — the real `add_test` count is **43** (HTTP-gated 3 + socket-gated 2 + fuzz-deterministic 4 are included when the corresponding options are ON).
- `CMakeLists.txt` includes `CPack` (line 132), sets `CPACK_GENERATOR "TGZ"`, and the `package` target is available via `cmake --build build --target package`.
- `mcpkit-cli` `inspect` needs a server binary; the packaged tree contains only the library/headers/pkg-config/cmake-config, **not** the examples or CLI. So the package smoke test must validate the *packaged deliverables* (headers, `.pc`, cmake-config, lib) rather than re-running a full server.

## File structure

| File | Action | Responsibility |
|---|---|---|
| `.github/workflows/ci-fuzz.yml` | Create | Fuzz-harness CI job: build with FUZZ=ON, run all 4 harnesses on valid+invalid inputs, assert no crash. |
| `.github/workflows/ci-package.yml` | Create | Packaging smoke job: `cmake --build build --target package`, extract TGZ, verify packaged layout + `pkg-config` + cmake-config presence. |
| `README.md` | Modify | Fix stale test count line (32 → accurate lower bound). |
| `tests/fuzz/fuzz_run.sh` | Create | Reusable script that drives all four stdin harnesses against generated valid + invalid inputs; exits non-zero only on crash (non-zero from a *clean rejection* is acceptable and is filtered by the "no crash" rule). Used by `ci-fuzz.yml` and locally. |

Design decision: the fuzz runner is a shell script committed under `tests/fuzz/` (not inlined into the workflow) so it is executable locally and in CI with identical behavior. It is **not** a ctest test (the deterministic corpus tests already cover ctest); it is a CI/local driver for the stdin harnesses.

---

## Chunk 1: CI fuzz workflow + runner script

### Task 1: Create the fuzz runner script

**Files:**
- Create: `tests/fuzz/fuzz_run.sh`

- [ ] **Step 1: Write the runner**

`tests/fuzz/fuzz_run.sh` — takes the build dir as `$1`. For each of the four harness binaries, feed a valid input and an invalid (truncated/garbage) input, and assert the process **does not crash** (rc 0 or 1 is acceptable; a signal / rc >= 128 is a crash → failure). Also verify the binaries exist.

```sh
#!/usr/bin/env bash
# Drive the four stdin fuzz harnesses against valid + invalid inputs.
# A harness may exit 1 on a *clean rejection*; that is acceptable.
# Only a crash (killed by a signal, or exit code >= 128) is a failure.
# Usage: fuzz_run.sh <build-dir>
set -u
BUILD_DIR="${1:?usage: fuzz_run.sh <build-dir>}"

fail=0

check_run() {
  # $1 = harness path, $2 = label
  local harness="$1" label="$2"
  if [[ ! -x "$harness" ]]; then
    echo "MISSING harness: $label"
    fail=1
    return
  fi
}

run_input() {
  # $1 = harness, $2 = label, $3 = input string
  local harness="$1" label="$2" input="$3"
  local rc
  printf '%s' "$input" | "$harness"
  rc=$?
  if (( rc >= 128 )); then
    echo "CRASH  $label (rc=$rc)"
    fail=1
  else
    echo "ok     $label (rc=$rc)"
  fi
}

declare -A H=( [json]=fuzz_json_stdin [schema]=fuzz_schema_stdin \
               [message]=fuzz_message_stdin [http]=fuzz_http_stdin )

# Only run the http harness when it was built (HTTP-gated).
if [[ -f "$BUILD_DIR/tests/fuzz_http_stdin" ]]; then
  :
fi

for name in json schema message http; do
  bin="$BUILD_DIR/tests/${H[$name]}"
  [[ -f "$bin" ]] || continue
  echo "== $name =="
  check_run "$bin" "$name"
  # valid input per harness
  case "$name" in
    json)    run_input "$bin" "$name-valid"   '{"a":1,"b":[true,null]}' ;;
    schema)  run_input "$bin" "$name-valid"   '{"type":"string"}' ;;
    message) run_input "$bin" "$name-valid"   '{"jsonrpc":"2.0","id":1,"method":"ping"}' ;;
    http)    run_input "$bin" "$name-valid"   $'GET / HTTP/1.1\r\nHost: x\r\n\r\n' ;;
  esac
  # invalid / truncated input
  case "$name" in
    json)    run_input "$bin" "$name-invalid"  '{"a":' ;;
    schema)  run_input "$bin" "$name-invalid"  '{"type":' ;;
    message) run_input "$bin" "$name-invalid"  '{"jsonrpc":' ;;
    http)    run_input "$bin" "$name-invalid"  'GET / HTT' ;;
  esac
  # empty input (edge)
  run_input "$bin" "$name-empty" ''
done

if (( fail )); then
  echo "FUZZ RUN: FAILED (crash detected)"
  exit 1
fi
echo "FUZZ RUN: all harnesses clean"
exit 0
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/fuzz/fuzz_run.sh`

- [ ] **Step 3: Verify it works locally (with fuzz built)**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_FUZZ=ON
cmake --build build -j
bash tests/fuzz/fuzz_run.sh build
```
Expected: `FUZZ RUN: all harnesses clean`, exit 0.

- [ ] **Step 4: Commit**

```bash
git add tests/fuzz/fuzz_run.sh
git commit -m "test(fuzz): add stdin-harness runner script"
```

### Task 2: Create the `ci-fuzz` workflow

**Files:**
- Create: `.github/workflows/ci-fuzz.yml`

- [ ] **Step 1: Write the workflow**

`.github/workflows/ci-fuzz.yml`:

```yaml
name: CI-Fuzz

on:
  push:
    branches: [main, master]
  pull_request:

jobs:
  fuzz:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Configure (fuzz)
        run: |
          cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Debug \
            -DMCPKIT_BUILD_HTTP=ON \
            -DMCPKIT_BUILD_SOCKET=ON \
            -DMCPKIT_BUILD_FUZZ=ON
      - name: Build
        run: cmake --build build -j
      - name: Run deterministic fuzz corpus (ctest)
        run: ctest --test-dir build --output-on-failure \
               -R 'fuzz'
      - name: Drive stdin harnesses
        run: bash tests/fuzz/fuzz_run.sh build
```

- [ ] **Step 2: Verify the ctest regex matches the corpus tests**

Run locally:
```bash
ctest --test-dir build -R 'fuzz' -N
```
Expected: lists `test_json_fuzz`, `test_schema_fuzz`, `test_message_fuzz`, `test_http_fuzz` (the `-R` regex matches the `add_test` names). Confirm all four appear; if `test_http_fuzz` is missing, the build was configured without `-DMCPKIT_BUILD_HTTP=ON`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci-fuzz.yml
git commit -m "ci: add fuzz-harness workflow (build FUZZ=ON, drive stdin targets)"
```

---

## Chunk 2: Fix stale README test count

### Task 3: Correct the README test-count line

**Files:**
- Modify: `README.md:32`

- [ ] **Step 1: Read the current line**

Run: `sed -n '30,35p' README.md`
Current: `- **Tested** — 32 unit + acceptance tests, deterministic fuzz corpus,`

- [ ] **Step 2: Replace with an accurate lower-bound phrasing**

The exact count drifts as tests are added, so phrase it as a stable lower bound plus what the suite includes, rather than a brittle hardcoded number:

```markdown
- **Tested** — 40+ unit / acceptance / fuzz tests under CTest,
  deterministic fuzz corpus, AddressSanitizer + UBSanitizer build presets.
```

- [ ] **Step 3: Verify**

Run: `grep -n 'Tested' README.md`
Expected: the updated line, no lingering "32 unit".

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: correct stale test count in README"
```

---

## Chunk 3: Packaging smoke workflow

### Task 4: Create the `ci-package` workflow

**Files:**
- Create: `.github/workflows/ci-package.yml`

- [ ] **Step 1: Write the workflow**

`.github/workflows/ci-package.yml`:

```yaml
name: CI-Package

on:
  push:
    branches: [main, master]
  pull_request:

jobs:
  package:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Configure
        run: |
          cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Release \
            -DMCPKIT_BUILD_HTTP=ON \
            -DMCPKIT_BUILD_SOCKET=ON \
            -DMCPKIT_BUILD_TOOLS=ON
      - name: Build + package
        run: |
          cmake --build build -j
          cmake --build build --target package
      - name: Inspect package contents
        run: |
          set -e
          tgz=$(ls build/mcpkit-*.tar.gz)
          echo "artifact: $tgz"
          mkdir pkg && tar -xzf "$tgz" -C pkg
          # expected layout: lib/, include/mcpkit/, pkg-config, cmake-config
          test -f pkg/lib/libmcpkit_core.a
          test -d pkg/include/mcpkit
          test -f pkg/lib/pkgconfig/mcpkit.pc
          test -f pkg/lib/cmake/mcpkit/mcpkit-config.cmake
          # pkg-config resolves against the package
          PKG_CONFIG_PATH=pkg/lib/pkgconfig pkg-config --exists mcpkit
          echo "package layout OK"
      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: mcpkit-package
          path: build/mcpkit-*.tar.gz
```

- [ ] **Step 2: Verify the package target name + layout locally**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_TOOLS=ON
cmake --build build -j
cmake --build build --target package
ls build/mcpkit-*.tar.gz
tar -tzf build/mcpkit-*.tar.gz | head -20
```
Expected: one `mcpkit-0.1.0-Linux.tar.gz` (or platform-suffixed) and the listing shows `lib/libmcpkit_core.a`, `include/mcpkit/...`, `lib/pkgconfig/mcpkit.pc`, `lib/cmake/mcpkit/...`. Adjust the `test -f` paths in the workflow to the actual prefixes if the TGZ nests a top-level dir.

- [ ] **Step 3: Confirm `pkg-config --exists` works from the extracted tree**

Run:
```bash
mkdir /tmp/mkpkg && tar -xzf build/mcpkit-*.tar.gz -C /tmp/mkpkg
PKG_CONFIG_PATH=/tmp/mkpkg/$(ls /tmp/mkpkg)/lib/pkgconfig pkg-config --exists mcpkit && echo PKGCONFIG-OK
```
If the TGZ wraps everything in a top-level `mcpkit-0.1.0/` dir, the workflow's `pkg/` paths must account for it (the Step-2 listing tells you the exact prefix). Fix the paths before committing.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci-package.yml
git commit -m "ci: add packaging smoke-test workflow (CPack TGZ + pkg-config check)"
```

---

## Chunk 4: Final verification + closeout

### Task 5: Full local gate + changelog + cleanup

**Files:**
- Modify: `CHANGELOG.md` (add an Unreleased entry under Added for the two new workflows + the doc fix)

- [ ] **Step 1: Confirm all three commits are on master**

Run: `git log --oneline -5`
Expected: the three new commits (`test(fuzz):...`, `ci: add fuzz-harness...`, `ci: add packaging...`, `docs: correct stale...` — 4 total across the three chunks).

- [ ] **Step 2: Add a CHANGELOG Unreleased bullet**

Under `## [Unreleased]` → `### Added`, add:

```markdown
- CI: `ci-fuzz` workflow builds with `-DMCPKIT_BUILD_FUZZ=ON` and drives
  the four stdin fuzz harnesses against valid/invalid inputs.
- CI: `ci-package` workflow runs the CPack TGZ packaging target and
  smoke-checks the packaged layout (lib, headers, pkg-config, cmake-config).
- `tests/fuzz/fuzz_run.sh`: reusable driver for the stdin fuzz harnesses.
```

- [ ] **Step 3: Run the full local gate (all options ON)**

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_FUZZ=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
bash tests/fuzz/fuzz_run.sh build
```
Expected: all 43 ctest entries pass; `FUZZ RUN: all harnesses clean`.

- [ ] **Step 4: Commit the changelog**

```bash
git add CHANGELOG.md
git commit -m "docs: log CI fuzz + packaging workflows in changelog"
```

- [ ] **Step 5: Clean up build dirs + verify tree clean**

```bash
rm -rf build
git status
```
Expected: only the intended tracked files are committed; no build artifacts staged.

---

## Out of scope (do NOT do in Phase A)

- P0-1 multi-connection socket transport (architecture project, Phase C).
- P0-2/P0-3 TLS + auth (security layer, Phase B).
- P1-1 HTTP keep-alive / 100-continue.
- P1-3 coverage gate (separate workflow, needs a coverage tool decision first).
- P2-2 shared-lib consumer test, P2-3 cross-platform.

These are tracked as later phases; Phase A is strictly the three low-risk CI/doc items above.
