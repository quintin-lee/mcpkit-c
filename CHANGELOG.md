# Changelog

Format follows Keep a Changelog. Versions follow SemVer.

## [Unreleased]

### Added

- CI: `ci-fuzz` workflow builds with `-DMCPKIT_BUILD_FUZZ=ON` and drives
  the four stdin fuzz harnesses against valid/invalid/empty inputs via
  `tests/fuzz/fuzz_run.sh`; a crash (rc >= 128) fails the job.
- CI: `ci-package` workflow runs the CPack `package` target, extracts the
  TGZ, and smoke-checks the packaged layout (lib, headers, mcpkit.pc,
  mcpkit-config.cmake) plus `pkg-config --exists mcpkit`.
- `tests/fuzz/fuzz_run.sh`: reusable local/CI driver for the stdin
  fuzz harnesses; exit 0 means all harnesses ran without crashing.

- Transport I/O timeouts: `mcp_transport_set_timeout` / `get_timeout`
  (per-call read/write deadlines in ms, 0 = block forever); socket and
  stdio backends return `MCP_ERR_TIMEOUT` on expiry.
- Bounded dispatch queue: `MCP_QUEUE_MAX_LEN` (1024); push beyond the
  cap fails with `MCP_ERR_NOMEM`.
- Cursor pagination on `tools/list`, `resources/list`, `prompts/list`
  (100 entries per page via `nextCursor` / `params.cursor`);
  `mcp_client_list_tools` follows pages and merges them transparently.
- Graceful shutdown: `mcp_request_shutdown()` /
  `mcp_shutdown_requested()` / `mcp_shutdown_clear()`; `mcp_loop_run`
  and `mcp_stdio_serve` drain in-flight requests and return
  `MCP_ERR_CANCELLED`.
- Observability: `mcp_logger_logf` formatter, dispatch-path log sites,
  atomic `mcp_server_counters()` snapshot, `mcp_server_set_tracer()`
  BEGIN/END request hook.
- Protocol honesty: one shared method table (`method_table.h`)
  between L2 validation and the dispatcher route; the 15 spec-known
  but unimplemented methods (roots, sampling, elicitation, resource
  subscriptions, ...) are no longer silent — a request for one is
  answered `-32601` Method Not Found, while the NOTIFICATION kind of
  `notifications/initialized` is still consumed by the dispatcher.
- Server→client push: `mcp_server_notify_client()` enqueues a
  notification in a per-server outbox; `mcp_server_outbox_pop()`
  drains it. `mcp_stdio_serve` and `mcp_loop_run` flush the outbox
  before each read, so tool handlers can push updates mid-request.
- Example hardening: `socket-server` handlers migrated from the
  error-prone chained-`object_set` pattern to
  `mcp_json_object_set_take`.
- Advanced protocol methods: `logging/setLevel` (drives the per-server log
  floor of the dispatcher's log sites; `warn` accepted as an alias for
  `warning`), `resources/subscribe` / `resources/unsubscribe`
  (per-server deduplicated subscription set, idempotent),
  `resources/templates/list` (honest empty `{templates: []}`); advanced
  notifications (`notifications/cancelled`, `notifications/progress`, the
  four `*_list_changed`, `notifications/resources/updated`,
  `logging/message`) are consumed by `mcp_server_notify` rather than
  answered as requests.
- `mcp_server_request_client`: push a server->client numbered request
  (`roots/list`, `sampling/createMessage`, `elicitation/create`, ...) into
  the per-server outbox; serve loops drain it before each transport read.
- Client provider injection: `mcp_client_set_roots_provider` /
  `mcp_client_set_sample_provider` /
  `mcp_client_set_elicitation_provider` register host callbacks that answer
  the corresponding server->client requests; `mcp_client_handle_server_request`
  routes an incoming `roots/list`, `sampling/createMessage`, or
  `elicitation/create` to the registered provider and builds the response
  (`-32601` when no provider is set or it returns NULL).
  `notifications/roots/list_changed` is now consumed by `mcp_server_notify`
  (moved from the spec-known-unrouted L2 set to the notification table).

### Fixed

- `queue_grow` NOMEM dangling pointer: if the sessions realloc failed
  after the msgs realloc had succeeded, `q->msgs` was left pointing to the
  old (possibly invalidated) allocation and `nm` leaked. Now `q->msgs` is
  committed to the new buffer before the sessions realloc is attempted, so a
  sessions failure leaves the queue in a safe state (no dangling pointer,
  no leak).
- `mcp_client_list_tools` double-free: `mcp_json_object_set_take` destroys
  `acc` on failure; the error path called `mcp_json_destroy(ctx, acc)` a
  second time. Removed the redundant destroy.
- `mcp_server_request_client` ID namespace collision: `next_server_id`
  previously started at `1.0`, identical to the client's `next_id`, making
  it ambiguous which in-flight request a response with `id=1.0` belonged
  to in a dual-role process. Server-originated IDs now start at `1000.0`.

## [0.1.0] - 2026-09-20

### Added

- Socket TCP transport (line-framed, server/client modes) with
  socket-server example (`MCPKIT_BUILD_SOCKET`, default ON).
- `completion/list` + `completion/complete` (provider registry,
  prefix match, client API) with prompt-server example.
- GitHub Actions CI (gcc / clang / ASan+UBSan triple gate),
  HTTP + schema + message fuzz targets with deterministic corpora,
  shared-library option with pkg-config and CMake package files.
- `mcp_json_object_set_take`: failure-consumes-value attach variant
  unifying all multi-step object-build paths.
- Version single-sourced from `VERSION` via generated header macro;
  `mcpkit-cli` reports the library version.
- Formal Doxygen contracts on all public headers + `Doxyfile`
  (generated `docs/api`, 205 pages, zero warnings); module-level
  `@file` blocks sit at the top of every header.
- NDEBUG-independent `CHECK` macro rolled out to all test files.
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
- Phase 9: mcpkit-cli (inspect/call/validate/test), deterministic JSON
  fuzz corpus, CMake sanitizer presets, CPack TGZ packaging, CLI
  acceptance test against P3/P7 example services.

### Fixed

- Schema `integer` check accepted out-of-int64-range finite doubles
  (now symmetric ±2^53-1 bounds with regression tests).
- Plugin registry merged to single-pass first-fit; HTTP header-end
  off-by-one at exact buffer end.
- Double-free class in chained `object_set` paths (client complete,
  `result_with_ui`, `tool_error_result`, `ui_read`): per-step checks,
  then unified via `object_set_take`.
- Completion provider received a borrowed `argument` pointer — now
  cloned before the callback.
- Protocol builders destroyed caller-owned values on failure,
  contradicting the caller-retains contract.
- Global plugin registry is now mutex-guarded (thread-safe).
- I/O hardening: EINTR retry on `accept()`/stdio reads,
  `MSG_NOSIGNAL` on socket sends, `SIGPIPE` ignored in all example
  and CLI binaries (closed peer → EPIPE/`MCP_ERR_IO`, no kill).
- `test_executor` counters are now atomic (killed the 32-task
  threadpool count flake).
