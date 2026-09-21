# mcpkit-c

Modular, production-grade MCP (Model Context Protocol) SDK and runtime for
C23. Zero dependencies: the core library compiles against libc only, and
pluggable backends (JSON, transport, executor) can be swapped at runtime
without changing application code. MCP Apps is an extension layer, not the
core.

## Features

- **JSON-RPC 2.0 protocol layer** — message envelope, `initialize`
  handshake with version negotiation (`2025-06-18` / `2024-11-05`),
  JSON-Schema builders and a three-level validation pipeline
  (envelope → method → params).
- **Server** — tool / resource / prompt registries with ownership
  transfer, multi-session lifecycle, per-session request-id tracking,
  and a queue-based dispatch pipeline.
- **Client** — synchronous request/response over any transport, with
  convenience methods for `tools/list`, `tools/call`,
  `resources/read`, `prompts/get`, and `ping`.
- **Transports** — stdio (line-delimited, 4 MB cap, serve loop) and
  Streamable HTTP (session map, SSE-compatible `GET` stream, optional,
  behind `MCPKIT_BUILD_HTTP=ON`).
- **Runtime** — pluggable executor: sync (inline) and C11 threadpool
  (worker pool with per-task context capture), plus a monotonic timer
  wheel and a transport-agnostic event loop.
- **MCP Apps extension** — `ui://` resources with lifecycle hooks,
  structured Content-Security-Policy (default-deny), tool visibility
  filters, and session permission masks.
- **CLI** — `mcpkit-cli` with `inspect`, `call`, `validate`, and
  `test` subcommands that spawn and drive a server process over pipes.
- **Tested** — 40+ unit / acceptance / fuzz tests under CTest,
  deterministic fuzz corpus, AddressSanitizer + UBSanitizer build presets.

## Quick start

```sh
# build with tests and examples (both ON by default)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# run the test suite
ctest --test-dir build

# try the stdio server by piping an initialize request
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"probe","version":"0"},"capabilities":{}}}' \
  | ./build/examples/stdio-server

# or use the CLI to inspect it
./build/tools/mcpkit-cli inspect ./build/examples/stdio-server
./build/tools/mcpkit-cli call ./build/examples/stdio-server echo '{"text":"hi"}'
```

See [docs/quickstart.md](docs/quickstart.md) for a minimal C server and
client you can compile against the library.

## Build options

| Option | Default | Description |
|---|---|---|
| `MCPKIT_BUILD_TESTS` | `ON` | Build unit and acceptance tests |
| `MCPKIT_BUILD_EXAMPLES` | `ON` | Build all examples |
| `MCPKIT_BUILD_TOOLS` | `ON` | Build `mcpkit-cli` |
| `MCPKIT_BUILD_HTTP` | `OFF` | Build the HTTP transport and `http-server` example |
| `MCPKIT_BUILD_FUZZ` | `OFF` | Build the `fuzz_json_stdin` driver |
| `MCPKIT_ENABLE_ASAN` | `OFF` | Add `-fsanitize=address` to all targets |
| `MCPKIT_ENABLE_UBSAN` | `OFF` | Add `-fsanitize=undefined` to all targets |

`compile_commands.json` is always exported in the build directory for
LSP / clangd.

## Examples

All examples build into `build/examples/<name>` and are driven over
stdio unless stated otherwise:

| Example | Demonstrates |
|---|---|
| `minimal-server` | Hardcoded tool + resource with a scripted demo session |
| `stdio-server` | Real `stdin`/`stdout` serve loop (the server the CLI drives) |
| `multi-session` | Two interleaved sessions on one server |
| `threadpool-server` | Serve loop with a 4-thread executor + timer |
| `client` | Client-side `list` / `call` / `ping` against a live server |
| `apps-host` | MCP Apps: mount, permission-gated tool, UI read, unmount |
| `http-server` | Streamable HTTP serve over file-backed I/O (needs `MCPKIT_BUILD_HTTP=ON`) |

## CLI

```sh
mcpkit-cli inspect <server-bin>   # list tools and resources
mcpkit-cli call <server-bin> <tool> <json-args>
mcpkit-cli validate <file>       # per-line JSON-RPC validation report
mcpkit-cli test <server-bin>     # init + ping + tools/list smoke test
```

## Project layout

```text
include/mcpkit/       public headers (core, json, protocol, server,
                      client, transport, runtime, apps, plugin)
src/                  implementation, mirrors the header tree
tests/unit/           unit tests (one binary per source area)
tests/acceptance/     end-to-end CLI acceptance script
tests/fuzz/           deterministic JSON fuzz corpus
examples/             runnable example programs
tools/mcpkit-cli/     the CLI driver
docs/                 quickstart, module reference, architecture
cmake/                options, sanitizer presets
```

## Design principles

**Layering (must not invert).**

```text
Examples -> apps -> server/client -> protocol -> core
Transport / Runtime / JSON depend inward on core only.
```

Forbidden: `core -> libuv/curl/OpenSSL`, `protocol -> HTTP`,
`apps -> pthread`.

**Ownership.** `create()` returns a handle the caller owns. After
`mcp_server_add_*()` the server owns the object and
`mcp_server_destroy()` frees it. On a successful call the container owns
the child value; on error the caller retains it. No ambiguous borrowed
pointers.

**API style.** `mcp_xxx_create/init/destroy/set/get/add/remove/
start/stop`. All functions take the context first (`ctx` may be `NULL`
for defaults). Errors are `mcp_status_t` only — never bare `return -1`.

**Zero-dependency core.** The core, JSON, stdio transport, and sync
executor use only libc. Anything else plugs in through an ops table.

## Documentation

| Doc | Description |
|-----|-------------|
| [Quickstart](docs/quickstart.md) | Minimal server + client, build options |
| [Module reference](docs/module-reference.md) | All public headers grouped by layer |
| [Architecture](docs/architecture/overview.md) | Layering, ownership, threading model |

## Toolchain baseline

GCC ≥ 13 or Clang ≥ 17, CMake ≥ 3.28, C23 (`-std=c2x` must support
`nullptr`, `bool`, `static_assert`, `<threads.h>`, `<stdatomic.h>`).

## License

MIT. See [LICENSE](LICENSE).
