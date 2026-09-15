# Module Reference

Every public header in `include/mcpkit/`, grouped by layer. Each entry lists
the key types and functions you will use most often; read the header for the
full contract.

## Core (`mcpkit/core/`)

### `types.h`
Opaque handles: `mcp_context_t`, `mcp_json_value_t`, `mcp_json_backend_ops_t`,
`mcp_tool_t`, `mcp_resource_t`, `mcp_prompt_t`, `mcp_session_t`,
`mcp_server_t`, `mcp_transport_t`, `mcp_client_t`, `mcp_executor_t`,
`mcp_timer_t`, `mcp_loop_t`, `mcp_csp_t`, `mcp_ui_resource_t`,
`mcp_idset_t`, `mcp_schema_builder_t` (internal only).

Allocator struct (`mcp_allocator_t`):
```c
typedef struct {
    void *(*malloc_fn)(size_t, void *);
    void *(*realloc_fn)(void *, size_t, void *);
    void (*free_fn)(void *, void *);
    void *userdata;
} mcp_allocator_t;
```
`mcp_default_allocator()` returns a static struct backed by libc `malloc`.

### `error.h`
`mcp_status_t` — 11 codes:
`MCP_OK=0, MCP_ERR_NOMEM=1, MCP_ERR_INVALID_ARGUMENT=2, MCP_ERR_IO=3,
MCP_ERR_PROTOCOL=4, MCP_ERR_UNSUPPORTED=5, MCP_ERR_NOMEM=1 …`
plus `MCP_ERR_NOT_FOUND`, `MCP_ERR_ALREADY_EXISTS`,
`MCP_ERR_SESSION_NOT_INITIALIZED`, `MCP_ERR_PERMISSION_DENIED`,
`MCP_ERR_INTERNAL`.
`mcp_status_string()` returns a human-readable name for any value, including
out-of-range (returns `"MCP_ERR_UNKNOWN"`).

### `result.h`
Convenience macros: `MCP_TRY(expr)` (returns early on non-OK),
`MCP_ASSIGN(out, expr)` (stores on OK, returns err on failure).

### `version.h`
`mcpkit_version_string()` → the build-time version macro, sourced from the
`VERSION` file at CMake configure time via a generated header.

### `capability.h`
`mcp_capability_t` — bitmask for server capabilities (tools, resources,
prompts).

### `context.h`
`mcp_context_config_t` — `{ allocator, logger, json_backend }` (any field
may be NULL; defaults are used).
```c
mcp_context_t *mcp_context_create(const mcp_context_config_t *config);
void           mcp_context_destroy(mcp_context_t *ctx);
const mcp_allocator_t *mcp_context_allocator(mcp_context_t *ctx);
mcp_logger_t    *mcp_context_logger(mcp_context_t *ctx);
```
A context is the implicit first argument to almost every API call.
Pass `NULL` to use all defaults (libc allocator, no logger, builtin JSON).

---

## Logging (`mcpkit/logging/`)

### `log.h`
`mcp_log_level_t`: `MCP_LOG_ERROR=0, MCP_LOG_WARN=1, MCP_LOG_INFO=2,
MCP_LOG_DEBUG=3`.
`mcp_log_fn` — sink callback `(level, tag, message, userdata)`.

### `logger.h`
```c
mcp_logger_t *mcp_logger_create(mcp_context_t *ctx, mcp_log_level_t max_level);
void          mcp_logger_destroy(mcp_context_t *ctx, mcp_logger_t *l);
void          mcp_log(mcp_logger_t *l, mcp_log_level_t level,
                      const char *fmt, ...);
void          mcp_logger_set_sink(mcp_logger_t *l, mcp_log_fn fn, void *ud);
```
Default sink writes to stderr; replace via `mcp_logger_set_sink`.

---

## JSON (`mcpkit/json/`)

All functions take `ctx` as first argument. `ctx == NULL` → builtin JSON
backend. NULL backend pointer → builtin.

### `value.h`
Constructors (each returns a caller-owned value or NULL on OOM):
```c
mcp_json_value_t *mcp_json_null_new(ctx);
mcp_json_value_t *mcp_json_bool_new(ctx, bool);
mcp_json_value_t *mcp_json_number_new(ctx, double);   /* NaN/Inf rejected */
mcp_json_value_t *mcp_json_string_new(ctx, const char *);
mcp_json_value_t *mcp_json_string_new_n(ctx, const char *, size_t);
mcp_json_value_t *mcp_json_object_new(ctx);
mcp_json_value_t *mcp_json_array_new(ctx);
```
Accessors:
```c
mcp_json_kind_t mcp_json_kind(ctx, const mcp_json_value_t *);
int             mcp_json_string_value(ctx, const mcp_json_value_t *, const char **);
int             mcp_json_number_value(ctx, const mcp_json_value_t *, double *);
int             mcp_json_bool_value(ctx, const mcp_json_value_t *, bool *);
void            mcp_json_destroy(ctx, mcp_json_value_t *);   /* frees tree */
mcp_json_value_t *mcp_json_clone(ctx, const mcp_json_value_t *);
```

### `object.h`
```c
int             mcp_json_object_get(ctx, mcp_json_value_t *obj, const char *key,
                                    mcp_json_value_t **out);  /* borrowed */
int             mcp_json_object_set(ctx, mcp_json_value_t *obj, const char *key,
                                    mcp_json_value_t *val);
/* container takes ownership of val on OK; caller keeps it on ERR */
int             mcp_json_object_key_at(ctx, mcp_json_value_t *obj, size_t i,
                                       const char **key, size_t *len);
```

### `array.h`
```c
int             mcp_json_array_append(ctx, mcp_json_value_t *arr, mcp_json_value_t *val);
int             mcp_json_array_at(ctx, const mcp_json_value_t *arr, size_t i,
                                   mcp_json_value_t **out);  /* borrowed */
size_t          mcp_json_array_size(ctx, const mcp_json_value_t *arr);
```

### `json.h`
Parse / serialize / backend ops:
```c
mcp_json_value_t *mcp_json_parse(ctx, const char *text, size_t len);
char             *mcp_json_serialize(ctx, const mcp_json_value_t *);  /* owned */
void              mcp_json_free_string(ctx, char *s);                  /* same ctx */
/* backend ops table (mcp_json_backend_ops_t) — swap via mcp_json_set_backend */
void              mcp_json_set_backend(ctx, const mcp_json_backend_ops_t *ops);
#define MCP_JSON_MAX_DEPTH 128
```
All JSON functions route allocation through `ctx`'s allocator; NULL ctx
falls back to libc.

### `schema.h`
Builders producing standard JSON-Schema DOM:
```c
mcp_json_value_t *mcp_schema_object_new(ctx);
mcp_json_value_t *mcp_schema_string_new(ctx);
mcp_json_value_t *mcp_schema_number_new(ctx);
mcp_json_value_t *mcp_schema_integer_new(ctx);
mcp_json_value_t *mcp_schema_boolean_new(ctx);
mcp_json_value_t *mcp_schema_array_new(ctx);
mcp_json_value_t *mcp_schema_null_new(ctx);
int               mcp_schema_add_property(ctx, schema, key, subschema);
int               mcp_schema_add_required(ctx, schema, key);
int               mcp_schema_add_min(ctx, schema, double);
int               mcp_schema_add_max(ctx, schema, double);
int               mcp_schema_add_enum(ctx, schema, value);
/* validate — returns MCP_OK or MCP_ERR_INVALID_ARGUMENT */
int               mcp_schema_validate(ctx, const mcp_json_value_t *schema,
                                       const mcp_json_value_t *instance);
```

---

## Protocol (`mcpkit/protocol/`)

### `message.h`
Opaque `mcp_message_t` wrapping a JSON DOM. Three kinds:
request, response, notification.

```c
/* Builders */
mcp_message_t *mcp_request_new_number_id(ctx, double id, const char *method,
                                         mcp_json_value_t *params);
mcp_message_t *mcp_request_new_string_id(ctx, const char *id, const char *method,
                                         mcp_json_value_t *params);
mcp_message_t *mcp_response_new_number_id(ctx, double id, mcp_json_value_t *result);
mcp_message_t *mcp_response_err_new(ctx, double id, int code, const char *message,
                                    mcp_json_value_t *data);
mcp_message_t *mcp_notification_new(ctx, const char *method, mcp_json_value_t *params);

/* Lifecycle */
void           mcp_message_destroy(ctx, mcp_message_t *);
char          *mcp_message_serialize(ctx, const mcp_message_t *);  /* owned */
mcp_message_t *mcp_message_parse(ctx, const char *text, size_t len); /* NULL on fail */

/* Accessors (return MCP_OK on success) */
mcp_msg_kind_t mcp_message_kind(ctx, const mcp_message_t *);
int            mcp_message_id_number(ctx, const mcp_message_t *, double *out);
int            mcp_message_id_string(ctx, const mcp_message_t *, const char **out);
int            mcp_message_method(ctx, const mcp_message_t *, const char **out);
int            mcp_message_error_code(ctx, const mcp_message_t *, int *out);
int            mcp_message_error_message(ctx, const mcp_message_t *, const char **out);
const mcp_json_value_t *mcp_message_result(ctx, const mcp_message_t *);
const char    *mcp_message_jsonrpc(ctx, const mcp_message_t *);  /* "2.0" */
```

RPC status codes used in `mcp_response_err_new` and `mcp_message_error_code`:
| Constant | Value | Meaning |
|---|---|---|
| `MCP_RPC_PARSE_ERROR` | -32700 | invalid JSON |
| `MCP_RPC_INVALID_REQUEST` | -32600 | malformed request |
| `MCP_RPC_METHOD_NOT_FOUND` | -32601 | unknown method |
| `MCP_RPC_INVALID_PARAMS` | -32602 | params failed validation |
| `MCP_RPC_INTERNAL_ERROR` | -32603 | server-internal failure |
| `MCP_RPC_SERVER_ERROR` | -32000 | custom server error range |

### `initialize.h`
```c
mcp_json_value_t *mcp_initialize_params_new_v(ctx, const char *protocol_version,
                                              const char *client_name,
                                              const char *client_version);
/* returns the result JSON; caller destroys */
int              mcp_initialize_params_validate(ctx, const mcp_json_value_t *params);
/* negotiate returns the chosen version string (strdup, caller frees) */
const char      *mcp_protocol_negotiate(const char *client_version);
#define MCP_PROTOCOL_VERSION_LATEST "2025-06-18"
#define MCP_PROTOCOL_VERSION_2024   "2024-11-05"
mcp_message_t    *mcp_initialized_notification_new(ctx);
```

### `validate.h`
Three-level pipeline. All functions return `mcp_status_t`.

```c
/* L1: JSON-RPC envelope — jsonrpc=="2.0", exactly one of result/error */
int mcp_message_validate_envelope(ctx, const mcp_message_t *msg);

/* L2: known method name (25-method table; responses skip this level) */
int mcp_message_validate_method(ctx, const mcp_message_t *msg);

/* L3: per-method params rules (incl. full initialize params validation) */
int mcp_message_validate_params(ctx, const mcp_message_t *msg);

/* Convenience: run all three in order */
int mcp_message_validate(ctx, const mcp_message_t *msg);
```

---

## Server (`mcpkit/server/`)

### `server.h`
```c
mcp_server_t *mcp_server_create(mcp_context_t *ctx, const char *name,
                                const char *version);
void          mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *s);

/* Ownership: after add_*, the server owns the object; destroy frees it. */
int           mcp_server_add_tool(ctx, s, mcp_tool_t *);
int           mcp_server_add_resource(ctx, s, mcp_resource_t *);
int           mcp_server_add_prompt(ctx, s, mcp_prompt_t *);

mcp_session_t *mcp_server_create_session(ctx, s);
void           mcp_server_destroy_session(ctx, s, mcp_session_t *);

/* Core dispatch: validates, routes, fills resp (caller destroys on OK). */
int           mcp_server_dispatch(ctx, s, session, mcp_message_t *req,
                                  mcp_message_t **resp_out);
int           mcp_server_notify(ctx, s, session, mcp_message_t *notif);
```

### `tool.h`
```c
mcp_tool_t *mcp_tool_new(ctx, const char *name, const char *desc,
                         mcp_json_value_t *input_schema,
                         mcp_tool_handler_fn handler, void *user_data);
/* handler signature */
typedef mcp_status_t (*mcp_tool_handler_fn)(
    mcp_context_t *ctx, mcp_session_t *session,
    const mcp_json_value_t *args, void *user_data,
    mcp_json_value_t **result_out);
void          mcp_tool_destroy(ctx, mcp_tool_t *);

/* Visibility (MCP Apps layer; default = BOTH) */
typedef enum { MCP_VIS_MODEL=1, MCP_VIS_APP=2, MCP_VIS_BOTH=3 } mcp_tool_visibility_t;
int           mcp_tool_set_visibility(ctx, mcp_tool_t *, mcp_tool_visibility_t);
int           mcp_tool_require_perms(ctx, mcp_tool_t *, uint32_t perm_mask);
```

### `resource.h` / `prompt.h`
```c
mcp_resource_t *mcp_resource_new(ctx, const char *uri, const char *name,
                                 mcp_resource_handler_fn handler, void *ud);
void            mcp_resource_destroy(ctx, mcp_resource_t *);
int             mcp_resource_set_cleanup(ctx, mcp_resource_t *,
                                         mcp_resource_cleanup_fn cleanup);
/* handler: (ctx, session, uri, user_data, mcp_json_value_t **result) */

mcp_prompt_t   *mcp_prompt_new(ctx, const char *name, const char *desc,
                               mcp_json_value_t *args_schema,
                               mcp_prompt_handler_fn handler, void *ud);
void           mcp_prompt_destroy(ctx, mcp_prompt_t *);
```

### `session.h`
```c
/* Apps-host flag: affects tool visibility filter in tools/list */
int  mcp_session_set_apps_host(ctx, mcp_session_t *, int flag);
int  mcp_session_is_apps_host(ctx, const mcp_session_t *);
/* Permission grants (mask semantics: 0 = none; all-bits = all granted) */
int  mcp_session_grant(ctx, mcp_session_t *, uint32_t perm_mask);
int  mcp_session_revoke(ctx, mcp_session_t *, uint32_t perm_mask);
int  mcp_session_grants(ctx, const mcp_session_t *, uint32_t perm_mask); /* nonzero if any */
```

### `dispatcher.h`
Queue-based dispatch for multi-session servers.
```c
mcp_dispatcher_t *mcp_dispatcher_create(ctx, mcp_server_t *s);
void              mcp_dispatcher_destroy(ctx, mcp_dispatcher_t *d);
int               mcp_dispatcher_submit(ctx, mcp_dispatcher_t *d,
                                        mcp_message_t *msg);
/* process_one consumes one queued message; returns MCP_ERR_NOT_FOUND when empty */
int               mcp_dispatcher_process_one(ctx, mcp_dispatcher_t *d,
                                              mcp_message_t **msg_out);
```

---

## Transport (`mcpkit/transport/`)

### `transport.h`
Four-method interface. All backends implement these:
```c
typedef struct {
    int (*start)(mcp_context_t *, mcp_transport_t *);
    int (*send) (mcp_context_t *, mcp_transport_t *, const char *, size_t);
    int (*recv) (mcp_context_t *, mcp_transport_t *, char **line_out);
    int (*stop) (mcp_context_t *, mcp_transport_t *);
} mcp_transport_ops_t;

mcp_transport_t *mcp_transport_create(ctx, const mcp_transport_ops_t *ops,
                                      void *backend);
void             mcp_transport_destroy(ctx, mcp_transport_t *t);
int              mcp_transport_start(ctx, mcp_transport_t *t);
int              mcp_transport_stop(ctx, mcp_transport_t *t);
int              mcp_transport_send(ctx, mcp_transport_t *t,
                                    const char *data, size_t len);
int              mcp_transport_recv(ctx, mcp_transport_t *t, char **line_out);
void            *mcp_transport_backend(ctx, mcp_transport_t *t);
```
`recv` returns `MCP_ERR_IO` when the input stream is at EOF and there is no
incomplete line. `send` is fire-and-forget on stdio (line-buffered); on
HTTP it appends to the request buffer.

### `stdio.h`
```c
mcp_transport_t *mcp_stdio_transport_create(ctx, FILE *in, FILE *out);
/* NULL in/out → uses stdin/stdout */
int              mcp_stdio_serve(ctx, mcp_server_t *, mcp_transport_t *t);
```
Framing: one JSON-RPC message per line (`\n`-terminated, `\r` stripped).
Oversized lines (> `MCP_PROTOCOL_MAX_MESSAGE_BYTES` = 4 MB) are discarded
and `recv` returns `MCP_ERR_PROTOCOL`.

### `http.h`
Buffer-level HTTP/1.1 parser/builder (no sockets).
```c
mcp_http_request_t *mcp_http_request_parse(ctx, const char *buf, size_t len);
mcp_http_response_t *mcp_http_response_new(ctx, int status, const char *reason);
int                  mcp_http_response_send(ctx, mcp_http_response_t *,
                                            FILE *out);
void                 mcp_http_request_destroy(ctx, mcp_http_request_t *);
void                 mcp_http_response_destroy(ctx, mcp_http_response_t *);
```

### `streamable_http.h`
Session-management serve loop over `mcp_http_io` (read/write callbacks):
```c
typedef struct {
    int (*read)(void *ud, char *buf, size_t cap, size_t *got);
    int (*write)(void *ud, const char *buf, size_t len);
} mcp_http_io;

int mcp_http_serve(ctx, mcp_server_t *, mcp_http_io *io, void *io_ud);
/* Wraps a response body as an SSE-compatible event stream */
int mcp_sse_wrap(ctx, const char *body, char **sse_out);
```
Accepts `POST` (JSON-RPC body), `GET` (SSE stream or 405), `DELETE`
(session teardown). Sessions are loop-local (up to 16, `sess-N` ids);
destroyed when the loop exits.

---

## Client (`mcpkit/client/`)

### `client.h`
Synchronous client over any `mcp_transport_t`. Caller owns all returned
JSON values; client takes `params` on every failure path (no leaks).

```c
mcp_client_t *mcp_client_create(ctx, mcp_transport_t *t);
void         mcp_client_destroy(ctx, mcp_client_t *c);
int          mcp_client_connect(ctx, mcp_client_t *c);
int          mcp_client_disconnect(ctx, mcp_client_t *c);

/* initialize: fire-and-forget initialized notification; returns send status */
int          mcp_client_initialize(ctx, c, const char *client_name,
                                   const char *client_version,
                                   mcp_json_value_t **server_info_out);

/* Convenience methods — each takes params (client takes ownership on failure),
   returns result (caller destroys). */
int          mcp_client_ping(ctx, c);
int          mcp_client_list_tools(ctx, c, mcp_json_value_t **result_out);
int          mcp_client_call_tool(ctx, c, const char *name,
                                  mcp_json_value_t *args,
                                  mcp_json_value_t **result_out);
int          mcp_client_read_resource(ctx, c, const char *uri,
                                      mcp_json_value_t **result_out);
int          mcp_client_get_prompt(ctx, c, const char *name,
                                   mcp_json_value_t *args,
                                   mcp_json_value_t **result_out);

/* Low-level request: any method; result_out may be NULL */
int          mcp_client_request(ctx, c, const char *method,
                                mcp_json_value_t *params,
                                mcp_json_value_t **result_out);

const char  *mcp_client_protocol_version(ctx, const mcp_client_t *c);
```

---

## Runtime (`mcpkit/runtime/`)

### `task.h`
```c
typedef void (*mcp_task_fn)(mcp_context_t *ctx, void *arg);
```

### `executor.h`
Three-slot backend ops: `{ submit, wait, destroy_backend }`.
```c
mcp_executor_t *mcp_executor_create(ctx, const mcp_executor_ops_t *ops,
                                    void *backend);
void            mcp_executor_destroy(ctx, mcp_executor_t *ex);
int             mcp_executor_submit(ctx, ex, mcp_task_fn fn, void *arg);
int             mcp_executor_wait(ctx, ex);   /* blocks until drained */
```

### `sync.h`
Single-thread executor. `submit` runs the task inline; `wait` is a no-op.
```c
mcp_executor_t *mcp_sync_executor_create(ctx);
```

### `threadpool.h`
C11 `<threads.h>` backed. `0` threads → returns NULL.
Destroy without wait discards pending tasks.
```c
mcp_executor_t *mcp_threadpool_create(ctx, unsigned int n_threads);
/* task runs with the ctx captured at submit time (per-node) */
```

### `timer.h`
Monotonic (`CLOCK_MONOTONIC`) timer wheel.
```c
mcp_timer_t  *mcp_timer_create(ctx);
void          mcp_timer_destroy(ctx, mcp_timer_t *t);
int           mcp_timer_schedule(ctx, t, unsigned ms, mcp_task_fn fn, void *arg);
int           mcp_timer_poll(ctx, t);   /* runs all due tasks in FIFO order */
int           mcp_timer_cancel(ctx, t, mcp_task_fn fn, void *arg);
```

### `loop.h`
Transport-agnostic event loop combining recv, timer, and executor:
```c
int mcp_loop_run(ctx, mcp_server_t *srv, mcp_transport_t *t,
                 mcp_executor_t *ex_or_null, mcp_timer_t *timer_or_null);
/* Returns MCP_ERR_NOT_FOUND on EOF; aborts on send/alloc failure */
```
When `ex_or_null` is non-NULL, each dispatch job is submitted to the
executor and the loop waits before the next iteration (sequential,
session-safe). When `timer_or_null` is non-NULL, `poll` is called before
every recv.

---

## Apps (`mcpkit/apps/`)

### `csp.h`
Default-deny 4-domain Content-Security-Policy builder.
```c
mcp_csp_t *mcp_csp_create(ctx);
void       mcp_csp_destroy(ctx, mcp_csp_t *csp);
int        mcp_csp_set(ctx, csp, mcp_csp_domain_t domain, const char *directive);
int        mcp_csp_clear(ctx, csp, mcp_csp_domain_t domain);
char      *mcp_csp_serialize(ctx, csp);  /* owned string */
```
Domains: `MCP_CSP_DEFAULT`, `MCP_CSP_SCRIPT`, `MCP_CSP_STYLE`,
`MCP_CSP_CONNECT`. Default = `none` (deny all).

### `ui.h`
`ui://` UI resource with optional lifecycle hooks and `_meta.ui` stamping.
```c
mcp_ui_resource_t *mcp_ui_resource_create(ctx,
    const char *uri,          /* must start with "ui://" */
    const char *html,
    const mcp_csp_t *csp,    /* NULL → default-deny snapshot */
    mcp_ui_on_mount_fn on_mount,       /* called when host mounts */
    mcp_ui_on_unmount_fn on_unmount,   /* called when host unmounts */
    void *user_data);

/* Result helper: overwrites _meta.ui.resourceUri in the result object */
int mcp_ui_result_with_ui(ctx, mcp_json_value_t *result, const char *uri);

/* Stamps the resource contents array into a resources/read result */
int mcp_ui_resource_read_result(ctx, mcp_ui_resource_t *res,
                                mcp_json_value_t **result_out);
```

---

## Plugin (`mcpkit/plugin/`)

Static global registry (32 entries, kind + name deduped).
```c
typedef struct {
    const char *name;
    const char *kind;          /* "json", "transport", "executor" */
    void       *impl;
} mcp_plugin_entry_t;

int mcp_plugin_register(const mcp_plugin_entry_t *entry);
int mcp_plugin_unregister(const char *kind, const char *name);
const mcp_plugin_entry_t *mcp_plugin_get(const char *kind, const char *name);
```
No ctx — global (matches the `mcp_default_allocator` precedent).
