# Changelog

Format follows Keep a Changelog. Versions follow SemVer.

## [Unreleased]

### Added

- Tasks Extension (`io.modelcontextprotocol/tasks`, SEP-2663):
  - Protocol data structures in `mcpkit/protocol/tasks.h`: `mcp_task_status_t` (`working`, `input_required`, `completed`, `failed`, `cancelled`), `mcp_task_desc_t`, and task manager `mcp_task_mgr_t`.
  - Server integration: `mcp_server_enable_tasks()`, `mcp_server_get_task_mgr()`, dispatcher routes `tasks/get`, `tasks/update`, `tasks/cancel`, and dynamic capability advertising under `capabilities.extensions["io.modelcontextprotocol/tasks"]` in `server/discover`.
  - Client helper APIs: `mcp_client_tasks_get()`, `mcp_client_tasks_update()`, and `mcp_client_tasks_cancel()`.
  - L3 validation requiring string `taskId` parameter.
- Skills Extension (`io.modelcontextprotocol/skills`, SEP-2640):
  - Protocol data structures in `mcpkit/protocol/skills.h`: `mcp_skill_resource_t`, `mcp_skill_t`, and `mcp_skill_registry_t` for managing agent tool and workflow skill manifests.
  - Server integration: `mcp_server_enable_skills()`, `mcp_server_get_skill_registry()`, dispatcher routes `skills/list` (paginated with `CacheableResult` metadata) and `skills/get` (lookup by name or URI), and dynamic capability advertising under `capabilities.extensions["io.modelcontextprotocol/skills"]` in `server/discover`.
  - Client helper APIs: `mcp_client_skills_list()` and `mcp_client_skills_get()`.
  - L3 validation requiring string `name` or `uri`.
- W3C Trace Context Propagation (SEP-414):
  - `mcp_trace_extract_from_meta()` and `mcp_trace_inject_into_meta()` in `mcpkit/core/trace.h` for extracting and injecting W3C `traceparent`, `tracestate`, and `baggage` fields in `_meta` JSON objects.
- CI: `ci-fuzz` workflow builds with `-DMCPKIT_BUILD_FUZZ=ON` and drives
  the four stdin fuzz harnesses against valid/invalid/empty inputs via
  `tests/fuzz/fuzz_run.sh`; a crash (rc >= 128) fails the job.
- CI: `ci-package` workflow runs the CPack `package` target, extracts the
  TGZ, and smoke-checks the packaged layout (lib, headers, mcpkit.pc,
  mcpkit-config.cmake) plus `pkg-config --exists mcpkit`.
- `tests/fuzz/fuzz_run.sh`: reusable local/CI driver for the stdin
  fuzz harnesses; exit 0 means all harnesses ran without crashing.
- `mcp_tls_transport_create`: OpenSSL-backed TLS transport adapter
  (behind `MCPKIT_BUILD_TLS`, default OFF).  The default zero-dep build
  is unchanged; when enabled, links OpenSSL PRIVATE to `mcpkit_core`.
  Both client and server handshakes are deferred to
  `mcp_transport_start()`; peer certificate verification is disabled
  by default (development / loopback use only).
- `mcp_http_serve_with_auth()`: optional Bearer-token authentication for
  Streamable HTTP POST traffic, via a host-supplied validation callback;
  401 + `WWW-Authenticate: Bearer` on missing/invalid token.  `auth_fn==NULL`
  preserves the existing `mcp_http_serve` behavior exactly.
- Multi Round-Trip Requests (MRTR, MCP 2026-07-28 spec):
  - Protocol helpers in `mcpkit/protocol/mrtr.h`: `mcp_mrtr_elicit_request_new`,
    `mcp_mrtr_result_input_required_new`, `mcp_mrtr_is_input_required`,
    `mcp_mrtr_get_request_state`, `mcp_mrtr_get_input_requests`,
    `mcp_mrtr_input_response_new`, and `mcp_elicit_action_t`.
  - Server-side V2 tools: `mcp_tool_call_ctx_t`, `mcp_tool_handler_v2_fn`,
    `mcp_tool_new_v2()`. Dispatcher `route_tools_call` extracts `inputResponses`
    and `requestState` to support multi-turn resumption; `InputRequiredResult`
    is returned to the client without `complete` decoration.
  - Client-side auto-retry engine: `mcp_client_call_tool_mrtr()` drives up to
    5 round-trips automatically via host-injected `mcp_client_mrtr_elicit_fn`
    callback registered with `mcp_client_set_mrtr_elicit_handler()`.
  - Server-side secure `requestState` engine (Phase 2): `mcp_mrtr_state_pack()`,
    `mcp_mrtr_state_unpack()`, `mcp_mrtr_state_pack_raw()`, `mcp_mrtr_state_unpack_raw()`,
    and `mcp_mrtr_state_free()`. Provides zero-dependency C99 HMAC-SHA256 tamper-proofing,
    URL-safe Base64 token packing, and TTL expiration enforcement for distributed or
    stateless server resumption.
  - Example server `examples/mrtr-server` demonstrating interactive 2FA
    transaction authorization with secure HMAC-SHA256 signed `requestState` tokens.
  - Fix in `src/transport/socket.c`: added carry buffer to `sock_recv` so that
    trailing pipelined bytes across newline boundaries are never dropped.
- Protocol statelessness (MCP 2026-07-28 spec):
  - `mcp_message_meta` accessor, `mcp_result_inject_result_type` and
    `mcp_result_inject_meta` decorators, `mcp_server_set_list_cache` /
    `mcp_server_set_response_meta` host config setters, `mcp_session_client_meta`
    accessor, and `server/discover` RPC route.
  - All successful responses now carry `resultType: "complete"`; list responses
    additionally carry `ttlMs` / `cacheScope` when configured.
  - Dispatcher allows `server/discover` pre-flight probes and requests carrying
    `_meta` to bypass the session uninitialized gate without requiring prior
    handshake (SEP-2575).
  - Streamable HTTP transport (`src/transport/streamable_http.c`) supports
    stateless POST requests (with `_meta` or `server/discover`) without
    requiring or returning `Mcp-Session-Id` header (SEP-2567).
  - Streamable HTTP Header Mirror Validation (SEP-2243): incoming HTTP POST
    requests are validated against `MCP-Protocol-Version`, `Mcp-Method`, and
    `Mcp-Name`. Missing required headers or mismatches return standard
    JSON-RPC error responses with HTTP 400 (`MCP_RPC_HEADER_MISMATCH` -32020 or
    `MCP_RPC_UNSUPPORTED_PROTOCOL_VERSION` -32022).
  - Subscriptions Stream (`subscriptions/listen`) Protocol (SEP-2575):
    added `subscriptions/listen` method and `notifications/subscriptions/acknowledged`
    notification to method tables and dispatcher. Registers event filters
    (`toolsListChanged`, `promptsListChanged`, `resourcesListChanged`,
    `resourceSubscriptions`) and returns subscription acknowledgment carrying
    the subscription ID under `_meta["io.modelcontextprotocol/subscriptionId"]`.
    Tracks subscription cancellations via `notifications/cancelled`.
  - Subscription Notification Delivery & Filter Enforcement (SEP-2575):
    added `mcp_session_has_active_subscription()`, `mcp_session_is_subscribed_to_notification()`,
    `mcp_session_build_notification()`, and `mcp_server_session_notify()` to filter
    and automatically attach `_meta["io.modelcontextprotocol/subscriptionId"]` on
    streams with active subscriptions. `mcp_server_notify_client()` transparently
    routes through active session filters.
  - Graceful Closure for Subscriptions (SEP-2575):
    added `mcp_session_build_subscription_closure()` and `mcp_server_session_close_subscription()`
    to emit the final JSON-RPC response completing the original `subscriptions/listen` request
    (`resultType: complete` and `io.modelcontextprotocol/subscriptionId`).
    Added `mcp_response_ok_string_id_new()` and `mcp_response_ok_number_id_new()` builders
    in `mcpkit/protocol/message.h`.
  - Client-Side Subscriptions & Message Stream APIs:
    added `mcp_client_subscriptions_listen()`, `mcp_client_cancel_subscription()`, and
    `mcp_client_recv_message()` in `mcpkit/client/client.h`.
  - Client-Side Streamable HTTP Header Mirroring (SEP-2243):
    added `mcp_http_request_new()`, `mcp_http_request_set_header()`,
    `mcp_http_request_set_body()`, `mcp_http_request_serialize()`, and
    `mcp_http_request_set_mcp_metadata()` in `mcpkit/transport/http.h`.
    Added `MCP_PROTOCOL_VERSION_2026_07_28` and `MCP_PROTOCOL_VERSION_2025_11_25` constants
    in `mcpkit/protocol/initialize.h`.
  - CacheableResult for Resources & Request-level Log Level (SEP-2549, SEP-2575, SEP-2577):
    `resources/read` and `resources/templates/list` results are decorated with
    `ttlMs` and `cacheScope` when list cache is configured. Requests carrying
    `_meta["io.modelcontextprotocol/logLevel"]` dynamically adjust the server
    log floor for that request.
  - Standard error codes: `MCP_RPC_HEADER_MISMATCH` (-32020),
    `MCP_RPC_MISSING_REQUIRED_CLIENT_CAPABILITY` (-32021),
    `MCP_RPC_UNSUPPORTED_PROTOCOL_VERSION` (-32022).

- `mcp_socket_serve`: multi-connection TCP accept-loop; each accepted
  connection is served on a threadpool worker.  Blocks until
  `mcp_shutdown_requested()`; drains in-flight connections before
  returning.  `pool=NULL` runs connections inline (sequential).
- Concurrency hardening: `log_floor` is now an `atomic_int` and
  `subscribed_uris` is guarded by `pthread_mutex_t subscribed_lock` so
  that concurrent `mcp_server_dispatch` calls from multiple threadpool
  workers cannot race on the advanced-method server state.

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
- `mcp_stdio_serve_with_client` / `mcp_loop_run_with_client`: serve-loop
  variants that route `roots/list`, `sampling/createMessage`, and
  `elicitation/create` REQUEST messages to a host-supplied client; the
  client's registered providers answer them and the response is sent
  over the transport.  All other methods go through normal server
  dispatch.  Client = NULL gives identical behaviour to the base
  functions.

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
