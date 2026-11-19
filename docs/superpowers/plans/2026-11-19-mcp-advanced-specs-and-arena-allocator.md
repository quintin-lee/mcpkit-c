# MCP Advanced Protocol Specifications & Request-Scoped Arena Allocator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Model Context Protocol advanced features (Streamable HTTP event resumption with per-session ring buffer replay, fine-grained subscription routing, and HTTP conditional ETag caching) alongside a request-scoped monotonic arena allocator (`mcp_arena_t`) and direct streaming JSON-RPC serializer.

**Architecture:** A chunked bump arena (`mcp_arena_t`) with 8-byte chunk allocation headers bridges into `mcp_allocator_t` for zero-malloc request parsing and dispatch, while persistent session/server state uses the upstream allocator. In the transport layer, an event ring buffer attached to HTTP sessions tracks monotonic SSE IDs, enabling seamless event replay on `Last-Event-ID` reconnection and HTTP 410 on expiration. An observer sink pattern bridges server-side resource/tool updates to transport connections, and ETag hashing slashes redundant polling bandwidth.

**Tech Stack:** C99, POSIX threads, CMake, cJSON, RFC 6570, RFC 7232 (Conditional Requests), Model Context Protocol Specs (SEP-1577, SEP-1699, SEP-2575).

**Spec:** `docs/superpowers/specs/2026-11-19-mcp-advanced-specs-and-arena-allocator-design.md`

## Global Constraints
- Target standard: C99 with POSIX.1-2008 compatibility.
- Zero compiler warnings under `-Wall -Wextra -Wpedantic -Werror`.
- Double-gate verification: All unit and integration tests must pass 100% under both standard Release and ASan+UBSan builds.
- Zero memory leaks, zero heap-use-after-free, zero undefined behaviors.
- Backward compatibility: Existing `mcp_server_dispatch` and `mcp_async_server` public APIs must remain functional.

## Review Focus
1. **Arena chunk expansion across size boundaries**: Allocating larger than default chunk size (32KB) must dynamically allocate dedicated oversized chunks without overflowing or corrupting subsequent small bump allocations.
2. **Realloc in-place top bump vs relocation**: Reallocating the most recent arena allocation must expand in-place if capacity permits; reallocating older allocations must relocate and preserve prior bytes via `memcpy`.
3. **Session persistent state isolation from request arena**: Client info stored during `initialize` or URIs stored during `resources/subscribe` must remain fully valid after `mcp_arena_reset` is called at the end of the request.
4. **SSE ring buffer rollover and eviction**: When ring buffer fills past capacity (128 items), oldest items must be cleanly evicted and freed, `oldest_id` must advance, and reconnection requests below `oldest_id` must return HTTP 410 Gone.
5. **Conditional ETag pagination bypass**: Requests for `tools/list` carrying a `cursor` parameter must not match 304 Not Modified even if the schema hash matches, returning the correct paginated slice.

---

### Task 1: Request-Scoped Arena Allocator (`mcp_arena_t`)

**Files:**
- Create: `include/mcpkit/core/arena.h`
- Create: `src/core/arena.c`
- Test: `tests/unit/test_arena.c`
- Modify: `src/CMakeLists.txt:1-40`
- Modify: `tests/CMakeLists.txt:30-80`

**Interfaces:**
- Produces:
  ```c
  typedef struct mcp_arena mcp_arena_t;

  mcp_arena_t *mcp_arena_create(const mcp_allocator_t *upstream, size_t default_chunk_size);
  void *mcp_arena_alloc(mcp_arena_t *arena, size_t size);
  void *mcp_arena_alloc_aligned(mcp_arena_t *arena, size_t size, size_t alignment);
  void *mcp_arena_realloc(mcp_arena_t *arena, void *ptr, size_t new_size);
  char *mcp_arena_strdup(mcp_arena_t *arena, const char *s);
  void mcp_arena_reset(mcp_arena_t *arena);
  void mcp_arena_destroy(mcp_arena_t *arena);
  mcp_allocator_t mcp_arena_as_allocator(mcp_arena_t *arena);
  ```

- [ ] **Step 1: Declare `mcp_arena_t` API in `include/mcpkit/core/arena.h`**
  Define `mcp_arena_t` opaque struct and function prototypes in `include/mcpkit/core/arena.h`.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_arena.c` and register in `tests/CMakeLists.txt`**
  Cover:
  - Basic bump allocation and alignment (8, 16, 64 bytes).
  - String duplication (`mcp_arena_strdup`).
  - In-place top realloc vs relocated non-top realloc.
  - Multi-chunk chaining when allocation exceeds default chunk size.
  - Reset and reuse: verify memory is reused without increasing total chunks.
  - Bridge to `mcp_allocator_t`: allocate via `allocator.malloc_fn`, realloc via `allocator.realloc_fn`, free (no-op).

- [ ] **Step 3: Run test to verify it fails to build/link**
  Run: `cmake -B build -DMCPKIT_BUILD_TESTS=ON && cmake --build build --target test_arena`
  Expected: FAIL (missing `src/core/arena.c` implementation).

- [ ] **Step 4: Implement `src/core/arena.c` and add to `src/CMakeLists.txt`**
  Implement chunked bump allocation with 8-byte allocation header:
  ```c
  typedef struct {
      size_t size;
  } mcp_arena_alloc_hdr_t;
  ```
  Implement `mcp_arena_alloc_aligned`, `mcp_arena_realloc` (supporting in-place extension if top-of-bump, else new alloc + memcpy), `mcp_arena_reset`, and `mcp_arena_as_allocator`.

- [ ] **Step 5: Run tests and verify they pass**
  Run: `cmake --build build --target test_arena && ctest --test-dir build -R test_arena --output-on-failure`
  Expected: PASS (100%).

- [ ] **Step 6: Commit**
  ```bash
  git add include/mcpkit/core/arena.h src/core/arena.c tests/unit/test_arena.c src/CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(core): implement chunked request arena allocator with realloc support"
  ```

---

### Task 2: Direct Streaming JSON-RPC Serializer (`mcp_json_stream_t`)

**Files:**
- Create: `include/mcpkit/protocol/json_stream.h`
- Create: `src/protocol/json_stream.c`
- Test: `tests/unit/test_json_stream.c`
- Modify: `src/CMakeLists.txt:20-60`
- Modify: `tests/CMakeLists.txt:50-90`

**Interfaces:**
- Consumes: `mcp_arena_t` from Task 1.
- Produces:
  ```c
  typedef struct {
      char *buf;
      size_t len;
      size_t cap;
      mcp_arena_t *arena;
      const mcp_allocator_t *allocator;
  } mcp_json_stream_t;

  void mcp_json_stream_init_arena(mcp_json_stream_t *s, mcp_arena_t *arena);
  void mcp_json_stream_init_allocator(mcp_json_stream_t *s, const mcp_allocator_t *allocator);
  void mcp_json_stream_write_raw(mcp_json_stream_t *s, const char *str, size_t len);
  void mcp_json_stream_write_string(mcp_json_stream_t *s, const char *str);
  void mcp_json_stream_write_int64(mcp_json_stream_t *s, int64_t val);
  void mcp_json_stream_write_uint64(mcp_json_stream_t *s, uint64_t val);

  mcp_status_t mcp_serialize_response_stream(mcp_json_stream_t *stream,
                                             const mcp_json_rpc_id_t *id,
                                             const char *result_json_raw);

  mcp_status_t mcp_serialize_error_stream(mcp_json_stream_t *stream,
                                          const mcp_json_rpc_id_t *id,
                                          int code,
                                          const char *message);
  ```

- [ ] **Step 1: Declare `mcp_json_stream_t` in `include/mcpkit/protocol/json_stream.h`**
  Declare streaming buffer writer and serialization helpers.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_json_stream.c`**
  Cover:
  - String escaping (`"hello\n\"world\""`).
  - Integer and unsigned integer formatting.
  - JSON-RPC 2.0 response envelope with string ID, integer ID, and null ID.
  - JSON-RPC 2.0 error envelope with standard error codes and message escaping.

- [ ] **Step 3: Run test to verify it fails to build/link**
  Run: `cmake --build build --target test_json_stream`
  Expected: FAIL.

- [ ] **Step 4: Implement `src/protocol/json_stream.c`**
  Implement dynamic buffer growth (via `mcp_arena_alloc` or `allocator->realloc_fn`), fast string escaping, and envelope writers.

- [ ] **Step 5: Run tests and verify they pass**
  Run: `cmake --build build --target test_json_stream && ctest --test-dir build -R test_json_stream --output-on-failure`
  Expected: PASS (100%).

- [ ] **Step 6: Commit**
  ```bash
  git add include/mcpkit/protocol/json_stream.h src/protocol/json_stream.c tests/unit/test_json_stream.c src/CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(protocol): implement direct streaming JSON-RPC serializer"
  ```

---

### Task 3: Streamable HTTP Event Ring Buffer & Resumption (SEP-1577 / SEP-1699)

**Files:**
- Create: `include/mcpkit/transport/ring_buffer.h`
- Create: `src/transport/ring_buffer.c`
- Modify: `include/mcpkit/transport/async_server.h:30-100`
- Modify: `src/transport/async_server.c:660-740, 1370-1410`
- Test: `tests/unit/test_streamable_resumption.c`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```c
  typedef struct mcp_event_ring_buffer mcp_event_ring_buffer_t;

  mcp_event_ring_buffer_t *mcp_event_ring_buffer_create(const mcp_allocator_t *alloc, size_t capacity);
  void mcp_event_ring_buffer_destroy(mcp_event_ring_buffer_t *rb);
  mcp_status_t mcp_event_ring_buffer_push(mcp_event_ring_buffer_t *rb, const char *data, size_t len, uint64_t *assigned_id);
  mcp_status_t mcp_event_ring_buffer_replay(mcp_event_ring_buffer_t *rb, uint64_t last_event_id,
                                            void (*replay_fn)(uint64_t id, const char *data, size_t len, void *ud),
                                            void *ud);
  uint64_t mcp_event_ring_buffer_oldest_id(mcp_event_ring_buffer_t *rb);
  uint64_t mcp_event_ring_buffer_latest_id(mcp_event_ring_buffer_t *rb);
  ```

- [ ] **Step 1: Declare `mcp_event_ring_buffer_t` in `include/mcpkit/transport/ring_buffer.h`**
  Declare ring buffer creation, destruction, push, replay, and status queries.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_streamable_resumption.c`**
  Cover:
  - Ring buffer push assigning incremental `id: 1, 2, ...`.
  - Replay after client reconnects with `Last-Event-ID: 1` -> replays event 2.
  - Buffer overflow eviction advancing `oldest_id`.
  - Replay query for evicted ID returning `MCP_ERR_EXPIRED` (maps to HTTP 410 Gone).
  - Server integration: `mcp_async_server_push_sse` returning `MCP_OK` when client disconnected, and subsequent GET with `Last-Event-ID` replaying missed messages.

- [ ] **Step 3: Run test to verify it fails**
  Run: `cmake --build build --target test_streamable_resumption`
  Expected: FAIL.

- [ ] **Step 4: Implement `src/transport/ring_buffer.c`**
  Implement thread-safe circular array storing `event_id`, copied payload, and length. On eviction, free the oldest data and increment `oldest_id`.

- [ ] **Step 5: Integrate Ring Buffer into `async_server.c`**
  - Add `mcp_event_ring_buffer_t *ring_buf` to `session_entry_t`.
  - In `mcp_async_server_push_sse`, push to `ring_buf` with monotonic ID. If `sse_conn == NULL`, return `MCP_OK`.
  - In GET SSE handler (`src/transport/async_server.c`), extract `Last-Event-ID`. If `< oldest_id - 1`, return HTTP `410 Gone`. If valid, send 200 OK headers and replay missed events through outbox before taking new events.

- [ ] **Step 6: Run tests and verify they pass**
  Run: `cmake --build build --target test_streamable_resumption && ctest --test-dir build -R test_streamable_resumption --output-on-failure`
  Expected: PASS (100%).

- [ ] **Step 7: Commit**
  ```bash
  git add include/mcpkit/transport/ring_buffer.h src/transport/ring_buffer.c src/transport/async_server.c include/mcpkit/transport/async_server.h tests/unit/test_streamable_resumption.c src/CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(transport): implement Streamable HTTP SSE event ring buffer and Last-Event-ID resumption"
  ```

---

### Task 4: Fine-Grained Subscription Engine & Notification Sink

**Files:**
- Modify: `include/mcpkit/server/session.h:80-160`
- Modify: `include/mcpkit/server/server.h:280-350`
- Modify: `src/server/session.c:100-240`
- Modify: `src/server/server.c:300-450`
- Modify: `src/server/routes.c:1060-1200`
- Modify: `src/transport/async_server.c:680-720`
- Test: `tests/unit/test_subscription_engine.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```c
  typedef mcp_status_t (*mcp_session_sink_fn)(mcp_context_t *ctx,
                                              mcp_session_t *session,
                                              const char *payload,
                                              size_t len,
                                              void *userdata);

  void mcp_session_set_sink(mcp_session_t *session, mcp_session_sink_fn sink, void *userdata);

  mcp_status_t mcp_server_notify_resource_updated(mcp_context_t *ctx, mcp_server_t *server, const char *uri);
  mcp_status_t mcp_server_notify_tools_changed(mcp_context_t *ctx, mcp_server_t *server);
  mcp_status_t mcp_server_notify_prompts_changed(mcp_context_t *ctx, mcp_server_t *server);
  ```

- [ ] **Step 1: Declare notification sink and server dispatch prototypes**
  Add declarations to `include/mcpkit/server/session.h` and `include/mcpkit/server/server.h`.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_subscription_engine.c`**
  Cover:
  - Route `resources/subscribe` adds URI to session's direct subscriptions.
  - Route `resources/unsubscribe` removes URI.
  - `mcp_server_notify_resource_updated` sends `notifications/resources/updated` only to sessions subscribed to that specific URI.
  - Coexistence with SEP-2575 `subscriptions/listen`: verify that standard URI subscriptions do NOT inject spurious `subscriptionId`, while `subscriptions/listen` streams still inject `_meta.subscriptionId`.
  - Notification sink delivery invoked asynchronously.

- [ ] **Step 3: Run test to verify it fails**
  Run: `cmake --build build --target test_subscription_engine`
  Expected: FAIL.

- [ ] **Step 4: Implement subscription registry and notification dispatching**
  - Add `direct_subscriptions` set (`mcp_uri_set_t`) to `struct mcp_session`. Ensure it allocates via `session->ctx` (persistent allocator).
  - Implement handlers for `resources/subscribe` and `resources/unsubscribe` in `src/server/routes.c`.
  - Implement `mcp_session_set_sink` in `src/server/session.c`.
  - Implement `mcp_server_notify_resource_updated`, `mcp_server_notify_tools_changed`, and `mcp_server_notify_prompts_changed` in `src/server/server.c`.
  - Connect `mcp_session_set_sink` in `src/transport/async_server.c` to route server notifications into the session's SSE stream.

- [ ] **Step 5: Run tests and verify they pass**
  Run: `cmake --build build --target test_subscription_engine && ctest --test-dir build -R test_subscription_engine --output-on-failure`
  Expected: PASS (100%).

- [ ] **Step 6: Commit**
  ```bash
  git add include/mcpkit/server/session.h include/mcpkit/server/server.h src/server/session.c src/server/server.c src/server/routes.c src/transport/async_server.c tests/unit/test_subscription_engine.c tests/CMakeLists.txt
  git commit -m "feat(server): implement resources/subscribe and targeted notification routing"
  ```

---

### Task 5: HTTP Conditional Caching (`ETag` / `If-None-Match` 304)

**Files:**
- Modify: `include/mcpkit/server/server.h:180-220`
- Modify: `src/server/server.c:120-180`
- Modify: `src/transport/async_server.c:800-920`
- Test: `tests/unit/test_http_etag.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```c
  uint64_t mcp_server_get_tools_revision(const mcp_server_t *server, mcp_tool_visibility_t vis);
  uint64_t mcp_server_get_resources_revision(const mcp_server_t *server);
  uint64_t mcp_server_get_prompts_revision(const mcp_server_t *server);
  ```

- [ ] **Step 1: Declare revision queries in `include/mcpkit/server/server.h`**
  Add revision hash queries for tools, resources, and prompts.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_http_etag.c`**
  Cover:
  - Tool registration updates tools revision hash.
  - GET/POST `tools/list` on `async_server` produces `ETag: "\"<hex>\""` in HTTP header.
  - Subsequent request with matching `If-None-Match` returns HTTP `304 Not Modified` with 0 content length.
  - Adding a new tool changes ETag, so subsequent request returns HTTP `200 OK` with new ETag.
  - Pagination check: request with `params.cursor` bypasses 304 and returns full 200 OK page.

- [ ] **Step 3: Run test to verify it fails**
  Run: `cmake --build build --target test_http_etag`
  Expected: FAIL.

- [ ] **Step 4: Implement revision hashing and 304 short-circuit**
  - Implement FNV-1a 64-bit hashing on tool/resource/prompt addition/removal in `src/server/server.c`.
  - In `src/transport/async_server.c`, during worker task processing for `tools/list`, `resources/list`, and `prompts/list`, inspect `req->if_none_match`. If matched and `cursor == NULL`, emit HTTP 304 response directly.

- [ ] **Step 5: Run tests and verify they pass**
  Run: `cmake --build build --target test_http_etag && ctest --test-dir build -R test_http_etag --output-on-failure`
  Expected: PASS (100%).

- [ ] **Step 6: Commit**
  ```bash
  git add include/mcpkit/server/server.h src/server/server.c src/transport/async_server.c tests/unit/test_http_etag.c tests/CMakeLists.txt
  git commit -m "feat(http): implement ETag hashing and 304 Not Modified conditional caching"
  ```

---

### Task 6: Request-Scoped Arena Integration into `async_server` & Lifetime Auditing

**Files:**
- Modify: `src/server/routes.c:1-300`
- Modify: `src/transport/async_server.c:300-450, 1000-1150`
- Test: `tests/unit/test_async_arena_lifecycle.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `mcp_arena_t` from Task 1, `mcp_json_stream_t` from Task 2.

- [ ] **Step 1: Audit `src/server/routes.c` for persistent allocation boundaries**
  In `route_initialize` and `route_resources_subscribe`: ensure any copied strings (e.g. `client_info.name`, `client_info.version`, subscription URIs) are allocated with `session->ctx` (persistent allocator) rather than transient `ctx`.

- [ ] **Step 2: Add failing lifecycle stress test in `tests/unit/test_async_arena_lifecycle.c`**
  Cover:
  - Worker thread runs 1,000 rapid requests through `mcp_server_dispatch` with an arena context that is reset after each request.
  - Verify `session->client_info` remains valid and uncorrupted after 1,000 resets.
  - Verify subscription URIs remain valid and notifications continue to route properly after 1,000 resets.
  - Memory leak check: zero leaks reported by LeakSanitizer.

- [ ] **Step 3: Run test to verify it fails**
  Run: `cmake --build build --target test_async_arena_lifecycle`
  Expected: FAIL (or heap use-after-free before audit).

- [ ] **Step 4: Integrate `mcp_arena_t` into `async_server.c` worker threads**
  - Allocate `mcp_arena_t *arena` on `worker_t` in `init_workers`.
  - In `worker_dispatch_fn`, create `req_ctx` backed by `mcp_arena_as_allocator(worker->arena)`.
  - After dispatch and serialization, destroy `req_ctx` and call `mcp_arena_reset(worker->arena)`.
  - Destroy `worker->arena` in `stop_workers`.

- [ ] **Step 5: Run tests and verify they pass**
  Run: `cmake --build build --target test_async_arena_lifecycle && ctest --test-dir build -R test_async_arena_lifecycle --output-on-failure`
  Expected: PASS (100%).

- [ ] **Step 6: Commit**
  ```bash
  git add src/server/routes.c src/transport/async_server.c tests/unit/test_async_arena_lifecycle.c tests/CMakeLists.txt
  git commit -m "feat(server): integrate request-scoped arena into async worker threads with strict lifetime separation"
  ```

---

### Task 7: Double-Gate Verification & Benchmark Validation

**Files:**
- Modify: `benchmarks/bench_main.c`
- Test: Full test suite (Release + ASan/UBSan)

- [ ] **Step 1: Add arena allocator and direct streaming benchmarks to `benchmarks/bench_main.c`**
  Measure:
  - Arena allocation/reset QPS vs libc malloc/free.
  - Direct streaming JSON-RPC serialization QPS vs cJSON DOM building.

- [ ] **Step 2: Run benchmark suite and record metrics**
  Run: `cmake -B build-bench -DCMAKE_BUILD_TYPE=Release && cmake --build build-bench --target mcpkit_bench && ./build-bench/benchmarks/mcpkit_bench`
  Verify serialization QPS and latency improvements.

- [ ] **Step 3: Run complete test suite in Release mode**
  Run: `ctest --test-dir build-bench --output-on-failure`
  Expected: 100% tests PASS, 0 warnings.

- [ ] **Step 4: Run complete test suite in ASan + UBSan mode**
  Run:
  ```bash
  cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_TESTS=ON
  cmake --build build-asan -j$(nproc)
  ctest --test-dir build-asan --output-on-failure
  ```
  Expected: 100% tests PASS, 0 memory leaks, 0 undefined behaviors.

- [ ] **Step 5: Commit**
  ```bash
  git add benchmarks/bench_main.c
  git commit -m "test(bench): add arena and stream benchmarks, complete double-gate verification"
  ```
