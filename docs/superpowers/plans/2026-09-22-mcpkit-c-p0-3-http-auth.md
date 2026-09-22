# P0-3 HTTP Auth Middleware Implementation Plan

> **For agentic workers**: REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional Bearer-token authentication to `mcp_http_serve` via a new `mcp_http_serve_with_auth` entry point, using a host-supplied validation callback. No new CMake option; pure core-logic change gated by the existing `MCPKIT_BUILD_HTTP` flag.

**Architecture:** Refactor `mcp_http_serve`'s body into a shared `http_serve_impl` that takes an optional `auth_fn`. When non-NULL, every POST request is checked against an `Authorization: Bearer <token>` header BEFORE the existing 100-continue / body-parse / dispatch logic; a missing/non-Bearer header or a `false` callback return yields a `401 Unauthorized` + `WWW-Authenticate: Bearer` response (built by a new small `send_unauthorized` helper, not by extending `send_bytes`). GET/DELETE are unchanged. `mcp_http_serve` becomes a thin wrapper calling `http_serve_impl` with `auth_fn=NULL`, preserving its exact existing behavior.

**Tech Stack:** C23, existing buffer-level HTTP parser (`src/transport/http.c`), streamable HTTP serve loop (`src/transport/streamable_http.c`), no new external dependencies.

---

## Chunk 1: API + Implementation

### Task 1: Add `mcp_http_serve_with_auth` declaration and auth callback typedef

**Files:**
- Modify: `include/mcpkit/transport/streamable_http.h` (after line 64, before the final `#endif`)

- [ ] **Step 1: Add the typedef and new function declaration**

In `include/mcpkit/transport/streamable_http.h`, add (after the `mcp_sse_wrap` declaration, before the closing `#endif`):

```c
/**
 * @brief Host-supplied Bearer-token validation callback for
 *        mcp_http_serve_with_auth().
 *
 * @param ctx Context; may be NULL.
 * @param bearer_token The token portion extracted from the request's
 *        "Authorization: Bearer <token>" header (the substring after the
 *        7-char case-insensitive "Bearer " prefix; may be an empty string
 *        if the header value was exactly "Bearer " or "Bearer").  The
 *        pointer is valid only for the duration of the callback itself.
 * @param user_data Opaque host-provided context (e.g. a table of valid
 *        tokens, a service-lookup table, whatever the host needs).
 * @return true to allow the request to proceed; false to reject it with
 *        a 401 Unauthorized response.
 */
typedef bool (*mcp_http_auth_fn)(mcp_context_t *ctx,
                                  const char *bearer_token,
                                  void *user_data);

/**
 * @brief Runs a synchronous HTTP serve loop with optional Bearer-token
 *        authentication on every POST request.
 *
 * When `auth_fn` is NULL this is identical to mcp_http_serve() — no
 * authentication is performed.  When `auth_fn` is non-NULL, every POST
 * request must carry an "Authorization: Bearer <token>" header; a missing
 * or non-Bearer-prefixed header, or a callback returning false, causes a
 * `401 Unauthorized` response (with a `WWW-Authenticate: Bearer`
 * challenge header) instead of the request being dispatched.  GET and
 * DELETE requests are NOT affected by `auth_fn` and behave exactly as
 * under mcp_http_serve().
 *
 * The caller must keep `auth_fn`'s `user_data` (and anything it points
 * to) alive for the entire duration of this call.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server to dispatch to.
 * @param io I/O backend; must outlive the call.
 * @param auth_fn Validation callback, or NULL to disable authentication.
 * @param auth_user_data Opaque context passed through to auth_fn.
 * @return MCP_OK on clean EOF; MCP_ERR_IO on I/O failure.
 */
mcp_status_t mcp_http_serve_with_auth(mcp_context_t *ctx, mcp_server_t *server,
                                       mcp_http_io_t *io,
                                       mcp_http_auth_fn auth_fn,
                                       void *auth_user_data);
```

- [ ] **Step 2: Verify it compiles (declaration only, no impl yet — expected link failure for the new symbol, but header itself must be clean)**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build -j4 2>&1 | tail -20
```
Expected: `streamable_http.c` still compiles (it doesn't reference the new symbol yet); no NEW errors related to the header. A successful full build is expected since nothing references `mcp_http_serve_with_auth` yet.

- [ ] **Step 3: Commit**

```bash
git add include/mcpkit/transport/streamable_http.h
git commit -m "feat(http): declare mcp_http_serve_with_auth and auth callback typedef"
```

---

### Task 2: Refactor `mcp_http_serve` into `http_serve_impl` and add the auth gate

**Files:**
- Modify: `src/transport/streamable_http.c`

- [ ] **Step 1: Add the `send_unauthorized` helper**

In `src/transport/streamable_http.c`, immediately after the existing `send_bytes` helper (line ~105), add:

```c
static mcp_status_t send_unauthorized(mcp_context_t *ctx, mcp_http_io_t *io) {
    mcp_http_response_t *resp = mcp_http_response_new(ctx, 401, "Unauthorized");
    if (resp == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_http_response_set_header(ctx, resp, "WWW-Authenticate",
                                                   "Bearer");
    if (st == MCP_OK) {
        st = mcp_http_response_set_body(ctx, resp, "", 0);
    }
    if (st == MCP_OK) {
        st = mcp_http_response_set_header(ctx, resp, "Content-Length", "0");
    }
    const char *bytes = NULL;
    if (st == MCP_OK) {
        bytes = mcp_http_response_serialize(ctx, resp);
        if (bytes == NULL) {
            st = MCP_ERR_NOMEM;
        }
    }
    if (st == MCP_OK) {
        st = io->write(ctx, io->user, bytes, strlen(bytes));
    }
    mcp_http_response_destroy(ctx, resp);
    return st;
}
```

- [ ] **Step 2: Add a small case-insensitive 7-char "Bearer " prefix check**

Still in `src/transport/streamable_http.c`, add (near the other static helpers, e.g. right before `handle_post`):

```c
/* Case-insensitive match of the first `len` chars of `s` against a
 * lowercase-only pattern; mirrors the hand-rolled tolower approach used
 * by mcp_http_request_wants_close() in http.c (strncasecmp is POSIX,
 * unavailable under this project's C23 + -Werror baseline). */
static bool ci_prefix_eq(const char *s, size_t len, const char *pattern) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != pattern[i]) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 3: Refactor `mcp_http_serve` into `http_serve_impl` + wrapper**

Rename the existing `mcp_http_serve` function body: change its signature from

```c
mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_http_io_t *io) {
```

to

```c
static mcp_status_t http_serve_impl(mcp_context_t *ctx, mcp_server_t *server,
                                    mcp_http_io_t *io, mcp_http_auth_fn auth_fn,
                                    void *auth_ud) {
```

(everything else in the function body stays identical for now). Then, immediately after `http_serve_impl`'s closing brace, add:

```c
mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server,
                             mcp_http_io_t *io) {
    return http_serve_impl(ctx, server, io, NULL, NULL);
}

mcp_status_t mcp_http_serve_with_auth(mcp_context_t *ctx, mcp_server_t *server,
                                       mcp_http_io_t *io, mcp_http_auth_fn auth_fn,
                                       void *auth_user_data) {
    if (server == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return http_serve_impl(ctx, server, io, auth_fn, auth_user_data);
}
```

- [ ] **Step 4: Insert the auth gate at the top of `handle_post`'s dispatch path**

`handle_post` currently starts by reading the body and (if empty) returning 400. The auth gate must run BEFORE any of that, and only when `auth_fn != NULL`. Change `handle_post`'s signature to also take `auth_fn`/`auth_ud`:

```c
static mcp_status_t handle_post(mcp_context_t *ctx, mcp_server_t *server,
                                 http_session_table_t *table, mcp_http_io_t *io,
                                 mcp_http_request_t *req,
                                 mcp_http_auth_fn auth_fn, void *auth_ud) {
```

Insert as the VERY FIRST lines of the new `handle_post` body (before the existing `size_t body_len = 0;` line):

```c
    if (auth_fn != NULL) {
        const char *auth_hdr = mcp_http_header(ctx, req, "Authorization");
        if (auth_hdr == NULL || !ci_prefix_eq(auth_hdr, 7, "bearer ")) {
            return send_unauthorized(ctx, io);
        }
        const char *token = auth_hdr + 7;
        if (!auth_fn(ctx, token, auth_ud)) {
            return send_unauthorized(ctx, io);
        }
    }
```

Update the single call site inside `http_serve_impl` (where `handle_post(ctx, server, &table, io, req)` is invoked, inside the `if (m == MCP_HTTP_POST)` branch) to pass through `auth_fn` and `auth_ud`:

```c
if (m == MCP_HTTP_POST) {
    st = handle_post(ctx, server, &table, io, req, auth_fn, auth_ud);
}
```

Note: the auth check intentionally runs BEFORE the `Expect: 100-continue`
check (which is further down in `handle_post`, after the body-read), so a
rejected request is never preceded by a spurious 100-Continue.

- [ ] **Step 5: Verify it compiles cleanly**

Run:
```bash
cmake --build build -j4 2>&1 | tail -20
```
Expected: 0 errors, 0 new warnings.

- [ ] **Step 6: Commit**

```bash
git add src/transport/streamable_http.c include/mcpkit/transport/streamable_http.h
git commit -m "feat(http): add mcp_http_serve_with_auth bearer-token auth gate"
```

---

## Chunk 2: Tests

### Task 3: Add auth test cases to `test_http_serve.c`

**Files:**
- Modify: `tests/unit/test_http_serve.c`

- [ ] **Step 1: Add auth callback test doubles and a helper**

In `tests/unit/test_http_serve.c`, after the existing `make_post`/`check_status_line` helpers (around line 123), add:

```c
static bool auth_always_true(mcp_context_t *ctx, const char *tok, void *ud) {
    (void)ctx; (void)tok; (void)ud;
    return true;
}

typedef struct {
    const char *expected;
    int calls;
} auth_expected_t;

static bool auth_check_token(mcp_context_t *ctx, const char *tok, void *ud) {
    (void)ctx;
    auth_expected_t *e = ud;
    e->calls++;
    return strcmp(tok, e->expected) == 0;
}

static char *run_script_auth(mcp_context_t *ctx, mcp_server_t *srv,
                             const char **reqs, size_t n, mcp_status_t *st_out,
                             mcp_http_auth_fn auth_fn, void *auth_ud) {
    mem_io_t m;
    memset(&m, 0, sizeof(m));
    m.reqs = reqs;
    m.nreqs = n;
    mcp_http_io_t io;
    io.user = &m;
    io.read = mem_read;
    io.write = mem_write;
    *st_out = mcp_http_serve_with_auth(ctx, srv, &io, auth_fn, auth_ud);
    return m.out;
}
```

- [ ] **Step 2: Add the 6 test cases**

In `main()`, after the existing case (j) block (i.e. just before the final `mcp_server_destroy`/`mcp_context_destroy` lines, ~line 235), add:

```c
    /* (k) auth: no Authorization header + auth enabled -> 401 */
    {
        char buf[4096];
        char *req = make_post(kInitBody, NULL, buf, sizeof(buf));
        const char *reqs[] = { req };
        char *out = run_script_auth(ctx, srv, reqs, 1, &st, auth_always_true, NULL);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "WWW-Authenticate: Bearer") != NULL);
        free(out);
    }

    /* (l) auth: Authorization: Bearer good + callback true -> 200 */
    {
        char buf[4096];
        int n = snprintf(buf, sizeof(buf),
                         "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                         "Content-Type: application/json\r\n"
                         "Authorization: Bearer good\r\n\r\n%s",
                         strlen(kInitBody), kInitBody);
        CHECK(n > 0 && (size_t)n < sizeof(buf));
        const char *reqs[] = { buf };
        auth_expected_t exp = { .expected = "good", .calls = 0 };
        char *out = run_script_auth(ctx, srv, reqs, 1, &st, auth_check_token, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(exp.calls == 1);
        CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
        CHECK(strstr(out, "Mcp-Session-Id:") != NULL);
        free(out);
    }

    /* (m) auth: Authorization: Bearer bad + callback false -> 401 */
    {
        char buf[4096];
        int n = snprintf(buf, sizeof(buf),
                         "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                         "Content-Type: application/json\r\n"
                         "Authorization: Bearer bad\r\n\r\n%s",
                         strlen(kInitBody), kInitBody);
        CHECK(n > 0 && (size_t)n < sizeof(buf));
        const char *reqs[] = { buf };
        auth_expected_t exp = { .expected = "good", .calls = 0 };
        char *out = run_script_auth(ctx, srv, reqs, 1, &st, auth_check_token, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(exp.calls == 1);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        free(out);
    }

    /* (n) auth: empty token after "Bearer " (header value exactly
           "Bearer ") -> callback invoked with "" ; allow -> 200 */
    {
        char buf[4096];
        int n = snprintf(buf, sizeof(buf),
                         "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                         "Content-Type: application/json\r\n"
                         "Authorization: Bearer \r\n\r\n%s",
                         strlen(kInitBody), kInitBody);
        CHECK(n > 0 && (size_t)n < sizeof(buf));
        const char *reqs[] = { buf };
        auth_expected_t exp = { .expected = "", .calls = 0 };
        char *out = run_script_auth(ctx, srv, reqs, 1, &st, auth_check_token, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(exp.calls == 1);
        CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
        free(out);
    }

    /* (o) auth: case-insensitive prefix "bearer good" -> 200 when
           callback accepts */
    {
        char buf[4096];
        int n = snprintf(buf, sizeof(buf),
                         "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                         "Content-Type: application/json\r\n"
                         "Authorization: bearer good\r\n\r\n%s",
                         strlen(kInitBody), kInitBody);
        CHECK(n > 0 && (size_t)n < sizeof(buf));
        const char *reqs[] = { buf };
        auth_expected_t exp = { .expected = "good", .calls = 0 };
        char *out = run_script_auth(ctx, srv, reqs, 1, &st, auth_check_token, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(exp.calls == 1);
        CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
        free(out);
    }

    /* (p) auth disabled (auth_fn=NULL, i.e. plain mcp_http_serve) with a
           no-Authorization POST -> still 200 (existing behavior unchanged) */
    {
        char buf[4096];
        char *req = make_post(kInitBody, NULL, buf, sizeof(buf));
        const char *reqs[] = { req };
        char *out = run_script_auth(ctx, srv, reqs, 1, &st, NULL, NULL);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
        free(out);
    }
```

- [ ] **Step 3: Register nothing new in CMake** (test_http_serve is already registered under `MCPKIT_BUILD_HTTP` in `tests/CMakeLists.txt`; no change needed).

- [ ] **Step 4: Run the new test cases**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build -j4
./build/tests/test_http_serve
```
Expected: `test_http_serve OK` (all CHECKs pass, including the 6 new cases).

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_http_serve.c
git commit -m "test(http): prove mcp_http_serve_with_auth bearer-token gate"
```

---

## Chunk 3: Docs + closeout

### Task 4: Update module-reference and CHANGELOG, run gates, clean up

**Files:**
- Modify: `docs/module-reference.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Add a module-reference line for `mcp_http_serve_with_auth`**

In `docs/module-reference.md`, find the `streamable_http.h` section (search for `mcp_http_serve`). Immediately after the existing `mcp_http_serve` bullet/line, add:

```markdown
- `mcp_http_serve_with_auth(ctx, server, io, auth_fn, auth_ud)` — same as
  `mcp_http_serve` but runs a host-supplied `mcp_http_auth_fn`
  (Bearer-token validator, called for every POST; `false` => 401 with
  `WWW-Authenticate: Bearer`) before dispatch. `auth_fn == NULL` is
  identical to `mcp_http_serve`. GET/DELETE are unaffected. Caller keeps
  `auth_ud` alive for the call's duration.
```

- [ ] **Step 2: Add a CHANGELOG bullet under `[Unreleased] > Added`**

In `CHANGELOG.md`, under the `[Unreleased]` section's `Added` list, add:

```markdown
- `mcp_http_serve_with_auth()`: optional Bearer-token authentication for
  Streamable HTTP POST traffic, via a host-supplied validation callback;
  401 + `WWW-Authenticate: Bearer` on missing/invalid token.  `auth_fn==NULL`
  preserves the existing `mcp_http_serve` behavior exactly.
```

- [ ] **Step 3: Run doxygen to confirm no new warnings**

Run:
```bash
doxygen Doxyfile
```
Expected: 0 real warnings (doxygen-1.18 does not flag missing @param for the
new declarations; verify visually no `@data`/typo-style warnings appeared for
the new blocks in `streamable_http.h`).

- [ ] **Step 4: Triple gate (all 3 configurations, HTTP+SOCKET=ON)**

Run (in order, from clean configures):
```bash
rm -rf build-gcc build-clang build-asan
cmake -S . -B build-gcc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build-gcc -j4 && ctest --test-dir build-gcc --output-on-failure
cmake -S . -B build-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build-clang -j4 && ctest --test-dir build-clang --output-on-failure
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON
cmake --build build-asan -j4 && ldd build-asan/tools/mcpkit-cli | grep -E 'libasan|libubsan' && ctest --test-dir build-asan --output-on-failure
```
Expected: all three report the same passing test count as before this change
started (46 with HTTP+SOCKET=ON; `test_http_serve` now includes the 6 new
auth cases in its existing binary — no new ctest entry, so the ctest count
is unchanged from before this task's test additions); `libasan.so.8` and
`libubsan.so.1` confirmed linked in the ASan build.

- [ ] **Step 5: Remove build dirs, verify tree clean, commit docs**

Run:
```bash
rm -rf build-gcc build-clang build-asan
git status
```
Expected: only `docs/module-reference.md` and `CHANGELOG.md` modified (everything else was committed in Tasks 1-3).

```bash
git add docs/module-reference.md CHANGELOG.md
git commit -m "docs: cover mcp_http_serve_with_auth in module reference and changelog"
```

- [ ] **Step 6: Final verification**

Run:
```bash
git status
```
Expected: clean working tree, no build dirs, master has 3 new commits on top of the pre-task HEAD (`feat(http): declare...`, `feat(http): add mcp_http_serve_with_auth...`, `test(http): prove...`) plus this docs commit (4 total commits for P0-3).
