# P0-3 HTTP Auth Middleware Design

## Goal

Add optional bearer-token authentication to `mcp_http_serve` so hosts can gate
POST traffic with a custom validation callback, without altering core dispatcher
or introducing a new CMake option.

## Decisions (confirmed with user)

- **Layer**: HTTP serve-loop middleware (inside `streamable_http.c`), not the
  server dispatcher.
- **Validation logic**: host supplies a callback; MCPKit only extracts the
  Bearer token from the `Authorization` header.
- **Scope**: POST only. GET (SSE) and DELETE are unaffected.

## API

```c
typedef bool (*mcp_http_auth_fn)(mcp_context_t *ctx,
                                  const char *bearer_token,
                                  void *user_data);

mcp_status_t mcp_http_serve_with_auth(mcp_context_t *ctx,
                                       mcp_server_t *server,
                                       mcp_http_io_t *io,
                                       mcp_http_auth_fn auth_fn,
                                       void *auth_user_data);
```

- `auth_fn == NULL` → identical to `mcp_http_serve` (no auth), backward
  compatible.
- `auth_fn != NULL` → for every POST:
  1. Read `Authorization` header.
     - Missing, or does not start with `Bearer ` (case-insensitive prefix,
       exactly one space) → `401 Unauthorized` + `WWW-Authenticate: Bearer`.
  2. Call `auth_fn(ctx, token, user_data)`.
     - Returns `false` → `401 Unauthorized` + `WWW-Authenticate: Bearer`.
  3. Returns `true` → proceed to normal POST handling (100-continue, dispatch,
     etc., unchanged).

GET and DELETE handlers are not modified.

## Implementation

- `include/mcpkit/transport/streamable_http.h`: add typedef + `mcp_http_serve_with_auth`
  declaration (Doxygen @brief/@param/@return, P3 public API contract).
  - `src/transport/streamable_http.c`:
  - Refactor `mcp_http_serve` body into `static mcp_status_t http_serve_impl(
      ctx, server, io, auth_fn, auth_ud)` where `auth_fn` may be NULL.
  - `mcp_http_serve` = `http_serve_impl(ctx, server, io, NULL, NULL)`.
  - `mcp_http_serve_with_auth` = `http_serve_impl(ctx, server, io, auth_fn, auth_ud)`.
  - New small helper `static mcp_status_t send_unauthorized(ctx, io)` that sends
    exactly: `HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer\r\n
    Content-Length: 0\r\n\r\n` — built inline via `mcp_http_response_new` +
    `mcp_http_response_set_header` (WWW-Authenticate) + `mcp_http_response_set_body`
    (empty, len 0) + serialize + `io->write`. Do NOT extend `send_bytes` (its
    single-extra-header signature cannot express both `WWW-Authenticate` and
    `Content-Type`/`Content-Length` combinations cleanly for a bodyless 401;
    a dedicated helper is smaller and leaves the existing call site unchanged).
  - At the top of `handle_post`, before any other processing, when
    `auth_fn != NULL`:
    - `const char *auth = mcp_http_header(ctx, req, "Authorization");`
    - If `auth == NULL`, or the first 7 bytes do NOT case-insensitively match
      `"Bearer "` (implement with a small hand-rolled tolower loop over the 7
      chars — do NOT use `strncasecmp`, which is POSIX-only and not available
      under the project's C23/`-Werror` baseline without feature-test macros)
      → `mcp_message_destroy(ctx, msg)` (already-parsed message, if we got that
      far — but note: this check happens BEFORE `mcp_message_parse`, so nothing
      to destroy here; it happens before `handle_post`'s existing body-parse
      step), then `return send_unauthorized(ctx, io);`
    - token = `auth + 7` (borrowed pointer into the request's owned header
      value string; valid for the request's lifetime, which is the full
      duration of `handle_post` since `mcp_http_request_destroy` is called by
      the caller of `handle_post` only after `handle_post` returns).
    - Call `auth_fn(ctx, token, auth_ud)`; on `false` → `return send_unauthorized(ctx, io);`
    - On `true`, fall through to the existing 100-continue / body-parse /
      dispatch logic, unchanged.
  - The auth check runs BEFORE the `Expect: 100-continue` check, so a
    401-Unauthorized is never preceded by a spurious 100-Continue.
- `mcp_http_serve_with_auth` gains a `mcp_http_serve`-level Doxygen note:
  caller must keep `auth_fn`'s `user_data` alive for the duration of the call.

## 401 Response Shape

```
HTTP/1.1 401 Unauthorized\r\n
WWW-Authenticate: Bearer\r\n
Content-Length: 0\r\n
\r\n
```

(No body. `Content-Length: 0` is set explicitly to avoid ambiguity. Note: the
existing `mcp_http_response_new`/`set_body` builder chain does NOT emit a
`Content-Length` header automatically for a zero-length body — `send_unauthorized`
must add it explicitly via `mcp_http_response_set_header(resp, "Content-Length",
"0")` after `set_body(NULL or "", 0)`.)

## Token Extraction Rules

- Header name is matched case-insensitively via the existing `mcp_http_header`
  helper.
- Prefix match is case-insensitive `Bearer ` (7 chars, exactly one space
  following "Bearer"). Implement with a hand-rolled 7-char tolower comparison
  loop (do NOT use `strncasecmp` — POSIX-only, unavailable under the project's
  C23 + `-Werror` baseline without feature-test macros; a 7-iteration loop is
  trivial and matches the existing style used by `mcp_http_request_wants_close`
  for its 5-char "close" check in `src/transport/http.c`).
- Any leading/trailing whitespace beyond the single space after "Bearer" is NOT
  stripped — the token is everything after the 7th character, verbatim (may be
  an empty string when the header value is exactly "Bearer " or "Bearer"; the
  host callback decides validity).
- Multiple `Authorization` headers: `mcp_http_header` returns the first one
  (documented existing behavior; no change).
- The extracted `token` pointer (`auth + 7`) is valid for the entire duration of
  `handle_post` (the request handle — and therefore its owned header strings —
  outlives the `handle_post` call; `mcp_http_request_destroy` is invoked by
  `http_serve_impl` only after `handle_post` returns). No copy is needed.

## Testing (`tests/unit/test_http_serve.c`, HTTP-gated)

New sub-block, using the existing fake mem-io transport:

1. `auth_fn=NULL` (i.e. plain `mcp_http_serve`) POST → 200 (existing tests
   already cover this; add explicit "no Authorization header, no auth enabled"
   case only if not already present).
2. POST with no `Authorization` header, `auth_fn` set (always-true callback)
   → 401, response contains `WWW-Authenticate: Bearer`.
3. POST with `Authorization: Bearer good`, callback returns true → 200 (verify
   by checking the recorded response body contains a valid JSON-RPC response
   or the expected dispatch result).
4. POST with `Authorization: Bearer bad`, callback returns false → 401.
5. POST with `Authorization: Bearer` (empty token after prefix), callback
   receives `""` — assert callback was invoked (via a counter in `user_data`)
   and the result is whatever the callback returns.
6. Case-insensitivity: `Authorization: bearer good` → token extracted as
   `good`, 200 when callback true.

Callback test doubles: `static bool always_true(ctx, tok, ud)` /
`static bool check_token(ctx, tok, ud)` comparing against a string in `ud`.

Additional edge cases to assert:
- Token pointer passed to the callback is `const char*` and is NOT NUL-terminated
  differently from the raw header-value remainder — the callback may safely call
  `strlen`/`strcmp` on it since the header value string itself is NUL-terminated
  (owned, strdup'd by the parser).
- Verify `mcp_http_request_destroy(ctx, req)` in `http_serve_impl` happens AFTER
  `handle_post` returns (i.e. the token pointer is never used after the request
  is destroyed) — this is structurally guaranteed by the existing call order in
  `http_serve_impl` (line ~319 of the current file: `mcp_http_request_destroy`
  after the `if (m == MCP_HTTP_POST)` dispatch block); no new test needed, but
  the implementer must NOT move the `mcp_http_request_destroy` call earlier when
  adding the auth block.

## Files Touched

- `include/mcpkit/transport/streamable_http.h` (+typedef, +1 fn decl, +Doxygen)
- `src/transport/streamable_http.c` (refactor to impl, +auth gate in
  `handle_post`, +401 helper or extended `send_bytes`)
- `tests/unit/test_http_serve.c` (+new sub-block, ~6 cases)
- `docs/module-reference.md` (streamable_http.h section: +`mcp_http_serve_with_auth`
  + auth callback contract line)
- `CHANGELOG.md` `[Unreleased] > Added` bullet

## Out of Scope / Non-Goals

- No new CMake option (this is pure core logic, no external deps).
- No mTLS / mutual-auth extension to the TLS adapter (P0-2 already chose
  `SSL_VERIFY_NONE`; mTLS is a separate, later concern).
- No per-tool / per-resource authorization (that's the P0-3b "authz" half,
  separate from this "authn" half; dispatcher-level per-tool checks are NOT
  part of this change).
- No `WWW-Authenticate` challenge-string customization beyond fixed `Bearer`.
- No rate limiting / lockout on failed attempts.
- GET/DELETE authentication (deliberately excluded per the "POST only"
  decision).

## Backward Compatibility

`mcp_http_serve` signature and behavior are unchanged. Existing callers,
tests, and examples build and behave identically with `auth_fn == NULL`.
