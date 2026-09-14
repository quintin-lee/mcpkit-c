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

## Consequences

- Core compiles with libc only. JSON/Runtime/Transport expose
  `interface -> backend` seams; first shippable backends are static
  (`sync`, `threadpool`, builtin JSON, stdio).
- Dynamic plugins (`.so`) wait until the ABI is stable.
- MCP Apps lives strictly above Server: `ui://` resources,
  `_meta.ui.resourceUri` linkage, structured CSP, permission sets,
  lifecycle callbacks, and text fallback for non-Apps hosts.
