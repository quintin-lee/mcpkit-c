# Client Ergonomics Enhancement & Performance Benchmark Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement high-level Client SDK ergonomics helpers (`mcp_client_call_tool_text`, `mcp_client_call_tool_json`, `mcp_client_listen_stream`) and a standalone performance benchmark suite (`benchmarks/`) with process-isolated RSS and latency measurement.

**Architecture:** Extend `include/mcpkit/client/client.h` and `src/client/client.c` with safe, memory-leak-free composition wrappers over existing transport/message primitives; add an optional `-DMCPKIT_BUILD_BENCHMARKS=ON` CMake target suite in `benchmarks/` providing high-resolution monotonic timing, P50/P90/P99 latency distribution, and `/proc/<pid>/statm` memory tracking.

**Tech Stack:** C23, POSIX standard APIs (sockets, `clock_gettime(CLOCK_MONOTONIC)`, `pipe`, `fork`), CMake 3.28+, AddressSanitizer/UBSan.

**Spec:** [docs/superpowers/specs/2026-11-19-client-ergonomics-and-performance-benchmarking-design.md](file:///data/home/quintin/workspace/source/c/mcpkit-c/docs/superpowers/specs/2026-11-19-client-ergonomics-and-performance-benchmarking-design.md)

---

## Global Constraints

- **Language Standard**: C23 (`set(CMAKE_C_STANDARD 23)`, `CMAKE_C_EXTENSIONS OFF`).
- **Compiler Warnings**: Zero warnings under `-Wall -Wextra` across GCC and Clang.
- **Zero Runtime Dependencies**: Core library must strictly link standard C/POSIX libc only (benchmarks use standard POSIX monotonic clocks and sockets).
- **Memory Contract**: Output string pointers from `mcp_client_call_tool_text` are allocated via context allocator and freed via `mcp_free(ctx, *out_text)`. Output pointers are guaranteed `NULL` on non-`MCP_OK` returns.
- **Double Gates**: Every change must pass both Release test suite and ASan/UBSan memory checks (87+ tests).

## Review Focus

1. **Tool Domain Error Handling**: When server returns `result.isError: true`, `mcp_client_call_tool_text` must return `MCP_OK` with `*out_is_error = true` and `*out_text` containing the error text, freeing caller from guessing if output pointer is allocated.
2. **Subscription Lifecycle Leak**: `mcp_client_listen_stream` must always emit `notifications/cancelled` on loop exit (whether due to event limit, timeout, or callback early return) so subsequent requests do not read unconsumed stream frames.
3. **Arguments Cloning Safety**: `mcp_client_call_tool_json` must clone borrowed `args_obj` rather than consuming it, preventing double-free in caller code.
4. **Benchmark Process Contamination**: `bench_async_server` must fork the server process so client connection buffers do not pollute server resident RSS metrics.
5. **Clean Inactivity Timeouts**: Per-read inactivity timeouts in `mcp_client_listen_stream` must cleanly return `MCP_ERR_TIMEOUT` without socket corruption.

---

### Task 1: Simple Tool Result Client API (`mcp_client_call_tool_text`)

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Create: `tests/unit/test_client_convenience.c`
- Modify: `tests/CMakeLists.txt:40-60`
- Modify: `src/client/client.c:380-420`

**Interfaces:**
- Produces:
  ```c
  mcp_status_t mcp_client_call_tool_text(mcp_context_t *ctx,
                                         mcp_client_t *client,
                                         const char *tool_name,
                                         const char *json_args_str,
                                         char **out_text,
                                         bool *out_is_error);
  ```

- [ ] **Step 1: Declare `mcp_client_call_tool_text` in `include/mcpkit/client/client.h`**
Add declaration and complete docstring with ownership contract.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_client_convenience.c` and register in `tests/CMakeLists.txt`**
Test cases:
1. `call_tool_text_success`: Calls an echo tool with `{"text":"hello"}`, asserts `st == MCP_OK`, `*out_is_error == false`, `strcmp(*out_text, "hello") == 0`, frees with `mcp_free`.
2. `call_tool_text_error`: Server returns `isError: true` with text `"failed"`, asserts `st == MCP_OK`, `*out_is_error == true`, `strcmp(*out_text, "failed") == 0`, frees with `mcp_free`.
3. `call_tool_text_invalid_args`: Invalid JSON syntax returns `MCP_ERR_PROTOCOL` and `*out_text == NULL`.

- [ ] **Step 3: Run test to verify it fails to link or compile**
Run: `cmake --build build -j --target test_client_convenience`
Expected: FAIL with undefined reference to `mcp_client_call_tool_text`.

- [ ] **Step 4: Implement `mcp_client_call_tool_text` in `src/client/client.c`**
Parse `json_args_str` if non-NULL, invoke `mcp_client_call_tool`, inspect `result.isError` and `result.content[0].text`, copy text via `ctx` allocator, destroy result, return `MCP_OK`.

- [ ] **Step 5: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_client_convenience --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**
```bash
git add include/mcpkit/client/client.h src/client/client.c tests/CMakeLists.txt tests/unit/test_client_convenience.c
git commit -m "feat(client): implement mcp_client_call_tool_text convenience API"
```

---

### Task 2: Structured Tool Result Client API (`mcp_client_call_tool_json`)

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `tests/unit/test_client_convenience.c`
- Modify: `src/client/client.c`

**Interfaces:**
- Consumes: `mcp_client_call_tool`
- Produces:
  ```c
  mcp_status_t mcp_client_call_tool_json(mcp_context_t *ctx,
                                         mcp_client_t *client,
                                         const char *tool_name,
                                         const mcp_json_value_t *args_obj,
                                         mcp_json_value_t **out_result);
  ```

- [ ] **Step 1: Declare `mcp_client_call_tool_json` in `include/mcpkit/client/client.h`**
Document that `args_obj` is borrowed and cloned internally, and `*out_result` is owned by caller.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_client_convenience.c`**
Test cases:
1. Pass structured JSON object `{"a": 10, "b": 20}`. Verify caller retains `args_obj` (destroyed by caller without crash), and `out_result` contains full returned object.
2. NULL `args_obj` successfully passes empty object.

- [ ] **Step 3: Run test to verify it fails**
Run: `cmake --build build -j --target test_client_convenience`
Expected: FAIL with undefined reference to `mcp_client_call_tool_json`.

- [ ] **Step 4: Implement `mcp_client_call_tool_json` in `src/client/client.c`**
Clone `args_obj` using `mcp_json_clone(ctx, args_obj)` (or pass NULL if args is NULL), invoke `mcp_client_call_tool`, and assign `*out_result`.

- [ ] **Step 5: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_client_convenience --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**
```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_client_convenience.c
git commit -m "feat(client): implement mcp_client_call_tool_json API"
```

---

### Task 3: Stream Subscription Callback Helper (`mcp_client_listen_stream`)

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `tests/unit/test_client_convenience.c`
- Modify: `src/client/client.c`

**Interfaces:**
- Produces:
  ```c
  typedef bool (*mcp_client_notification_fn)(mcp_client_t *client,
                                             const char *method,
                                             const mcp_json_value_t *params,
                                             void *user_data);

  mcp_status_t mcp_client_listen_stream(mcp_context_t *ctx,
                                        mcp_client_t *client,
                                        const char *filter_mask,
                                        int inactivity_timeout_ms,
                                        size_t max_events,
                                        mcp_client_notification_fn callback,
                                        void *user_data);
  ```

- [ ] **Step 1: Declare `mcp_client_listen_stream` in `include/mcpkit/client/client.h`**
Add typedef for `mcp_client_notification_fn` and declare `mcp_client_listen_stream` with lifecycle rules.

- [ ] **Step 2: Add failing unit tests in `tests/unit/test_client_convenience.c`**
Test cases:
1. `listen_stream_event_limit`: Subscribe to "tools", receive 2 notifications, callback counts events, returns `true`, function cleanly terminates when `max_events == 2`.
2. `listen_stream_early_exit`: Callback returns `false` on event 1; function immediately stops and sends `notifications/cancelled`.
3. `listen_stream_timeout`: Inactivity timeout fires with `MCP_ERR_TIMEOUT`.

- [ ] **Step 3: Run test to verify it fails**
Run: `cmake --build build -j --target test_client_convenience`
Expected: FAIL with undefined reference to `mcp_client_listen_stream`.

- [ ] **Step 4: Implement `mcp_client_listen_stream` in `src/client/client.c`**
Compose `mcp_client_subscriptions_listen`, extract `subscriptionId`, set transport timeout, enter pump loop calling `callback`, and call `mcp_client_cancel_subscription` before returning.

- [ ] **Step 5: Run tests and verify they pass**
Run: `cmake --build build -j && ctest --test-dir build -R test_client_convenience --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**
```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_client_convenience.c
git commit -m "feat(client): implement mcp_client_listen_stream with automatic subscription cancellation"
```

---

### Task 4: Client Example Modernization (`examples/client/main.c`)

**Files:**
- Modify: `examples/client/main.c`

**Interfaces:**
- Consumes: `mcp_client_call_tool_text`

- [ ] **Step 1: Refactor `examples/client/main.c`**
Replace manual JSON-RPC call construction with `mcp_client_call_tool_text(ctx, c, "echo", "{\"text\":\"hello world\"}", &reply, &is_err)`.

- [ ] **Step 2: Build and run `examples/client`**
Run: `cmake --build build -j --target client`
Expected: Clean compilation with 0 warnings.

- [ ] **Step 3: Commit**
```bash
git add examples/client/main.c
git commit -m "example(client): simplify client example using mcp_client_call_tool_text"
```

---

### Task 5: Benchmark Scaffolding & JSON/RPC In-Memory Benchmark

**Files:**
- Modify: `cmake/MCPKitOptions.cmake`
- Modify: `CMakeLists.txt`
- Create: `benchmarks/CMakeLists.txt`
- Create: `benchmarks/bench_common.h`
- Create: `benchmarks/bench_json_rpc.c`

**Interfaces:**
- Produces: `bench_timer_t`, `bench_stats_t`, binary `bench_json_rpc`

- [ ] **Step 1: Add `MCPKIT_BUILD_BENCHMARKS` option to `cmake/MCPKitOptions.cmake` and root `CMakeLists.txt`**
Add option and include `add_subdirectory(benchmarks)` when enabled.

- [ ] **Step 2: Create `benchmarks/bench_common.h`**
Implement monotonic nanosecond timing (`clock_gettime(CLOCK_MONOTONIC)`), sorting, and percentile calculation (`bench_calc_stats` for min, p50, p90, p99, max, qps), and JSON-formatter.

- [ ] **Step 3: Create `benchmarks/bench_json_rpc.c` and `benchmarks/CMakeLists.txt`**
Measure:
1. Serialization throughput of `mcp_message_serialize` over 100,000 iterations.
2. Parsing throughput of `mcp_message_parse` over 100,000 iterations.
Output both summary table and JSON line.

- [ ] **Step 4: Build and run `bench_json_rpc`**
Run: `cmake -B build-bench -DMCPKIT_BUILD_BENCHMARKS=ON && cmake --build build-bench --target bench_json_rpc && ./build-bench/benchmarks/bench_json_rpc`
Expected: Outputs stats without crash or memory violation.

- [ ] **Step 5: Commit**
```bash
git add cmake/MCPKitOptions.cmake CMakeLists.txt benchmarks/CMakeLists.txt benchmarks/bench_common.h benchmarks/bench_json_rpc.c
git commit -m "bench(core): add benchmark build infrastructure and json-rpc microbenchmark"
```

---

### Task 6: Streamable HTTP POST Concurrent Throughput Benchmark

**Files:**
- Create: `benchmarks/bench_http_throughput.c`
- Modify: `benchmarks/CMakeLists.txt`

**Interfaces:**
- Consumes: `bench_common.h`, `mcp_http_serve`, `mcp_transport_socket`
- Produces: binary `bench_http_throughput`

- [ ] **Step 1: Implement `benchmarks/bench_http_throughput.c`**
Spin up a local loopback HTTP server on an ephemeral port, launch N client worker threads driving pipelined `tools/call` HTTP POST requests over 5 seconds, collect latency samples, and compute QPS & P99 latency.

- [ ] **Step 2: Add target to `benchmarks/CMakeLists.txt`**

- [ ] **Step 3: Build and run `bench_http_throughput`**
Run: `cmake --build build-bench --target bench_http_throughput && ./build-bench/benchmarks/bench_http_throughput --concurrency 50 --duration 2`
Expected: Completes cleanly and prints QPS and latency distribution.

- [ ] **Step 4: Commit**
```bash
git add benchmarks/bench_http_throughput.c benchmarks/CMakeLists.txt
git commit -m "bench(http): add streamable http throughput and latency benchmark"
```

---

### Task 7: WebSocket Frame & Streaming Throughput Benchmark

**Files:**
- Create: `benchmarks/bench_ws_throughput.c`
- Modify: `benchmarks/CMakeLists.txt`

**Interfaces:**
- Consumes: `bench_common.h`, `mcpkit/transport/ws.h`, `mcp_async_server`
- Produces: binary `bench_ws_throughput`

- [ ] **Step 1: Implement `benchmarks/bench_ws_throughput.c`**
Start an async server with WebSocket support on loopback, connect concurrent WS clients, exchange 50,000 text and binary frames with masking, and record frame throughput (frames/sec) and round-trip latency.

- [ ] **Step 2: Add target to `benchmarks/CMakeLists.txt`**

- [ ] **Step 3: Build and run `bench_ws_throughput`**
Run: `cmake --build build-bench --target bench_ws_throughput && ./build-bench/benchmarks/bench_ws_throughput --concurrency 20 --frames 5000`
Expected: Completes cleanly and prints frame throughput statistics.

- [ ] **Step 4: Commit**
```bash
git add benchmarks/bench_ws_throughput.c benchmarks/CMakeLists.txt
git commit -m "bench(ws): add websocket streaming throughput benchmark"
```

---

### Task 8: Process-Isolated Multi-Connection Async Server RSS & Event Latency Benchmark

**Files:**
- Create: `benchmarks/bench_async_server.c`
- Modify: `benchmarks/CMakeLists.txt`

**Interfaces:**
- Consumes: `bench_common.h`, `mcp_async_server`
- Produces: binary `bench_async_server`

- [ ] **Step 1: Implement `benchmarks/bench_async_server.c`**
1. `fork()` child process running `mcp_async_server_start()` with 500 active idle and active connections.
2. Parent process samples child `/proc/<child_pid>/statm` (resident pages * 4096 bytes) at 0, 100, 500, 1000 connections.
3. Compute RSS byte overhead per session: `(RSS_1000 - RSS_baseline) / 1000`.
4. Gracefully terminate child via `kill(pid, SIGTERM)` and wait.

- [ ] **Step 2: Add target to `benchmarks/CMakeLists.txt`**

- [ ] **Step 3: Build and run `bench_async_server`**
Run: `cmake --build build-bench --target bench_async_server && ./build-bench/benchmarks/bench_async_server --connections 100`
Expected: Prints pure server RSS overhead per connection without client pollution.

- [ ] **Step 4: Commit**
```bash
git add benchmarks/bench_async_server.c benchmarks/CMakeLists.txt
git commit -m "bench(async): add process-isolated async server rss and connection scaling benchmark"
```

---

### Task 9: Targeted Fast-Path Optimization in `streamable_http.c`

**Files:**
- Modify: `src/transport/streamable_http.c`

**Interfaces:**
- Consumes: `bench_http_throughput` baseline metrics

- [ ] **Step 1: Benchmark baseline performance**
Run `bench_http_throughput` and record baseline QPS and P99 latency.

- [ ] **Step 2: Apply stack-allocated small-buffer fast path in `src/transport/streamable_http.c`**
Optimize error response serialization and header generation by avoiding heap reallocations when headers fit in standard 512-byte stack buffers.

- [ ] **Step 3: Re-run benchmarks and verify improvement**
Verify that tests still pass 100% and throughput is improved or preserved with lower allocator churn.

- [ ] **Step 4: Commit**
```bash
git add src/transport/streamable_http.c
git commit -m "perf(http): eliminate dynamic allocations on http error and header fast-path"
```

---

### Task 10: Benchmark Suite Automation & Final Double-Gate Verification

**Files:**
- Create: `benchmarks/run_benchmarks.sh`

**Interfaces:**
- Runs all 4 benchmark binaries in `--quick` mode.

- [ ] **Step 1: Create `benchmarks/run_benchmarks.sh`**
Executable shell script that builds with `-DMCPKIT_BUILD_BENCHMARKS=ON`, executes all benchmark targets with `--quick`, and validates JSON output format.

- [ ] **Step 2: Run full regression and memory gates**
Run:
1. `cmake --build build -j && ctest --test-dir build --output-on-failure`
2. `cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure`
3. `bash benchmarks/run_benchmarks.sh --quick`
Expected: 100% pass across all gates, 0 leaks, 0 warnings.

- [ ] **Step 3: Commit**
```bash
git add benchmarks/run_benchmarks.sh
git commit -m "ci(bench): add automated benchmark driver script"
```
