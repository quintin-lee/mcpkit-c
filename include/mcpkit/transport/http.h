/**
 * @file http.h
 * @brief Buffer-level HTTP/1.1 request parser and response builder.
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

#ifndef MCPKIT_TRANSPORT_HTTP_H
#define MCPKIT_TRANSPORT_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"


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
 * @brief Parses a raw HTTP/1.1 request from a byte buffer.
 *
 * Returns NULL on malformed input (missing CRLFCRLF, oversized line,
 * invalid Content-Length, NUL bytes, etc.).
 * The body (if present) is a borrowed pointer into `data`; the input
 * buffer must outlive the request handle.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param data Byte buffer containing the full request.
 * @param len Number of bytes in data.
 * @return Owned mcp_http_request_t, or NULL on parse failure or OOM.
 */
mcp_http_request_t *mcp_http_parse_request(mcp_context_t *ctx,
                                           const char *data, size_t len);

/**
 * @brief Destroys a parsed HTTP request.
 * @param ctx Context; may be NULL.
 * @param req Request to destroy; NULL is a no-op.
 */
void mcp_http_request_destroy(mcp_context_t *ctx, mcp_http_request_t *req);

/**
 * @brief Returns the HTTP method of a parsed request.
 * @param ctx Context; may be NULL.
 * @param req Parsed request.
 * @return mcp_http_method_t value.
 */
mcp_http_method_t mcp_http_request_method(mcp_context_t *ctx,
                                          const mcp_http_request_t *req);

/**
 * @brief Returns the request target (path + query string).
 * @param ctx Context; may be NULL.
 * @param req Parsed request.
 * @return BORROWED string pointer; valid until mcp_http_request_destroy.
 */
const char *mcp_http_request_target(mcp_context_t *ctx,
                                    const mcp_http_request_t *req);

/**
 * @brief Case-insensitive header lookup.
 * @param ctx Context; may be NULL.
 * @param req Parsed request.
 * @param name Header name (e.g. "Host"); case-insensitive.
 * @return BORROWED string, or NULL if the header is absent.
 */
const char *mcp_http_header(mcp_context_t *ctx, const mcp_http_request_t *req,
                            const char *name);

/**
 * @brief Returns the request body pointer and its length.
 *
 * The pointer is borrowed from the input buffer. Body is NULL when no
 * Content-Length was provided.
 *
 * @param ctx Context; may be NULL.
 * @param req Parsed request.
 * @param len_out Receives the body length (Content-Length value).
 * @return BORROWED body pointer, or NULL.
 */
const char *mcp_http_request_body(mcp_context_t *ctx,
                                  const mcp_http_request_t *req,
                                  size_t *len_out);

/**
 * @brief Creates an HTTP response.
 *
 * Caller owns the response; destroy with mcp_http_response_destroy().
 *
 * @param ctx Context; may be NULL.
 * @param status HTTP status code (e.g. 200, 404).
 * @param reason Optional reason phrase; may be NULL.
 * @return Owned mcp_http_response_t, or NULL on OOM.
 */
mcp_http_response_t *mcp_http_response_new(mcp_context_t *ctx, int status,
                                           const char *reason);

/**
 * @brief Adds a header to a response.
 * @param ctx Context; may be NULL.
 * @param resp Target response.
 * @param name Header name; not owned.
 * @param value Header value; not owned.
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_http_response_set_header(mcp_context_t *ctx,
                                          mcp_http_response_t *resp,
                                          const char *name,
                                          const char *value);

/**
 * @brief Sets the response body.
 *
 * The buffer is COPIED (owned by the response); the caller may free it
 * immediately.
 *
 * @param ctx Context; may be NULL.
 * @param resp Target response.
 * @param body Body bytes; copied, not owned.
 * @param len Body length in bytes.
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_http_response_set_body(mcp_context_t *ctx,
                                        mcp_http_response_t *resp,
                                        const char *body, size_t len);

/**
 * @brief Serializes the response to a single owned snapshot.
 *
 * Returns a BORROWED const char*; valid until the next set_header /
 * set_body call or destroy of the response.
 *
 * @param ctx Context; may be NULL.
 * @param resp Target response.
 * @return BORROWED serialized string, or NULL if the response is empty.
 */
const char *mcp_http_response_serialize(mcp_context_t *ctx,
                                        mcp_http_response_t *resp);

/**
 * @brief Destroys an HTTP response and its owned resources.
 * @param ctx Context; may be NULL.
 * @param resp Response to destroy; NULL is a no-op.
 */
void mcp_http_response_destroy(mcp_context_t *ctx, mcp_http_response_t *resp);

#endif
