# mcpkit-c

Modular, production-grade MCP SDK and runtime for C23. MCP Apps is an
extension layer, not the core.

## Layering (must not invert)

```text
Examples -> apps -> server/client -> protocol -> core
Transport/Runtime/JSON depend inward on core only.
```

Forbidden: `core -> libuv/curl/OpenSSL`, `protocol -> HTTP`, `apps -> pthread`.

## Ownership

`create()` -> caller owns. After `mcp_server_add_*()` -> server owns, and
`mcp_server_destroy()` frees everything. No unclear borrowed pointers.

## API style

`mcp_xxx_create/init/destroy/set/get/add/remove/start/stop`.
Errors use `mcp_status_t` only, never bare `return -1`.

## Build

```sh
cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build
```

Toolchain baseline: GCC >= 13 or Clang >= 17, CMake >= 3.28, C23
(`-std=c2x` must compile `nullptr`, `bool`, `static_assert`,
`<threads.h>`, `<stdatomic.h>`).

## Documentation

| Doc | Description |
|-----|-------------|
| [Quickstart](docs/quickstart.md) | Minimal server + client, build options |
| [Module reference](docs/module-reference.md) | All public headers grouped by layer |
| [Architecture](docs/architecture/overview.md) | Layering, ownership, threading model |

## License

MIT. See [LICENSE](LICENSE).
