#ifndef MCPKIT_TRANSPORT_HTTP_H
#define MCPKIT_TRANSPORT_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"

/**
 * @file http.h
 * Buffer-level HTTP/1.1 request parser and response builder.
 *
 * Limits (compile-time):
 *   - at most MCP_HTTP_MAX_HEADERS (64) headers per request
 *   - at most MCP_HTTP_MAX_LINE (8 KB) per header line
 *   - at most MCP_HTTP_MAX_BODY (4 MB) body
 *
 * Ownership:
 * - mcp_http_parse_request(): on success the request owns its headers
 *   (strdup'd via ctx); the body pointer is BORROWED from the input
 *   buffer (no copy). Caller must keep the input buffer alive while
 *   using the request, or call mcp_http_request_destroy() first if
 *   the body pointer must be invalidated.
 * - mcp_http_response_new(): caller owns the response; destroy with
 *   mcp_http_response_destroy().
 * - mcp_http_response_serialize(): returns a BORROWED const char*
 *   (a single owned snapshot stored in the response); valid until
 *   the next set_header/set_body call or destroy.
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_http_request mcp_http_request_t;
typedef struct mcp_http_response mcp_http_response_t;

typedef enum {
    MCP_HTTP_GET,
    MCP_HTTP_POST,
    MCP_HTTP_DELETE,
    MCP_HTTP_UNKNOWN,
} mcp_http_method_t;

#define MCP_HTTP_MAX_HEADERS 64
#define MCP_HTTP_MAX_LINE 8192
#define MCP_HTTP_MAX_BODY (4u << 20)

/**
 * Parses a raw HTTP/1.1 request from a byte buffer.
 * Returns NULL on malformed input (missing CRLFCRLF, oversized line,
 * invalid Content-Length, NUL bytes, etc.).
 * The body (if present) is a borrowed pointer into `data`; the input
 * buffer must outlive the request handle.
 */
mcp_http_request_t *mcp_http_parse_request(mcp_context_t *ctx,
                                            const char *data, size_t len);
void mcp_http_request_destroy(mcp_context_t *ctx, mcp_http_request_t *req);
mcp_http_method_t mcp_http_request_method(mcp_context_t *ctx,
                                            const mcp_http_request_t *req);
const char *mcp_http_request_target(mcp_context_t *ctx,
                                     const mcp_http_request_t *req);
/**
 * Case-insensitive header lookup. Returns NULL if the header is absent.
 */
const char *mcp_http_header(mcp_context_t *ctx, const mcp_http_request_t *req,
                             const char *name);

/**
 * Returns the body pointer and sets *len_out to the Content-Length value.
 * The pointer is borrowed from the input buffer (see @file note).
 * Body is NULL when no Content-Length was provided.
 */
const char *mcp_http_request_body(mcp_context_t *ctx,
                                   const mcp_http_request_t *req,
                                   size_t *len_out);

/**
 * Creates an HTTP response. Caller owns it; destroy with
 * mcp_http_response_destroy().
 */
mcp_http_response_t *mcp_http_response_new(mcp_context_t *ctx, int status,
                                            const char *reason);
mcp_status_t mcp_http_response_set_header(mcp_context_t *ctx,
                                           mcp_http_response_t *resp,
                                           const char *name,
                                           const char *value);

/**
 * Sets the response body. The buffer is COPIED (owned by the response);
 * the caller may free it immediately.
 */
mcp_status_t mcp_http_response_set_body(mcp_context_t *ctx,
                                         mcp_http_response_t *resp,
                                         const char *body, size_t len);

/**
 * Serializes the response to a single owned snapshot.
 * Returns a BORROWED const char* (valid until the next mutation or
 * destroy of the response).
 */
const char *mcp_http_response_serialize(mcp_context_t *ctx,
                                         mcp_http_response_t *resp);
void mcp_http_response_destroy(mcp_context_t *ctx, mcp_http_response_t *resp);

#endif
