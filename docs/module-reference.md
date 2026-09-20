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
`MCP_OK=0, MCP_ERR_INVALID_ARGUMENT, MCP_ERR_NOMEM, MCP_ERR_IO,
MCP_ERR_PROTOCOL, MCP_ERR_TIMEOUT, MCP_ERR_CANCELLED, MCP_ERR_NOT_FOUND,
MCP_ERR_ALREADY_EXISTS, MCP_ERR_UNSUPPORTED, MCP_ERR_PERMISSION`.
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

### `shutdown.h`
Process-wide graceful-shutdown flag (a `volatile sig_atomic_t`; safe to set
from a signal handler, never blocks):
```c
void mcp_request_shutdown(void);   /* e.g. from SIGTERM/SIGINT handler */
bool mcp_shutdown_requested(void);
void mcp_shutdown_clear(void);     /* host calls explicitly before a run */
```
`mcp_loop_run` and `mcp_stdio_serve` drain in-flight requests and return
`MCP_ERR_CANCELLED` once the flag is set. Pair with a finite recv timeout
so an idle loop wakes up promptly.

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
/* printf-style convenience (truncated to 256 bytes incl. NUL) */
void          mcp_logger_logf(mcp_logger_t *logger, mcp_log_level_t level,
                              const char *fmt, ...);
```
Default sink writes to stderr; replace via `mcp_logger_set_sink`.
The dispatch pipeline logs rejects at WARN/ERROR and lifecycle events at
INFO; logger is never locked — the host owns thread safety.

---

## JSON (`mcpkit/json/`)

All functions take `ctx` as first argument. `ctx == NULL` → builtin JSON
backend. NULL backend pointer → builtin.

### `value.h`
Kind enum (`mcp_json_type_t`): `MCP_JSON_NULL, MCP_JSON_BOOL,
MCP_JSON_NUMBER, MCP_JSON_STRING, MCP_JSON_ARRAY, MCP_JSON_OBJECT`.

Constructors (each returns a caller-owned value or NULL on OOM):
```c
mcp_json_value_t *mcp_json_null_new(ctx);
mcp_json_value_t *mcp_json_bool_new(ctx, bool);
mcp_json_value_t *mcp_json_number_new(ctx, double);   /* NaN/Inf rejected */
mcp_json_value_t *mcp_json_string_new(ctx, const char *);
mcp_json_value_t *mcp_json_string_new_n(ctx, const char *, size_t);
mcp_json_value_t *mcp_json_array_new(ctx);
mcp_json_value_t *mcp_json_object_new(ctx);

/* Deep copy through the active backend */
mcp_json_value_t *mcp_json_clone(ctx, const mcp_json_value_t *);
void              mcp_json_destroy(ctx, mcp_json_value_t *);   /* frees tree */

/* Accessors: MCP_OK on kind match, MCP_ERR_INVALID_ARGUMENT otherwise */
mcp_json_type_t   mcp_json_type(ctx, const mcp_json_value_t *);
mcp_status_t      mcp_json_bool_value(ctx, const mcp_json_value_t *, bool *out);
mcp_status_t      mcp_json_number_value(ctx, const mcp_json_value_t *, double *out);
mcp_status_t      mcp_json_string_value(ctx, const mcp_json_value_t *, const char **out);
```

### `object.h`
```c
/* container takes ownership of val on OK; caller keeps it on ERR */
mcp_status_t          mcp_json_object_set(ctx, mcp_json_value_t *obj,
                                          const char *key, mcp_json_value_t *val);
/* take variant: val consumed on ALL paths (destroyed on ERR, obj stays alive) */
mcp_status_t          mcp_json_object_set_take(ctx, mcp_json_value_t *obj,
                                          const char *key, mcp_json_value_t *val);
const mcp_json_value_t *mcp_json_object_get(ctx, const mcp_json_value_t *obj,
                                             const char *key);   /* borrowed */
bool                  mcp_json_object_has(ctx, const mcp_json_value_t *obj,
                                           const char *key);
size_t                mcp_json_object_size(ctx, const mcp_json_value_t *obj);
const char           *mcp_json_object_key_at(ctx, const mcp_json_value_t *obj,
                                              size_t i);   /* borrowed, NULL past end */
```

### `array.h`
```c
/* container takes ownership of val on OK; caller keeps it on ERR */
mcp_status_t              mcp_json_array_append(ctx, mcp_json_value_t *arr,
                                                mcp_json_value_t *val);
const mcp_json_value_t   *mcp_json_array_get(ctx, const mcp_json_value_t *arr,
                                              size_t i);   /* borrowed */
size_t                    mcp_json_array_size(ctx, const mcp_json_value_t *arr);
```

### `json.h`
Parse / serialize / backend ops:
```c
mcp_json_value_t *mcp_json_parse(ctx, const char *text, size_t len);
char             *mcp_json_serialize(ctx, const mcp_json_value_t *);  /* owned */
void              mcp_json_free_string(ctx, char *s);                  /* same ctx */
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
mcp_json_value_t *mcp_schema_array_new(ctx, mcp_json_value_t *items);

int               mcp_schema_add_property(ctx, schema, name, subschema);
int               mcp_schema_add_required(ctx, schema, name);
int               mcp_schema_add_enum(ctx, schema, mcp_json_value_t *values);
int               mcp_schema_set_minimum(ctx, schema, double);
int               mcp_schema_set_maximum(ctx, schema, double);
int               mcp_schema_set_description(ctx, schema, const char *);

/* MCP_OK on match, MCP_ERR_INVALID_ARGUMENT on mismatch */
int               mcp_schema_validate(ctx, const mcp_json_value_t *schema,
                                       const mcp_json_value_t *instance);
/* same, plus a human-readable failure path on mismatch */
int               mcp_schema_validate_verbose(ctx, schema, instance, char **path_out);
```

---

## Protocol (`mcpkit/protocol/`)

### `message.h`
Opaque `mcp_message_t` wrapping a JSON DOM. Kinds (`mcp_msg_kind_t`):
`MCP_MSG_INVALID / MCP_MSG_REQUEST / MCP_MSG_NOTIFICATION / MCP_MSG_RESPONSE`.

```c
/* Builders */
mcp_message_t *mcp_request_new_number_id(ctx, double id, const char *method,
                                         mcp_json_value_t *params);
mcp_message_t *mcp_request_new_string_id(ctx, const char *id, const char *method,
                                         mcp_json_value_t *params);
mcp_message_t *mcp_response_ok_new(ctx, const mcp_message_t *req,
                                   mcp_json_value_t *result);
mcp_message_t *mcp_response_err_new(ctx, const mcp_message_t *req_or_null,
                                    int code, const char *message,
                                    mcp_json_value_t *data);
mcp_message_t *mcp_notification_new(ctx, const char *method,
                                    mcp_json_value_t *params);

/* Lifecycle */
void           mcp_message_destroy(ctx, mcp_message_t *);
char          *mcp_message_serialize(ctx, const mcp_message_t *);  /* owned */
mcp_message_t *mcp_message_parse(ctx, const char *text, size_t len); /* NULL on fail */

/* Accessors */
mcp_msg_kind_t mcp_message_kind(ctx, const mcp_message_t *);
const char    *mcp_message_jsonrpc(ctx, const mcp_message_t *);
const char    *mcp_message_method(ctx, const mcp_message_t *);
const mcp_json_value_t *mcp_message_params(ctx, const mcp_message_t *);
mcp_id_type_t  mcp_message_id_type(ctx, const mcp_message_t *);
const char    *mcp_message_id_string(ctx, const mcp_message_t *);
int            mcp_message_id_number(ctx, const mcp_message_t *, double *out);
const mcp_json_value_t *mcp_message_result(ctx, const mcp_message_t *);
int            mcp_message_error_code(ctx, const mcp_message_t *, int *out);
const char    *mcp_message_error_text(ctx, const mcp_message_t *);
```

RPC status codes used in `mcp_response_err_new` and `mcp_message_error_code`:
| Constant | Value | Meaning |
|---|---|---|
| `MCP_RPC_PARSE_ERROR` | -32700 | invalid JSON |
| `MCP_RPC_INVALID_REQUEST` | -32600 | malformed request |
| `MCP_RPC_METHOD_NOT_FOUND` | -32601 | unknown method |
| `MCP_RPC_INVALID_PARAMS` | -32602 | params failed validation |
| `MCP_RPC_INTERNAL_ERROR` | -32603 | server-internal failure |

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

/* L2: known method name (shared 15-method server table + 4 spec-known
   non-routed names: roots/list, roots/list_changed, sampling/createMessage,
   elicitation/create; responses skip this level). A REQUEST for one of the
   non-routed names passes L2 and is answered -32601 by the dispatcher;
   NOTIFICATION kind is consumed by mcp_server_notify. */
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

/* Server->client push: enqueue a notification; drained by serve loops
   (mcp_stdio_serve / mcp_loop_run) or a host-owned transport. TAKES
   params on OK, caller retains on error. */
int           mcp_server_notify_client(ctx, s, const char *method,
                                       mcp_json_value_t *params);
/* Pop one pending push (LIFO); caller owns and destroys the message.
   MCP_ERR_NOT_FOUND with *out = NULL when empty. */
int           mcp_server_outbox_pop(ctx, s, mcp_message_t **out);

/* Server->client numbered request (e.g. roots/list, sampling/createMessage):
   built with the server's next_server_id counter, enqueued in the outbox;
   serve loops drain it before each transport read. TAKES params on OK,
   caller retains on error. */
int           mcp_server_request_client(ctx, s, const char *method,
                                        mcp_json_value_t *params);

/* Observability: lock-free atomic counters (monotonic, never reset) */
typedef struct mcp_server_counters {
    uint64_t requests_total;        /* dispatch entries */
    uint64_t requests_error;        /* requests ending in an RPC error */
    uint64_t notifications_total;   /* successful notifications */
    uint64_t tools_called;          /* tool handler invocations */
} mcp_server_counters_t;
int           mcp_server_counters(ctx, s, mcp_server_counters_t *out);

/* Request tracer hook: BEGIN/END per dispatch with method, status
   (0 on success, RPC code on error) and duration_ns. May fire on worker
   threads — the callback must be thread-safe and must not call back
   into the server. */
typedef enum { MCP_TRACE_BEGIN, MCP_TRACE_END } mcp_trace_event_t;
typedef void (*mcp_trace_fn)(mcp_context_t *ctx, mcp_trace_event_t ev,
                             const char *method, int status,
                             uint64_t duration_ns, void *userdata);
int           mcp_server_set_tracer(ctx, s, mcp_trace_fn fn_or_null,
                                    void *userdata);
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
typedef enum mcp_tool_visibility {
    MCP_TOOL_VIS_MODEL = 0,
    MCP_TOOL_VIS_APP   = 1,
    MCP_TOOL_VIS_BOTH  = 2,
} mcp_tool_visibility_t;
int           mcp_tool_set_visibility(ctx, mcp_tool_t *, mcp_tool_visibility_t);
int           mcp_tool_require_perms(ctx, mcp_tool_t *, uint32_t perm_mask);
```

### `resource.h` / `prompt.h`
```c
/* handler: (ctx, session, uri, user_data, mcp_json_value_t **result) */
mcp_resource_t *mcp_resource_new(ctx, const char *uri, const char *name,
                                 mcp_resource_handler_fn handler, void *ud);
void            mcp_resource_destroy(ctx, mcp_resource_t *);
/* cleanup hook: called by mcp_resource_destroy with the stored user_data */
int             mcp_resource_set_cleanup(ctx, mcp_resource_t *,
                                         mcp_resource_cleanup_fn cleanup);
int             mcp_resource_set_contents(ctx, mcp_resource_t *, mcp_json_value_t *);
int             mcp_resource_set_mime(ctx, mcp_resource_t *, const char *);

/* handler: (ctx, session, args, user_data, mcp_json_value_t **result) */
mcp_prompt_t   *mcp_prompt_new(ctx, const char *name, const char *desc,
                               mcp_json_value_t *args_schema,
                               mcp_prompt_handler_fn handler, void *ud);
void           mcp_prompt_destroy(ctx, mcp_prompt_t *);
```

### `session.h`
```c
bool mcp_session_is_initialized(ctx, const mcp_session_t *);
/* Apps-host flag: affects tool visibility filter in tools/list */
int  mcp_session_set_apps_host(ctx, mcp_session_t *, int flag);
bool mcp_session_is_apps_host(ctx, const mcp_session_t *);
/* Permission grants (mask semantics: 0 = none; all-bits = all granted) */
int  mcp_session_grant(ctx, mcp_session_t *, uint32_t perm_mask);
int  mcp_session_revoke(ctx, mcp_session_t *, uint32_t perm_mask);
bool mcp_session_grants(ctx, const mcp_session_t *, uint32_t perm_mask);
/* nonzero if any bit in mask is granted */
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
Push fails with `MCP_ERR_NOMEM` once the queue holds `MCP_QUEUE_MAX_LEN`
(1024) messages — the host must drain before pushing more.

List pagination: `tools/list`, `resources/list`, `prompts/list` return at
most 100 entries per call. Pass the returned `nextCursor` string back as
`params.cursor` for the next page; the last page omits `nextCursor`.
A malformed `cursor` fails with `MCP_ERR_INVALID_PARAMS`.

### Advanced protocol methods

`logging/setLevel` (`debug`/`info`/`notice`/`warning`/`error`, `warn`
accepted as an alias) sets the per-server log floor that gates the
dispatcher's `dlogf_srv` sites; `resources/subscribe` /
`resources/unsubscribe` maintain a per-server deduplicated subscription set
(idempotent); `resources/templates/list` answers an honest empty
`{templates: []}`. Advanced notifications (`notifications/cancelled`,
`notifications/progress`, the four `*_list_changed`,
`notifications/resources/updated`, `logging/message`) are consumed by
`mcp_server_notify` (counter + DEBUG log, no response) rather than being
routed as requests.

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

/* Per-call I/O timeouts in ms (0 = block forever, the default).
   Stored on the wrapper; socket and stdio backends honor them and
   return MCP_ERR_TIMEOUT on expiry. */
int              mcp_transport_set_timeout(ctx, mcp_transport_t *t,
                                          uint64_t read_ms, uint64_t write_ms);
int              mcp_transport_get_timeout(ctx, const mcp_transport_t *t,
                                          uint64_t *read_ms_out,
                                          uint64_t *write_ms_out);
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
and `recv` returns `MCP_ERR_PROTOCOL`. Honors the wrapper recv timeout
(`MCP_ERR_TIMEOUT` on expiry); returns `MCP_ERR_CANCELLED` once
`mcp_shutdown_requested()` is set.

### `http.h`
Buffer-level HTTP/1.1 parser/builder (no sockets).
```c
mcp_http_request_t *mcp_http_parse_request(ctx, const char *data, size_t len);
void                mcp_http_request_destroy(ctx, mcp_http_request_t *req);
mcp_http_method_t   mcp_http_request_method(ctx, const mcp_http_request_t *);
const char         *mcp_http_request_target(ctx, const mcp_http_request_t *);
const char         *mcp_http_header(ctx, const mcp_http_request_t *, const char *name);
const char         *mcp_http_request_body(ctx, const mcp_http_request_t *, size_t *len_out);

mcp_http_response_t *mcp_http_response_new(ctx, int status, const char *reason);
int                  mcp_http_response_set_header(ctx, resp, const char *name, const char *value);
int                  mcp_http_response_set_body(ctx, resp, const char *body, size_t len);
const char          *mcp_http_response_serialize(ctx, mcp_http_response_t *resp);
void                 mcp_http_response_destroy(ctx, mcp_http_response_t *resp);
```

### `streamable_http.h`
Session-management serve loop over `mcp_http_io_t` (opaque read/write callbacks):
```c
mcp_status_t mcp_http_serve(ctx, mcp_server_t *, mcp_http_io_t *io);
/* Wraps a JSON-RPC body as an SSE-compatible event stream (owned string) */
char       *mcp_sse_wrap(ctx, const char *json_text);
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
/* list_tools auto-paginates: follows nextCursor and returns the merged
   {"tools": [...]} object, so callers never see pages. */
int          mcp_client_call_tool(ctx, c, const char *name,
                                  mcp_json_value_t *args,
                                  mcp_json_value_t **result_out);
int          mcp_client_read_resource(ctx, c, const char *uri,
                                      mcp_json_value_t **result_out);
int          mcp_client_get_prompt(ctx, c, const char *name,
                                   mcp_json_value_t *args,
                                   mcp_json_value_t **result_out);

/* Low-level request: any method; result_out may be NULL.
   mcp_status_t: OK on success (result_out set), MCP_ERR_PROTOCOL on
   a real JSON-RPC error response (error_code maps via mcp_rpc_code_to_status),
   transport/alloc status otherwise. */
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
mcp_executor_t *mcp_threadpool_create(ctx, size_t thread_count);
/* task runs with the ctx captured at submit time (per-node) */
```

### `timer.h`
Monotonic (`CLOCK_MONOTONIC`) timer wheel.
```c
mcp_timer_t  *mcp_timer_create(ctx);
void          mcp_timer_destroy(ctx, mcp_timer_t *t);
int           mcp_timer_schedule(ctx, t, uint64_t delay_ms, mcp_task_fn fn, void *arg);
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
every recv. The loop drains in-flight requests and returns
`MCP_ERR_CANCELLED` once `mcp_shutdown_requested()` is set; a finite
recv timeout keeps an idle loop responsive to shutdown.

---

## Apps (`mcpkit/apps/`)

### `csp.h`
Content-Security-Policy builder; default-deny instance is the starting point.
```c
mcp_csp_t *mcp_csp_default_deny_new(ctx);
void       mcp_csp_destroy(ctx, mcp_csp_t *csp);
int        mcp_csp_set(ctx, csp, const char *directive, const char *sources_or_null);
int        mcp_csp_serialize(ctx, const mcp_csp_t *csp, char **out);  /* owned */
```
Directives are CSP keywords (`default-src`, `script-src`, `style-src`,
`connect-src`, …); `sources_or_null == NULL` means the directive applies
with no sources.

### `ui.h`
`ui://` UI resources, permission macros, and mount lifecycle.
```c
#define MCP_APPS_UI_MIME "text/html;profile=mcp-app"
#define MCP_APPS_UI_SCHEME "ui://"
#define MCP_APPS_PERM_CALL_TOOL (1u << 0)
#define MCP_APPS_PERM_READ_STATE (1u << 1)

/* UI resource: a regular resource tagged for Apps hosts */
mcp_resource_t *mcp_apps_ui_resource_new(ctx, const char *uri, const char *name,
                                         const char *html,
                                         const mcp_csp_t *csp_or_null);
/* Stamps _meta.ui.resourceUri into a tool-call result object */
int mcp_apps_result_with_ui(ctx, mcp_json_value_t *result,
                            const char *resource_uri);

/* Mount lifecycle: host calls mount, runs on_mount, unmount runs on_unmount */
typedef void (*mcp_apps_lifecycle_fn)(ctx, mcp_session_t *session, void *user_data);
int mcp_apps_mount(ctx, mcp_session_t *session,
                   mcp_apps_lifecycle_fn on_mount_or_null,
                   mcp_apps_lifecycle_fn on_unmount_or_null,
                   void *user_data, mcp_apps_mount_t **handle_out);
int mcp_apps_unmount(ctx, mcp_apps_mount_t *handle);
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
