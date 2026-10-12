# Audit Follow-up (P1+P2) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 3 CHANGELOG entries and reject over-long `greet` names in apps-server.

**Architecture:** Docs-only edit plus a pre-size check before `snprintf` in the
example handler; no API change, verification via existing ctest suites.

**Tech Stack:** C23, CMake/ctest, Keep-a-Changelog format.

---

## Chunk 1: CHANGELOG + apps-server fix

### Task 1: CHANGELOG entries

**Files:**
- Modify: `CHANGELOG.md` (`## [Unreleased]` → `### Added`, append bullets)

- [ ] **Step 1: Append three bullets under `### Added`**

```markdown
- File-server example: added root-jailed `read_file` tool with offset/limit pagination over stdio.
- Tool-level `_meta`: added `mcp_tool_set_meta()` / `mcp_tool_meta()`, advertised in `tools/list`.
- Apps-server example: added stdio demo with tool-level `_meta.ui`, result `_meta.ui` helpers (`mcp_apps_tool_set_ui()`, `mcp_apps_result_ui_uri()`), and `apps_acceptance` test.
```

- [ ] **Step 2: Verify the hits**

Run: `grep -c "file-server\|tool_set_meta\|apps-server" CHANGELOG.md`
Expected: count increases by ≥ 3 vs before.

- [ ] **Step 3: Commit (only with separate user approval)**

```bash
git add CHANGELOG.md docs/superpowers/specs/2026-10-09-audit-followup-design.md docs/superpowers/plans/2026-10-09-audit-followup.md
git commit -m "docs: changelog the file-server/apps/meta gaps"
```

### Task 2: apps-server long-name rejection

**Files:**
- Modify: `examples/apps-server/main.c:38-39`
- Test: manual `mcpkit-cli call` + full `ctest`

- [ ] **Step 1: Confirm current truncation behavior**

Run: `./build/tools/mcpkit-cli call ./build/examples/apps-server greet '{"name":"<300 x chars>"}'`
Expected: returns `"hello, <truncated>"` (bug demo).

- [ ] **Step 2: Add pre-size check**

```c
    char greeting[256];
    int need =
        snprintf(NULL, 0, "hello, %s", name);
    if (need < 0 || (size_t)need >= sizeof(greeting)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    snprintf(greeting, sizeof(greeting), "hello, %s", name);
```

- [ ] **Step 3: Verify fix + no regression**

Run: same long-name call → error (not truncated text); normal call
`'{"name":"qi"}'` → `"hello, qi"`.
Run: `cmake --build build` (zero warnings), `ctest --test-dir build`,
`ctest --test-dir build-http` → 60/60 both.

- [ ] **Step 4: Commit (only with separate user approval)**

```bash
git add examples/apps-server/main.c
git commit -m "fix(examples): reject over-long greet names in apps-server"
```
