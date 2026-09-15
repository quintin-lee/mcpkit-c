# Adapters (future optional backends)

This directory is the landing zone for **optional, externally-dependent**
backends. Core (`mcpkit_core`) stays zero-dependency; every adapter behind
this directory is opt-in via its own `MCPKIT_BUILD_*` CMake flag and must
never be required by default builds.

An adapter implements exactly one of the existing seam interfaces —
no new core plumbing per backend:

| Seam | Header | Examples (planned, not present) |
|---|---|---|
| JSON backend | `mcpkit/json/json.h` (`mcp_json_backend_ops_t`) | yyjson |
| Transport backend | `mcpkit/transport/transport.h` (`mcp_transport_backend`) | libuv TCP, TLS via OpenSSL |
| Executor backend | `mcpkit/runtime/executor.h` (executor ops) | libuv loop, io_uring |
| HTTP client | none yet (client needs one in a later phase) | curl |

Rules for adding an adapter:

1. One subdirectory per backend (`adapters/yyjson/`, ...), self-contained.
2. Its own `MCPKIT_BUILD_<NAME>` option, default `OFF`.
3. Only headers from `include/mcpkit/` may be used; no core rewrites
   to accommodate a backend — fix the adapter instead.
4. The default build (all adapter flags `OFF`) must stay green on
   gcc + clang + ASan/UBSan.
