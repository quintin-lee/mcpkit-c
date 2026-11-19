# Design Document: Client Ergonomics Enhancement & Performance Benchmark Harness (v0.3.0 Evolution)

**Date**: 2026-11-19  
**Status**: Complete (Builder Check Approved, Awaiting User Sign-off)  
**Target Release**: v0.3.0  
**Scope**: Full Project  

---

## 1. Intent & The "Why"

With the completion of the v0.2.0 health checkup, `mcpkit-c` has attained high architecture purity, strict one-way layer dependencies, zero compiler warnings under `-Wall -Wextra`, and 100% pass rates across 87 test suites under ASan/UBSan.

However, an objective evaluation of the codebase identifies two primary frontiers for industrial-grade maturity:
1. **Client Ergonomics Gap**: While server-side tool authoring was streamlined via `mcp_tool_result_text_new()`, writing an MCP client in C still requires manual request construction, manual RPC ID tracking, raw JSON argument DOM assembling, manual extraction of `result.content[0].text`, and low-level subscription envelope management.
2. **Empirical Performance Profiling**: While the reactor and async server feature robust atomic connection lifecycles, there is currently no standardized benchmark suite (`benchmarks/`) measuring requests-per-second (RPS), P99 latency, or memory RSS per connection under stress.

This design specifies a balanced evolution roadmap:
- **Phase 1**: High-level Client SDK convenience APIs reducing client code boilerplate by >60%.
- **Phase 2**: Standalone benchmark suite quantifying throughput, latency, and memory footprint, followed by targeted fast-path and buffer reuse optimizations.

---

## 2. Goals, Non-Goals & Anti-Goals

### 2.1 Goals
- **Client Ergonomics**:
  - Provide one-call synchronous tool invocation (`mcp_client_call_tool_text`) returning plain text results without requiring the caller to interact with JSON ASTs.
  - Provide a structured tool invocation helper (`mcp_client_call_tool_json`) taking a JSON arguments string or object and returning the owned `result` object.
  - Provide a stream listener helper (`mcp_client_listen_stream`) that abstracts `subscriptions/listen`, subscription ID extraction, and notification message dispatch via a user callback.
  - Refactor/provide an idiomatic client example (`examples/client/main.c`) demonstrating simple tool execution in under 50 lines of C.
- **Performance & Benchmarking**:
  - Add CMake option `MCPKIT_BUILD_BENCHMARKS` (default OFF) building isolated performance test binaries in `benchmarks/`.
  - Benchmark 1: JSON/RPC serialization & parsing throughput (ops/sec and MB/sec).
  - Benchmark 2: Streamable HTTP & WebSocket throughput (RPS) and latency percentiles (P50, P90, P99) under 100 to 1,000 concurrent client streams.
  - Benchmark 3: Memory footprint profiling (resident RSS bytes per idle/active session).
  - Identify and implement at least one hot-path optimization (e.g. static/preallocated buffer reuse for HTTP header formatting).

### 2.2 Non-Goals
- Introducing any external runtime dependencies (benchmarks will use POSIX `clock_gettime(CLOCK_MONOTONIC)` and standard socket APIs; core library remains 100% zero-dependency).
- Implementing Windows native IOCP runtime (non-Linux platforms continue to use POSIX `poll()` fallback).
- Adding asynchronous futures/promises (C remains idiomatic with synchronous blocking wrappers or reactor-driven callbacks).

### 2.3 Anti-Goals
- **Leaking memory ownership rules**: High-level wrappers must maintain crystal-clear ownership (all returned strings freed via `mcp_free` or clearly documented).
- **Breaking existing low-level APIs**: All existing `mcp_client_*` primitives remain unchanged; new APIs are strictly additive convenience layers.
- **Polluting the core library binary**: Benchmark harnesses will reside exclusively in `benchmarks/` and will not be linked into `libmcpkit_core.a`.

---

## 3. Approaches Considered & Trade-Off Analysis

| Approach | Description | Advantages | Disadvantages | Decision |
| :--- | :--- | :--- | :--- | :---: |
| **Approach 1: Client Ergonomics First** | Focus 100% on Client SDK syntax sugar, fluent builders, and client examples. | Directly resolves the lowest-scoring dimension (DX 88/100); immediate developer benefit. | Leaves networking performance unbenchmarked and lacks quantitative profiling data. | Dropped as solo approach |
| **Approach 2: Pure Networking Optimization** | Focus 100% on Reactor memory pooling, zero-copy socket buffers, and epoll optimizations. | Strengthens gateway/edge positioning; establishes raw throughput numbers. | High implementation complexity; client users continue to suffer from verbose boilerplate. | Dropped as solo approach |
| **Approach 3: Balanced Evolution (Phased)** | **Phase 1**: Client DX convenience APIs.<br>**Phase 2**: Standalone Benchmark Suite + Targeted Fast-Path Optimization. | Maximum ROI: addresses usability immediately, while establishing measurable benchmarks before optimizing. | Requires clear phasing and two distinct milestones. | **Selected (User Approved)** |

---

## 4. Architectural & API Design

### 4.1 Phase 1: Client Ergonomics (`include/mcpkit/client/client.h`)

#### 4.1.1 Simple Tool Execution (`mcp_client_call_tool_text`)
```c
/**
 * @brief High-level helper: invokes a tool and extracts the first text content block.
 *
 * Internally builds the tools/call request, transmits it over the bound transport,
 * parses the response, and extracts content[0].text into a newly allocated heap string.
 *
 * Contract & Ownership Rules:
 * - On successful JSON-RPC round-trip (regardless of tool success or tool domain error),
 *   the function returns MCP_OK, populates *out_text with the result/error message,
 *   and sets *out_is_error (if non-NULL) to indicate whether the tool reported an error.
 *   The caller MUST free *out_text via mcp_free(ctx, *out_text) on MCP_OK.
 * - On transport error, timeout, protocol parse failure, or OOM, the function returns
 *   a non-zero mcp_status_t error code, and *out_text is guaranteed to remain NULL.
 *
 * @param ctx             Context; may be NULL (default allocator).
 * @param client          Initialized client handle; must not be NULL.
 * @param tool_name       Name of the tool to execute; must not be NULL.
 * @param json_args_str   Arguments JSON string (e.g. "{\"text\":\"hello\"}"), or NULL for no args.
 * @param out_text        Receives newly allocated text string on MCP_OK.
 *                        Caller frees with mcp_free(ctx, *out_text).
 * @param out_is_error    Optional output flag: set to true if server returned isError:true.
 * @return MCP_OK on successful RPC round-trip;
 *         MCP_ERR_PROTOCOL on malformed JSON or response mismatch;
 *         MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_INVALID_ARGUMENT on invalid client/tool_name.
 */
mcp_status_t mcp_client_call_tool_text(mcp_context_t *ctx,
                                       mcp_client_t *client,
                                       const char *tool_name,
                                       const char *json_args_str,
                                       char **out_text,
                                       bool *out_is_error);
```

#### 4.1.2 Structured Tool Execution (`mcp_client_call_tool_json`)
```c
/**
 * @brief High-level helper: invokes a tool with a parsed or constructed JSON argument object.
 *
 * @param ctx             Context; may be NULL.
 * @param client          Initialized client handle; must not be NULL.
 * @param tool_name       Name of the tool to execute; must not be NULL.
 * @param args_obj        Borrowed JSON object containing tool arguments; may be NULL.
 * @param out_result      Receives owned JSON object corresponding to response "result".
 *                        Caller destroys with mcp_json_destroy(ctx, *out_result).
 * @return MCP_OK on success; MCP_ERR_* on transport, protocol, or allocation failure.
 */
mcp_status_t mcp_client_call_tool_json(mcp_context_t *ctx,
                                       mcp_client_t *client,
                                       const char *tool_name,
                                       const mcp_json_value_t *args_obj,
                                       mcp_json_value_t **out_result);
```

#### 4.1.3 Stream Subscription Callback Wrapper (`mcp_client_listen_stream`)
```c
/**
 * @brief Callback invoked whenever a notification matching the active subscription arrives.
 *
 * @param client    Client handle.
 * @param method    Notification method name (e.g. "notifications/tools/list_changed").
 * @param params    Borrowed JSON parameters object (may be NULL).
 * @param user_data User context pointer.
 * @return true to continue listening; false to stop the listening loop gracefully.
 */
typedef bool (*mcp_client_notification_fn)(mcp_client_t *client,
                                           const char *method,
                                           const mcp_json_value_t *params,
                                           void *user_data);

/**
 * @brief High-level helper: subscribes to server notifications and pumps incoming events.
 *
 * Lifecycle & Timeout Rules:
 * - Executes subscriptions/listen, extracts subscriptionId, and enters the event pump.
 * - When max_events is reached, callback returns false, or inactivity_timeout_ms expires,
 *   the function automatically transmits `notifications/cancelled` for the subscriptionId,
 *   drains any pending ack, and returns cleanly to preserve connection frame alignment.
 * - inactivity_timeout_ms specifies the maximum wait time for the next event; if no event
 *   arrives within this interval, the loop exits with MCP_ERR_TIMEOUT.
 *
 * @param ctx                   Context; may be NULL.
 * @param client                Initialized client handle.
 * @param filter_mask           Bitmask or filter string (e.g. "tools", "resources", "all").
 * @param inactivity_timeout_ms Inactivity timeout per read in milliseconds (0 = non-blocking, -1 = block forever).
 * @param max_events            Maximum number of events to process before returning (0 = unlimited).
 * @param callback              Notification handler callback; must not be NULL.
 * @param user_data             Opaque pointer passed to callback.
 * @return MCP_OK on clean completion; MCP_ERR_TIMEOUT if inactivity timeout expires; MCP_ERR_* on failure.
 */
mcp_status_t mcp_client_listen_stream(mcp_context_t *ctx,
                                      mcp_client_t *client,
                                      const char *filter_mask,
                                      int inactivity_timeout_ms,
                                      size_t max_events,
                                      mcp_client_notification_fn callback,
                                      void *user_data);
```

---

### 4.2 Phase 2: Performance Benchmark Harness (`benchmarks/`)

#### 4.2.1 Directory Structure & Build Integration
```
benchmarks/
├── CMakeLists.txt
├── bench_common.h          # High-resolution monotonic timer, statistical helpers (p50/p90/p99)
├── bench_json_rpc.c        # In-memory JSON serialization/deserialization and message framing
├── bench_http_throughput.c # Multi-client concurrent Streamable HTTP POST load generator
├── bench_ws_throughput.c   # RFC 6455 WebSocket frame & message streaming load generator
└── bench_async_server.c    # Process-isolated multi-connection async server RSS & P99 latency measurement
```
Build option in `cmake/MCPKitOptions.cmake`:
```cmake
option(MCPKIT_BUILD_BENCHMARKS "Build performance benchmark suite" OFF)
```

#### 4.2.2 Process Isolation for RSS Profiling
To accurately measure server RSS without distortion from client connection sockets and load-generator buffers:
`bench_async_server` forks a child process hosting the `mcp_async_server_t` reactor. The parent process drives concurrent connections over TCP. The parent samples the child server's pure resident set size directly from `/proc/<child_pid>/statm` (on Linux) or platform task info.

#### 4.2.3 Metrics & Reporting Format
Each benchmark binary outputs both human-readable summary tables and machine-parseable JSON lines:
```json
{
  "benchmark": "streamable_http_concurrency",
  "concurrency": 500,
  "duration_sec": 5.0,
  "total_requests": 142500,
  "qps": 28500.0,
  "latency_us": {
    "min": 18,
    "p50": 110,
    "p90": 240,
    "p99": 650,
    "max": 2100
  },
  "rss_kb_peak": 8420
}
```

#### 4.2.3 Targeted Fast-Path Optimization Plan
1. **HTTP Error Formatting Fast-Path**: Eliminate temporary allocations in `streamable_http.c` error responses using stack-allocated small buffers.
2. **JSON Buffer Re-use**: In `mcp_message_serialize`, evaluate small-string buffer reservation to avoid intermediate reallocations on common payload shapes.

---

## 5. Verification & Test Plan

1. **Unit Test Suite**:
   - `tests/unit/test_client_convenience.c`: Test `mcp_client_call_tool_text` with success, tool error (`isError: true`), invalid arguments, and server protocol errors.
   - Test `mcp_client_call_tool_json` with complex JSON payloads and verify ownership cleanup under ASan.
   - Test `mcp_client_listen_stream` with mock notification pump and verify max_events and timeout termination.
2. **Benchmark Execution Verification**:
   - Add a fast smoke-test script `benchmarks/run_benchmarks.sh --quick` verifying each benchmark binary compiles, executes 1,000 iterations without crash, and emits valid statistics.
3. **Double Verification Gates**:
   - Gate 1: Release mode (all 87 tests + new client unit tests pass with zero warnings).
   - Gate 2: ASan + UBSan (all tests pass with zero leaks and zero memory errors).

---

## 6. What's Left to the Builder
- Exact internal implementation details of `mcp_client_call_tool_text` error message extraction if `content` array contains non-text elements.
- Benchmark command line flag parsing (e.g. `--concurrency`, `--duration`, `--json`).
- Tuning benchmark default iteration counts to keep standard CI runs fast (<5 seconds) while allowing deep profiling mode (`--deep`).
