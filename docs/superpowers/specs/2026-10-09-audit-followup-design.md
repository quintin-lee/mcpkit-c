# Audit Follow-up (P1+P2) Design

**Goal:** Eliminate the doc drift found in the 2026-10-09 incremental audit and the
silent-truncation P2 in `apps-server`, with no API changes.

**Scope (approved):** CHANGELOG entries + `apps-server` long-name rejection.
Explicitly out of scope: `_meta` non-object type check (separate item),
any public API change.

## Section 1 — CHANGELOG entries (P1)

File: `CHANGELOG.md`, under the current Unreleased/`[0.1.0]` area
(follow the exact heading/format of the existing entries).

Three entries, one line each (keep the file's terse style):

1. `file-server` example: root-jailed `read_file` with offset/limit
   pagination (from `17ddd07`).
2. `mcp_tool_set_meta` / `mcp_tool_meta`: tool-level `_meta` advertised
   in `tools/list` (from `b21a311`).
3. `apps-server` example + `mcp_apps_tool_set_ui` /
   `mcp_apps_result_ui_uri` + `apps_acceptance` test (from `b21a311`,
   `6240096`).

Acceptance: `grep -c "file-server\|tool_set_meta\|apps-server" CHANGELOG.md`
returns ≥ 3 new hits; no other sections touched.

## Section 2 — apps-server long-name rejection (P2)

File: `examples/apps-server/main.c`, `greet_handler`.

Current: `snprintf(greeting, 256, "hello, %s", name)` silently truncates
names longer than ~249 bytes.

Change: pre-compute with `snprintf(NULL, 0, ...)`; if the required size
exceeds the buffer, return `MCP_ERR_INVALID_ARGUMENT` (client-visible
error, same convention as the file's other argument rejections).
Keep the 256-byte stack buffer; do not heap-allocate.

Acceptance: over-long `name` via `mcpkit-cli call` returns an error
(not truncated `"hello, …"`); normal names unchanged; zero warnings;
`ctest` 60/60 in both configs.

## Non-goals

- No `_meta` non-object validation (filed separately).
- No README/module-reference changes (already in sync).
- No commits without a separate user approval.
