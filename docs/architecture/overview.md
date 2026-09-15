# Architecture overview

## Separation

Protocol, Runtime, and Transport are three independent axes.
`mcp_server_t` never names `uv_loop_t`, `pthread_t`, or any HTTP type.

## Dependency direction

```text
                        Examples
                            |
                        mcpkit-apps
                            |
                     Server / Client
                            |
                         Protocol
                            |
                           Core
                            |
         Transport + Runtime + JSON (inward only)
```

Each layer sees only its immediate inward dependency. Apps sits above
Server; Server sits above Protocol; Protocol sits above Core. Transport
and Runtime are sibling backends plugged into the Core interface seams.

## Core constraints

- Core compiles with libc only. No third-party dependencies.
- JSON, Transport, and Runtime expose `interface -> backend` seams.
  First shippable backends are static: `sync`, `threadpool`, builtin JSON,
  `stdio`.
- Dynamic plugins (`.so` / shared objects) are deferred until the ABI is
  stable. The static registry in `mcpkit/plugin/plugin.h` is the interim
  landing zone.
- All allocation routes through `mcp_context_t`. Passing `ctx == NULL`
  falls back to the default libc allocator.

## Ownership rules

- `mcp_xxx_create()` returns a handle owned by the caller.
- `mcp_server_add_*()` transfers ownership to the server;
  `mcp_server_destroy()` frees everything registered.
- On a successful call, the container owns the child value.
  On any error, the caller retains ownership of the argument.

## MCP Apps

MCP Apps lives strictly above Server. The extension surface is:
- `ui://` UI resources with lifecycle callbacks (`on_mount` / `on_unmount`)
- `_meta.ui.resourceUri` stamping in tool call results
- Structured Content-Security-Policy (default-deny, 4-domain)
- Tool visibility filters (`MODEL` / `APP` / `BOTH`)
- Session permission masks
- Text fallback for non-Apps hosts

Server headers carry the visibility enum and session permission masks as
typeless `uint32_t` values so the server layer never includes an Apps
header. Apps modules build on top of that seam.

## Threading model

The dispatcher is single-threaded by design. All protocol validation,
routing, and tool dispatch happens on the calling thread. The
`threadpool` executor runs tool handler callbacks concurrently; the main
thread waits at each iteration before re-dispatching, ensuring session
state is never accessed from two threads simultaneously.

## Documentation

- [Quickstart](../quickstart.md) — minimal server and client, build options
- [Module reference](../module-reference.md) — all public headers by layer
