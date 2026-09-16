# P1+P2: socket transport, completion, prompt example, CI, fuzz, shared-lib

> **For agentic workers:** REQUIRED: Use executing-plans to implement this plan. Steps use checkbox syntax.

**Goal:** Fill 7 specific gaps: real TCP socket transport, completion protocol methods, prompt example, CI workflow, 3 new fuzz targets, shared-lib + pkg-config.

**Architecture:** Socket transport is a new `mcp_socket_transport_create(ctx, host, port, out_is_server_mode)` that opens a `socket()`/`bind`/`accept`/`connect` pair and routes its `FILE*`-less I/O through the same `mcp_transport_ops_t` four-method table as stdio (read/write are raw `read(2)`/`write(2)` on a fd, line-framed with a `recv`-buffer accumulator, no `fgetc` overhead). Completion methods are routed in `dispatcher.c` (`completion/list` returns the registered completion providers as a static array; `completion/complete` takes `{ref:{type:"ref/prompt"/"ref/resource", ...}, arguments}` and calls the matching provider callback, or returns empty when no provider exists). The prompt example reuses `mcp_prompt_new` + `mcp_server_add_prompt` + a stdio serve loop. CI is a GitHub Actions 3-job matrix. Fuzz targets follow the existing `tests/fuzz/fuzz_json_stdin.c` stdin-driven pattern. Shared-lib adds `MCPKIT_BUILD_SHARED=OFF` option (adds a `mcpkit` shared target alongside the static `mcpkit_core`), a `cmake/mcpkit.pc.in` template, and a `mcpkit-config.cmake` install rule.

**Tech Stack:** POSIX sockets (`sys/socket.h`), C23, CMake 4.x, GitHub Actions, pkg-config.

---

## Chunk 1: Task definitions

### Task 1: P1-1 — Socket TCP transport

**Files:**
- Create: `include/mcpkit/transport/socket.h`
- Create: `src/transport/socket.c`
- Create: `examples/socket-server/main.c`
- Create: `tests/unit/test_socket.c`
- Modify: `CMakeLists.txt` (add `src/transport/socket.c` under `MCPKIT_BUILD_SOCKET=ON`)
- Modify: `cmake/MCPKitOptions.cmake` (add `MCPKIT_BUILD_SOCKET` option, default `ON`)
- Modify: `include/mcpkit/mcpkit.h` (include `transport/socket.h` under `MCPKIT_BUILD_SOCKET`)
- Modify: `tests/CMakeLists.txt` (add `test_socket` under `MCPKIT_BUILD_SOCKET`)
- Modify: `examples/CMakeLists.txt` (add `socket-server` under `MCPKIT_BUILD_SOCKET`)

**Design:**

`socket.h` API:
```c
// Client mode: connect() to host:port, returns NULL on failure.
// Server mode: listen() on port, accept() on start(), returns NULL on failure.
mcp_transport_t *mcp_socket_transport_create(mcp_context_t *ctx,
                                             const char *host_or_null,
                                             uint16_t port,
                                             bool server_mode);
```

`socket.c` internals:
```c
typedef struct {
    int  fd;
    bool server_mode;
    char read_buf[8192];
    size_t read_len;
    size_t read_cap;
} socket_backend_t;
```

- `start()`: if `server_mode`, call `accept(fd)`, store the accepted fd, return OK. If not, no-op (client already connected in `create`).
- `send()`: `write(fd, data, len)` with EINTR retry loop. Appends `\n`.
- `recv()`: accumulate `read(fd, read_buf, ...)` into a line buffer (NUL-terminated, `\n`-framed), same overflow-discard-to-EOL pattern as stdio (4MB cap → `MCP_ERR_PROTOCOL`). Returns `MCP_ERR_IO` on fd close.
- `stop()`: `close(fd)`.

Client `create`: `socket(AF_INET, SOCK_STREAM)`, `inet_pton`, `connect`, `setsockopt(TCP_NODELAY)`.
Server `create`: `socket`, `setsockopt(SO_REUSEADDR)`, `bind`, `listen`.

Test: loopback — create server on `127.0.0.1:0` (ephemeral port), get port via `getsockname`, create client, connect, send a line, recv it, verify roundtrip, close both.

### Task 2: P1-2 — completion/list + completion/complete

**Files:**
- Modify: `include/mcpkit/server/server.h` (add `mcp_completion_provider_t` typedef + `mcp_server_add_completion_provider`)
- Modify: `src/server/internals.h` (add `n_completions`, `completions[]` to `mcp_server` struct)
- Modify: `src/server/server.c` (implement `mcp_server_add_completion_provider`, destroy in `mcp_server_destroy`)
- Modify: `src/server/dispatcher.c` (add `route_completion_list`, `route_completion_complete`, wire into `route_request`)
- Modify: `include/mcpkit/client/client.h` (add `mcp_client_complete`)
- Modify: `src/client/client.c` (implement `mcp_client_complete`)
- Modify: `tests/unit/test_dispatcher.c` (add completion routing test)
- Modify: `tests/unit/test_client.c` (add completion roundtrip test)

**Design:**

Completion provider (registered by the application):
```c
typedef struct mcp_completion_provider {
    char *name;           // e.g. "prompt-args", "resource-uri"
    char *description;    // optional
    mcp_status_t (*on_complete)(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *params, void *user_data,
                                 mcp_json_value_t **result_out);
    void *user_data;
} mcp_completion_provider_t;
```

`route_completion_list`: returns `{"providers":[{"name":...,"description":...},...]}`.
`route_completion_complete`: params `{"ref":{"type":"ref/prompt"|"ref/resource","name":"..."},"arguments":{...}}`.
Looks up the matching provider by name (the `ref.name` field), calls its `on_complete`, returns the provider's result. Unknown provider → `-32601`.

Client: `mcp_client_complete(ctx, client, ref_type, ref_name, args, result_out)` sends `completion/complete` with `{"ref":{"type":ref_type,"name":ref_name},"arguments":args}`.

### Task 3: P1-3 — Prompt server example

**Files:**
- Create: `examples/prompt-server/main.c`
- Modify: `examples/CMakeLists.txt` (add `prompt-server`)

**Design:**
A stdio server that registers one prompt (`"greet"`, args schema `{name: string}`) whose handler returns a `messages` array with a single user-role message containing the greeting text. `mcp_stdio_serve` loop.

### Task 4: P2-1 — CI workflow

**Files:**
- Create: `.github/workflows/ci.yml`

**Design:**
3 jobs, all on `ubuntu-latest`:
1. `gcc`: `cmake -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_TOOLS=ON -DMCPKIT_BUILD_EXAMPLES=ON`, `cmake --build build -j$(nproc)`, `ctest --test-dir build --output-on-failure`
2. `clang`: same with `-DCMAKE_C_COMPILER=clang`
3. `asan`: same with `-DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON`, plus `ldd build/tools/mcpkit-cli | grep -E 'libasan|libubsan'`

### Task 5: P2-2 — HTTP + schema + message fuzz targets

**Files:**
- Create: `tests/fuzz/fuzz_http_stdin.c`
- Create: `tests/fuzz/fuzz_schema_stdin.c`
- Create: `tests/fuzz/fuzz_message_stdin.c`
- Create: `tests/fuzz/test_http_fuzz.c`
- Create: `tests/fuzz/test_schema_fuzz.c`
- Create: `tests/fuzz/test_message_fuzz.c`
- Modify: `tests/CMakeLists.txt` (register new fuzz tests + `MCPKIT_BUILD_FUZZ` targets)

**Design:**
Each `_stdin` target reads one line from stdin, passes it to the parser, exits 0 on valid/expected-reject, exits 1 on crash. Each deterministic test file has 20+ hardcoded cases.

- `fuzz_http_stdin.c`: `mcp_http_parse_request` on raw bytes; verify no crash, either a valid request or NULL.
- `fuzz_schema_stdin.c`: parse JSON, build schema from it, run `mcp_schema_validate` against random test values.
- `fuzz_message_stdin.c`: `mcp_message_parse` + `mcp_message_validate` (L1+L2) on raw JSON lines.

### Task 6: P2-3 — shared-lib option + .pc + config.cmake

**Files:**
- Modify: `cmake/MCPKitOptions.cmake` (add `MCPKIT_BUILD_SHARED` option, default `OFF`)
- Modify: `CMakeLists.txt` (add shared target + `.pc` generation + install config when `MCPKIT_BUILD_SHARED=ON`)
- Create: `cmake/mcpkit.pc.in`
- Create: `cmake/mcpkit-config.cmake.in`

**Design:**
When `MCPKIT_BUILD_SHARED=ON`:
```cmake
add_library(mcpkit_shared SHARED <same sources as mcpkit_core>)
target_link_libraries(mcpkit_shared PUBLIC ${CMAKE_DL_LIBS})
install(TARGETS mcpkit_shared LIBRARY DESTINATION lib)
configure_file(cmake/mcpkit.pc.in ${CMAKE_BINARY_DIR}/mcpkit.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/mcpkit.pc DESTINATION lib/pkgconfig)
configure_file(cmake/mcpkit-config.cmake.in ${CMAKE_BINARY_DIR}/mcpkit-config.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/mcpkit-config.cmake DESTINATION lib/cmake/mcpkit)
```

`mcpkit.pc.in`:
```
prefix=@CMAKE_INSTALL_PREFIX@
libdir=${prefix}/lib
includedir=${prefix}/include
Name: mcpkit
Description: MCP SDK for C
Version: @MCPKIT_VERSION@
Libs: -L${libdir} -lmcpkit
Cflags: -I${includedir}
```

---

## Chunk 2: Execution order + acceptance

**Order:** T1 → T2 → T3 → T4 → T5 → T6 → gates

Each task is a separate commit. Commit messages:
- T1: `feat(transport): add POSIX socket TCP transport`
- T2: `feat(server): add completion/list and completion/complete protocol methods`
- T3: `feat(examples): add prompt-server example`
- T4: `ci: add GitHub Actions workflow with gcc/clang/asan matrix`
- T5: `test(fuzz): add HTTP, schema, and message fuzz targets`
- T6: `build: add shared-lib option with pkg-config and CMake config`
- T7: `chore: triple gates gcc/clang/asan + changelog`

**Final gates (from clean configures):**
- `cmake -S . -B build-gcc -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_TOOLS=ON -DMCPKIT_BUILD_EXAMPLES=ON -DMCPKIT_BUILD_SOCKET=ON` → `ctest` all pass
- Same with `-DCMAKE_C_COMPILER=clang` → all pass
- Same with `-DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON` → all pass + `ldd` shows `libasan`

Remove all build dirs. Tree clean.
