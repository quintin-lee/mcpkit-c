/**
 * @file streamable_http.h
 * @brief Streamable HTTP transport layer over a pluggable I/O backend.
 * @ingroup mcpkit-transport
 *
 * - mcp_http_io_t is an opaque I/O vtable; the implementation is
 *   responsible for fulfilling read/write callbacks against a real
 *   network socket or file.
 * - mcp_http_serve(): runs a synchronous HTTP serve loop. Sessions are
 *   loop-local (destroyed when the loop exits); at most
 *   MCP_HTTP_MAX_SESSIONS concurrent sessions are supported.
 *   POST requests carrying "Expect: 100-continue" receive an interim
 *   "HTTP/1.1 100 Continue" response before the final status.
 * - mcp_sse_wrap(): returns an owned SSE-framed string; caller frees
 *   with mcp_json_free_string(ctx, s).
 */

#ifndef MCPKIT_TRANSPORT_STREAMABLE_HTTP_H
#define MCPKIT_TRANSPORT_STREAMABLE_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;

typedef struct mcp_http_io mcp_http_io_t;
struct mcp_http_io {
    void *user;
    mcp_status_t (*read)(mcp_context_t *ctx, void *user, char *buf, size_t cap,
                           size_t *n_out);
    mcp_status_t (*write)(mcp_context_t *ctx, void *user, const char *data, size_t len);
};

#define MCP_HTTP_MAX_SESSIONS 16

/**
 * @brief Runs a synchronous HTTP serve loop until the I/O backend
 *        signals EOF.
 *
 * Sessions are destroyed when the loop exits; the caller must not use
 * them afterwards.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server to dispatch to.
 * @param io I/O backend; must outlive the call.
 * @return MCP_OK on clean EOF; MCP_ERR_IO on I/O failure.
 */
mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server,
                            mcp_http_io_t *io);

/**
 * @brief Wraps a JSON string in an SSE `data:` frame.
 *
 * Returns an owned heap string; caller frees with mcp_json_free_string().
 *
 * @param ctx Context; may be NULL.
 * @param json_text Null-terminated JSON string; not owned.
 * @return Owned SSE-framed string, or NULL on OOM.
 */
char *mcp_sse_wrap(mcp_context_t *ctx, const char *json_text);

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

#endif
