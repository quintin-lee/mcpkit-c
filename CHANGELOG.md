# Changelog

Format follows Keep a Changelog. Versions follow SemVer.

## [Unreleased]

### Added

- Phase 0 bootstrap: CMake build, C23 baseline, core
  (`types` / `error` / `result` / `version`) with unit tests.
- Phase 1: pluggable allocator, logger, context, zero-dependency JSON.
- Phase 2: JSON-RPC message envelope, initialize handshake, schema
  builder/validator, three-level validation pipeline with error mapping.
- Phase 3: server registry (tool/resource/prompt), multi-session lifecycle,
  dispatch pipeline with queue and session gating, minimal-server and
  multi-session examples.
- Phase 4: four-method transport interface, stdio line-delimited framing
  with 4MB cap, stdio serve loop over server dispatch, stdio-server example.
- Phase 5: sync client (initialize, tools/resources/prompts) over any transport.
- Phase 6: runtime layer — sync executor, C11 threadpool executor, timer,
  and transport-agnostic event loop; threadpool-server example.
- Phase 7: MCP Apps extension — tool visibility, session permission masks,
  CSP builder, UI resource with lifecycle, apps-host example.
- Phase 8: static plugin registry, buffer-level HTTP/1.1, Streamable HTTP
  serve loop with session map + SSE compat, adapters landing zone,
  http-server example (MCPKIT_BUILD_HTTP=ON).
