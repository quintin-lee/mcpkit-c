# MCP Advanced Protocol Specifications & Request-Scoped Arena Allocator Design

## 1. Intent and the Why
`mcpkit-c` recently completed its Phase 1 production hardening, delivering vectorized I/O (`mcp_transport_sendv`), user-space socket backpressure with high/low watermarks and slow-client stall eviction, Streamable HTTP SSE session management, and graceful server draining (`mcp_async_server_drain`). Benchmarking demonstrates high baseline throughput (JSON serialization ~860k QPS, WebSocket streaming ~520k QPS, Streamable HTTP POST ~101k QPS) with a lightweight RSS footprint (~328B/conn) and 100% leak-free, warning-free execution.

However, to elevate `mcpkit-c` into an enterprise-ready, industry-standard MCP reference implementation in C, two fundamental architectural gaps remain:
1. **MCP Protocol Standard Advanced Features (SEP Conformance)**: Modern clients (e.g., Claude Desktop, VS Code, Cursor) and production agents rely heavily on stream resumption over unstable networks (Streamable HTTP event replay via `Last-Event-ID` & server-side ring buffering, SEP-1577 / SEP-1699), fine-grained resource/tool change notifications (`resources/subscribe`, `notifications/resources/updated`, `notifications/tools/list_changed`), and HTTP conditional caching (`ETag` / `If-None-Match` 304 responses).
2. **Request-Scoped Arena Allocator & Direct Streaming Serialization**: The current request dispatch path allocates multiple discrete heap nodes via libc `malloc`/`free` through the DOM layer (cJSON) for every incoming request and outgoing response. Under high-concurrency microservice loads or embedded edge deployments, dynamic memory churn induces heap fragmentation, cache misses, and memory allocator lock contention. A request-scoped monotonic arena allocator (`mcp_arena_t`) combined with a direct streaming serializer writing straight to `mcp_iovec_t` eliminates heap thrashing and provides predictable sub-microsecond latency.

---

## 2. Goals, Non-Goals, and Anti-Goals

### 2.1 Goals
- **G1: Streamable HTTP Event Resumption (SEP-1577 / SEP-1699)**:
  - Add bounded, per-session event ring buffers to `async_server`.
  - Assign monotonically increasing `uint64_t` event IDs to outgoing SSE events (`id: <id>\ndata: ...\n\n`).
  - Parse incoming `Last-Event-ID` on GET SSE connections:
    - If valid and unexpired in the ring buffer, replay all missed events seamlessly upon reconnect.
    - If expired (evicted from ring buffer), respond with HTTP `410 Gone`.
    - If absent, proceed with standard fresh stream attachment.
  - Allow `mcp_async_server_push_sse` to successfully buffer events (`MCP_OK`) even when a client is temporarily disconnected (`sse_conn == NULL`).
- **G2: Fine-Grained Subscription & Change Notification Engine**:
  - Implement dynamic subscription registry for `resources/subscribe` and `resources/unsubscribe` (2024-11-05 & 2025 specs), coexisting cleanly with SEP-2575 `subscriptions/listen`.
  - Expose server notification dispatch APIs:
    - `mcp_server_notify_resource_updated(srv, uri)`: Routes notifications to sessions subscribed to the target URI.
    - `mcp_server_notify_tools_changed(srv)`: Emits `notifications/tools/list_changed` to active sessions.
    - `mcp_server_notify_prompts_changed(srv)`: Emits `notifications/prompts/list_changed` to active sessions.
  - Provide an abstract session notification sink (`mcp_session_set_sink`) so transports (async_server, socket, stdio) receive server-pushed notifications asynchronously.
- **G3: HTTP Conditional Caching (`ETag` / `If-None-Match`)**:
  - Implement deterministic 64-bit fingerprinting of registered tool, resource, and prompt schemas, scoped by visibility mode.
  - In `async_server.c`, evaluate `If-None-Match` headers for unpaginated `tools/list`, `resources/list`, and `prompts/list` requests. Return HTTP `304 Not Modified` with empty body when unchanged.
- **G4: Request-Scoped Monotonic Arena Allocator (`mcp_arena_t`)**:
  - Implement a chunk-based bump allocator supporting arbitrary alignments, compact 8-byte chunk allocation headers enabling full `realloc` support, bulk reset in $O(1)$ without freeing underlying chunks to the OS, and bridging into `mcp_allocator_t`.
  - Enforce strict lifetime separation: transient request parsing and DOM trees use the request arena; persistent session/server state mutations (e.g. `initialize`, subscriptions) explicitly use `server->ctx` persistent allocator.
- **G5: Direct Streaming JSON-RPC Serializer**:
  - Provide a zero-DOM streaming emitter for JSON-RPC results, errors, ping/pong, and standard notifications directly formatting into pre-allocated memory or `mcp_iovec_t` buffers.

### 2.2 Non-Goals
- **NG1: Distributed State Synchronization**: Ring buffers and subscriptions are session-bound and in-process; distributed clustering/shared redis backends are out of scope.
- **NG2: Replacing cJSON Completely**: cJSON remains supported as a built-in DOM backend; the direct streaming serializer and arena allocator operate alongside it as performance-accelerated paths.
- **NG3: Kernel-specific io_uring or Multi-Reactor**: Multi-reactor scaling and `io_uring` are reserved for subsequent cloud-scale milestones.

### 2.3 Anti-Goals
- **AG1: Unbounded Ring Buffers**: Memory per session must be strictly bounded; slow or disconnected clients must never induce unbounded heap growth.
- **AG2: Breaking API Changes**: Existing signatures for `mcp_server_t`, `mcp_server_dispatch`, and `mcp_async_server_t` must remain backward-compatible.
- **AG3: Thread Contention on Arenas**: Arenas must not be shared across concurrent threads without locks; preferred architecture is per-worker thread arenas with zero lock contention.

---

## 3. Approaches Considered & Trade-offs

During initial planning, four potential evolution directions were evaluated:
1. **Approach A: MCP Protocol Standard Advanced Features (SEP Conformance)**.
2. **Approach B: Request Arena Allocator & Direct Streaming Serialization**.
3. **Approach C: Multi-Reactor & Linux `io_uring`**.
4. **Approach D: Static Minimal Embedded Profile (`-DMCPKIT_EMBEDDED=ON`)**.

### Selection: Hybrid Approach (Direction 1 + Direction 2)
- **Trade-off Analysis**:
  - Approach C optimizes multi-core scaling for cloud servers but ties the codebase heavily to Linux-specific kernel interfaces while the protocol features are still missing.
  - Approach D narrows target platforms to tiny microcontrollers while `mcpkit-c`'s primary usage today is as a high-performance native agent/tool server.
  - Approaches 1 and 2 directly complement each other: Approach 1 brings full standard conformance and resilience for modern LLM clients (Cursor, Claude, VS Code), while Approach 2 ensures that adding rich protocol features does not degrade latency or create heap fragmentation.
- **Decision**: Adopt the hybrid plan combining Protocol Advanced Specs (Resumption, Subscriptions, ETags) with Request Arena Allocator and Direct Streaming Serialization.

---

## 4. Architectural Design & Implementation Details

### 4.1 Streamable HTTP Event Resumption & Ring Buffer (SEP-1577 / SEP-1699)

#### 4.1.1 Event Ring Buffer (`mcp_event_ring_buffer_t`)
Each active HTTP session entry (`session_entry_t`) in `async_server.c` owns an optional event ring buffer:
```c
typedef struct {
    uint64_t event_id;
    char *data;         /* Fully framed SSE payload: "id: <id>\ndata: ...\n\n" */
    size_t len;
} mcp_event_entry_t;

typedef struct {
    mcp_event_entry_t *entries;
    size_t capacity;     /* Configurable, default e.g. 128 slots */
    size_t head;         /* Index of next write */
    size_t count;        /* Active items in ring */
    uint64_t next_id;    /* Monotonically increasing next event ID (starts at 1) */
    uint64_t oldest_id;  /* Lowest event ID currently preserved in buffer */
    mtx_t lock;
} mcp_event_ring_buffer_t;
```

#### 4.1.2 Lifecycle, Framing, and Reconnection Flow
1. **Pushing Events (`mcp_async_server_push_sse`)**:
   - Accepts raw JSON or text payload.
   - If the payload does not already contain an `id:` line, automatically formats with monotonic event ID: `id: %llu\ndata: %s\n\n`.
   - Acquires `ring_buffer->lock`.
   - Stores the entry in `entries[head]`, evicting the oldest item if `count == capacity` (advancing `oldest_id = entries[new_tail].event_id`).
   - If session currently has an attached live SSE connection (`entry->sse_conn != NULL`), enqueues payload to socket outbox.
   - If session is temporarily disconnected (`entry->sse_conn == NULL`), returns `MCP_OK` because the event is safely preserved in the ring buffer awaiting reconnection.
   - Releases `ring_buffer->lock`.

2. **Incoming GET `/mcp` Connection Handling**:
   - Extracts `Mcp-Session-Id` and `Last-Event-ID` from HTTP request headers.
   - If `Last-Event-ID` is present:
     - Parse `last_id = strtoull(last_event_id_str, NULL, 10)`.
     - Acquire `ring_buffer->lock`.
     - **Case A (`last_id < ring_buffer->oldest_id - 1`)**: The client's requested point in time has already been evicted from the buffer. Return `HTTP/1.1 410 Gone\r\nContent-Type: text/plain\r\n\r\nStream event history expired`.
     - **Case B (`last_id >= ring_buffer->oldest_id && last_id < ring_buffer->next_id - 1`)**:
       - Send `HTTP/1.1 200 OK\r\nContent-Type: text/event-stream...`.
       - Replay all entries where `entry->event_id > last_id` directly into the newly attached `conn->write_buf` (or vector outbox) in chronological order.
     - **Case C (`last_id >= ring_buffer->next_id - 1`)**: Client is already up to date. Send 200 OK headers; await future pushes.
     - Release `ring_buffer->lock`.

---

### 4.2 Fine-Grained Subscription & Notification Routing

#### 4.2.1 Data Structures in `mcp_session_t`
To cleanly reconcile legacy MCP subscriptions (`resources/subscribe`) with SEP-2575 (`subscriptions/listen`), `mcp_session_t` maintains two dedicated subscription structures:
```c
typedef struct {
    char **uris;
    size_t count;
    size_t capacity;
} mcp_uri_set_t;

/* In struct mcp_session: */
struct mcp_session {
    ...
    mcp_uri_set_t direct_subscriptions;     /* From resources/subscribe */
    mcp_subscription_stream_t active_stream; /* From SEP-2575 subscriptions/listen */
    mcp_session_sink_fn notify_sink;        /* Callback for asynchronous notifications */
    void *notify_userdata;
    mtx_t sub_lock;
};
```

#### 4.2.2 RPC Routes
- **`resources/subscribe`**:
  - Request: `{"jsonrpc":"2.0","id":1,"method":"resources/subscribe","params":{"uri":"resource://status"}}`
  - Validates `uri` against registered resources / templates.
  - Adds URI to `session->direct_subscriptions` (allocating via `session->ctx` persistent allocator).
  - Response: `{"jsonrpc":"2.0","id":1,"result":{}}`.
- **`resources/unsubscribe`**:
  - Removes URI from `session->direct_subscriptions`.
  - Response: `{"jsonrpc":"2.0","id":1,"result":{}}`.

#### 4.2.3 Server Notification Dispatch APIs & Transport Sink
```c
typedef mcp_status_t (*mcp_session_sink_fn)(mcp_context_t *ctx,
                                            mcp_session_t *session,
                                            const char *payload,
                                            size_t len,
                                            void *userdata);

void mcp_session_set_sink(mcp_session_t *session,
                          mcp_session_sink_fn sink,
                          void *userdata);

mcp_status_t mcp_server_notify_resource_updated(mcp_context_t *ctx,
                                                mcp_server_t *server,
                                                const char *uri);

mcp_status_t mcp_server_notify_tools_changed(mcp_context_t *ctx,
                                             mcp_server_t *server);

mcp_status_t mcp_server_notify_prompts_changed(mcp_context_t *ctx,
                                               mcp_server_t *server);
```
- In `async_server.c`, when an HTTP session is created, it calls `mcp_session_set_sink(session, async_session_sink, s)`:
  - `async_session_sink` routes directly into `mcp_async_server_push_sse(ctx, s, session_id, payload, len)`.
- When `mcp_server_notify_resource_updated` is called:
  - For each session:
    - If subscribed via `direct_subscriptions`: formats standard `notifications/resources/updated` (without `_meta.subscriptionId`).
    - If subscribed via `active_stream`: formats SEP-2575 notification with `_meta.subscriptionId`.
    - Dispatches payload through `session->notify_sink`.

---

### 4.3 HTTP Conditional Caching (`ETag` / `If-None-Match`)

#### 4.3.1 ETag Computation & Visibility Scoping
- Server maintains 64-bit revision hashes:
  - `uint64_t tools_hash_all`, `uint64_t tools_hash_apps`;
  - `uint64_t resources_hash`, `uint64_t prompts_hash`.
- Recalculated using FNV-1a whenever tools/resources/prompts are registered or unregistered.
- Format: `ETag: "\"<hex64>\""`.

#### 4.3.2 Transport Caching Evaluation in Worker Pool
1. Evaluated in worker thread during `tools/list`, `resources/list`, `prompts/list` handling.
2. If request contains a pagination `cursor` (`params.cursor != NULL`), skip ETag match (pagination bypasses 304).
3. If `If-None-Match` header is present and matches the hash corresponding to the session's visibility scope:
   - Immediately return HTTP `304 Not Modified\r\nETag: <etag>\r\nCache-Control: private, no-cache\r\nContent-Length: 0\r\n\r\n`.
   - Skip DOM tree serialization and JSON-RPC dispatch completely.
4. Otherwise:
   - Execute dispatch, attach `ETag: <etag>` to 200 OK HTTP response headers.

---

### 4.4 Request-Scoped Monotonic Arena Allocator (`mcp_arena_t`)

#### 4.4.1 Chunk Layout & 8-Byte Allocation Headers for `realloc`
To support `realloc` (required by JSON DOM parsing and array growth) without losing bump allocation speed, each allocation prepends an 8-byte header:
```c
typedef struct {
    size_t size;  /* Usable allocation size */
} mcp_arena_alloc_hdr_t;

typedef struct mcp_arena_chunk {
    struct mcp_arena_chunk *next;
    size_t capacity;
    size_t offset;
    uint8_t data[];
} mcp_arena_chunk_t;

struct mcp_arena {
    mcp_arena_chunk_t *first;
    mcp_arena_chunk_t *current;
    size_t default_chunk_size; /* e.g. 32KB */
    size_t total_allocated;
    const mcp_allocator_t *upstream;
};
```
- **`mcp_arena_alloc`**: Rounds `size + sizeof(mcp_arena_alloc_hdr_t)` up to 8-byte / 16-byte alignment. Writes `hdr->size = size`, returns `data` pointer.
- **`mcp_arena_realloc`**: Reads `hdr->size`. If `ptr` is the top-of-bump item and chunk capacity allows, expands `offset` in-place. Otherwise, allocates new block and performs `memcpy(new_ptr, ptr, MIN(old_size, new_size))`.

#### 4.4.2 Separation of Transient vs. Persistent State
- **Strict Rule**:
  - `req_ctx` (backed by worker `mcp_arena_t`) is strictly for transient request processing: JSON request parsing, AST navigation, intermediate values, and response DOM serialization.
  - Any state that outlives the request (e.g. `clientInfo` in `initialize`, URI subscriptions in `resources/subscribe`, registered tools/resources) MUST be allocated using `server->ctx` or `session->ctx` (the persistent upstream allocator).
  - In `src/server/routes.c`, `route_initialize` and subscription handlers explicitly allocate persistent session fields from `session->ctx`.

#### 4.4.3 Worker Lifecycle
In `async_server.c`, each worker thread in the thread pool owns an `mcp_arena_t`:
```c
/* In worker dispatch loop: */
mcp_arena_reset(worker->arena);
mcp_allocator_t alloc = mcp_arena_as_allocator(worker->arena);
mcp_context_config_t req_cfg = { .allocator = &alloc, .logger = s->logger };
mcp_context_t *req_ctx = mcp_context_create(&req_cfg);

/* Parse and dispatch request */
mcp_server_dispatch(req_ctx, srv, session, msg, &resp);

/* Serialize response to socket outbox */
...
mcp_context_destroy(req_ctx);
mcp_arena_reset(worker->arena);
```

---

### 4.5 Direct Streaming JSON-RPC Serializer

#### 4.5.1 Mechanism & Integration Scope
Direct streaming serializer (`mcp_json_stream_t`) provides high-speed, zero-AST serialization formatting directly into arena memory or outbox buffers:
```c
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    mcp_arena_t *arena;
} mcp_json_stream_t;

void mcp_json_stream_init(mcp_json_stream_t *s, mcp_arena_t *arena);
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
- **Usage Scope**:
  - Automatically used for error envelopes, ping/pong heartbeats, HTTP 202 accepted responses, and standard subscription acks.
  - Used for raw result payloads when tool handlers provide direct JSON text output.
  - Standard handlers using DOM continue to work unmodified.

---

## 5. What's Left to the Builder (Implementation Discretion)
- Selection of initial default capacity for the SSE event ring buffer (default: 128 events, configurable via `mcp_async_server_config_t::sse_event_ring_capacity`).
- Selection of default chunk size for `mcp_arena_t` (default: 32KB).
- Allocation placement of `worker->arena` in `async_server.c` (`worker_t` struct array vs TLS).

---

## 6. Verification and Acceptance Criteria
1. **Double-Gate Build Verification**:
   - Standard release build: 0 compiler warnings under `-Wall -Wextra -Wpedantic -Werror`.
   - ASan + UBSan build: 100% tests pass, 0 memory leaks, 0 undefined behaviors.
2. **Unit & Integration Test Suite**:
   - `test_arena`: Verify allocation alignment, realloc expansion, multiple chunk growth, `mcp_arena_reset`, and `mcp_allocator_t` bridge compliance.
   - `test_streamable_resumption`: Simulate SSE client disconnection and reconnection with `Last-Event-ID`:
     - Test replaying missed events when in range.
     - Test HTTP `410 Gone` when `Last-Event-ID` has expired.
     - Test incremental ID ordering.
     - Test pushing events to disconnected sessions (`MCP_OK`).
   - `test_subscriptions`: Verify `resources/subscribe`, `resources/unsubscribe`, and targeted notification delivery via `mcp_server_notify_resource_updated`.
   - `test_http_etag`: Verify 304 Not Modified returned on matching `If-None-Match` without cursor, and 200 OK returned on modified schemas or cursor pagination.
   - `test_json_stream`: Verify serialized output matches exact JSON-RPC 2.0 specifications.
3. **Performance Benchmarks**:
   - Benchmark QPS improvement for JSON-RPC serialization using the direct stream emitter.
   - Benchmark dispatch throughput and latency under high-concurrency request workloads with `mcp_arena_t`.
