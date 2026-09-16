# Quickstart

Minimal server and client using only the core API. No external dependencies
beyond libc.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_EXAMPLES=ON
cmake --build build -j
```

After a successful build, run any example:

```sh
./build/examples/minimal-server
./build/examples/stdio-server
./build/examples/multi-session
./build/examples/threadpool-server
./build/examples/client          # interactive client against a running server
```

## Minimal echo server (server side)

```c
#include <stdio.h>
#include "mcpkit/mcpkit.h"

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *s,
                                 const mcp_json_value_t *args, void *ud,
                                 mcp_json_value_t **result_out) {
    (void)s; (void)ud;
    const mcp_json_value_t *text = mcp_json_object_get(ctx, args, "text");
    const char *str;
    if (mcp_json_string_value(ctx, text, &str) != MCP_OK)
        return MCP_ERR_INVALID_ARGUMENT;

    /* Build result = {"content":[{"type":"text","text":<echo>}]}. */
    mcp_json_value_t *item    = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result  = mcp_json_object_new(ctx);
    mcp_json_value_t *ty      = mcp_json_string_new(ctx, "text");
    mcp_json_value_t *tv      = mcp_json_string_new(ctx, str);
    if (!item || !content || !result || !ty || !tv ||
        mcp_json_object_set(ctx, item, "type", ty) != MCP_OK ||
        mcp_json_object_set(ctx, item, "text", tv) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK ||
        mcp_json_object_set(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, ty); mcp_json_destroy(ctx, tv);
        mcp_json_destroy(ctx, item); mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);           /* defaults  */
    mcp_server_t    *s = mcp_server_create(ctx, "demo", "1.0");
    if (!ctx || !s) return 1;

    /* Register tool with input schema. */
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    mcp_schema_add_required(ctx, schema, "text");
    mcp_server_add_tool(ctx, s,
        mcp_tool_new(ctx, "echo", "Echo text", schema, echo_handler, NULL));

    /* Connect stdio transport and serve. */
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    mcp_transport_start(ctx, t);
    mcp_stdio_serve(ctx, s, t);          /* blocks until EOF on stdin  */
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
    mcp_server_destroy(ctx, s);
    mcp_context_destroy(ctx);
    return 0;
}
```

## Minimal client call

The `mcpkit-cli` tool demonstrates the full client flow. A raw C client
needs a transport (e.g. pipes to a spawned server); see
`tools/mcpkit-cli/main.c` for the spawn + fdopen wiring and
`examples/client/` for the client-side API usage:

```c
#include "mcpkit/mcpkit.h"

mcp_transport_t *t = mcp_stdio_transport_create(ctx, file_in, file_out);
mcp_client_t    *c = mcp_client_create(ctx, t);
mcp_client_connect(ctx, c);
mcp_client_initialize(ctx, c, "demo-client", mcpkit_version_string(), NULL);

mcp_json_value_t *args =
    mcp_json_parse(ctx, "{\"text\":\"hello\"}", 15);
mcp_json_value_t *result = NULL;
mcp_client_call_tool(ctx, c, "echo", args, &result);
if (result) {
    char *out = mcp_json_serialize(ctx, result);
    if (out) { puts(out); mcp_json_free_string(ctx, out); }
    mcp_json_destroy(ctx, result);
}
mcp_client_destroy(ctx, c);
mcp_transport_destroy(ctx, t);
```

> `args` is owned by the client on every failure path (no leak); on
> success the client takes it and `result` is caller-owned.

## CMake options

| Option                     | Default | Description                          |
|----------------------------|---------|--------------------------------------|
| `MCPKIT_BUILD_TESTS`       | `ON`    | Build unit + acceptance tests        |
| `MCPKIT_BUILD_EXAMPLES`    | `OFF`   | Build all examples                   |
| `MCPKIT_BUILD_TOOLS`       | `ON`    | Build `mcpkit-cli`                   |
| `MCPKIT_BUILD_HTTP`        | `OFF`   | Build HTTP transport + http example  |
| `MCPKIT_BUILD_FUZZ`        | `OFF`   | Build fuzz-stdin target              |
| `MCPKIT_ENABLE_ASAN`       | `OFF`   | Add AddressSanitizer flags           |
| `MCPKIT_ENABLE_UBSAN`      | `OFF`   | Add UndefinedBehaviorSanitizer flags |
