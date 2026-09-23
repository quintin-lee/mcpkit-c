/**
 * @file http.h
 * @brief Buffer-level HTTP/1.1 request parser and response builder.
 * @ingroup mcpkit-transport
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
 * - mcp_http_response_set_body(): stores the caller's pointer (no
 *   copy); the caller must keep the body buffer alive for the
 *   response's lifetime.
 * - mcp_http_response_serialize(): returns a BORROWED const char*
 *   (a single owned snapshot stored in the response); valid until
 *   the next set_header/set_body call or destroy.
 */

#ifndef MCPKIT_TRANSPORT_HTTP_H
#define MCPKIT_TRANSPORT_HTTP_H

#include <stdbool.h>
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
 * @brief Returns true when the request carries "Connection: close".
 *
 * HTTP/1.1 connections are keep-alive by default; a client that explicitly
 * sends "Connection: close" signals it will close the TCP connection after
 * the response.  The host should use this to decide whether to reuse the
 * transport for the next request.  The check is case-insensitive.
 *
 * @param ctx Context; may be NULL.
 * @param req Parsed request (must be non-NULL).
 * @return true if "Connection: close" is present; false otherwise.
 */
bool mcp_http_request_wants_close(mcp_context_t *ctx,
                                  const mcp_http_request_t *req);

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
 * @brief Creates a new HTTP request for client use.
 *
 * @param ctx     Context; may be NULL.
 * @param method  HTTP method (MCP_HTTP_GET, MCP_HTTP_POST, MCP_HTTP_DELETE).
 * @param target  Request target path (e.g. "/mcp"); copied internally.
 * @return Owned mcp_http_request_t, or NULL on allocation failure.
 */
mcp_http_request_t *mcp_http_request_new(mcp_context_t *ctx, mcp_http_method_t method,
                                         const char *target);

/**
 * @brief Sets or replaces a header on an HTTP request.
 *
 * @param ctx    Context; may be NULL.
 * @param req    Target request.
 * @param name   Header name; not owned.
 * @param value  Header value; not owned.
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_INVALID_ARGUMENT on bad arguments.
 */
mcp_status_t mcp_http_request_set_header(mcp_context_t *ctx, mcp_http_request_t *req,
                                         const char *name, const char *value);

/**
 * @brief Sets the body on an HTTP request.
 *
 * The buffer pointer is stored (not copied); caller must keep the body buffer alive
 * for the lifetime of the request.
 *
 * @param ctx    Context; may be NULL.
 * @param req    Target request.
 * @param body   Body bytes; pointer stored, not copied.
 * @param len    Body length in bytes.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on bad arguments.
 */
mcp_status_t mcp_http_request_set_body(mcp_context_t *ctx, mcp_http_request_t *req,
                                       const char *body, size_t len);

/**
 * @brief Serializes the HTTP request to a single owned snapshot.
 *
 * Returns a BORROWED const char*; valid until the next set_header /
 * set_body call or destroy of the request.
 *
 * @param ctx  Context; may be NULL.
 * @param req  Target request.
 * @return BORROWED serialized string, or NULL on error.
 */
const char *mcp_http_request_serialize(mcp_context_t *ctx, mcp_http_request_t *req);

/**
 * @brief Sets SEP-2243 Streamable HTTP headers on an HTTP request.
 *
 * Sets "MCP-Protocol-Version: 2026-07-28" (or the provided protocol_version),
 * and if method is non-NULL, sets "Mcp-Method: <method>".
 * If name_or_uri is non-NULL, sets "Mcp-Name: <name_or_uri>".
 *
 * @param ctx               Context; may be NULL.
 * @param req               Target HTTP request.
 * @param protocol_version  Protocol version string (e.g. MCP_PROTOCOL_VERSION_LATEST); may be NULL to use default.
 * @param method            MCP method string (e.g. "tools/call"); may be NULL.
 * @param name_or_uri       MCP name or URI string (e.g. tool name); may be NULL.
 * @return MCP_OK on success; MCP_ERR_* on error.
 */
mcp_status_t mcp_http_request_set_mcp_metadata(mcp_context_t *ctx, mcp_http_request_t *req,
                                               const char *protocol_version,
                                               const char *method,
                                               const char *name_or_uri);

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
 * The buffer pointer is stored (not copied); the caller must keep the
 * body buffer alive for the lifetime of the response.
 *
 * @param ctx Context; may be NULL.
 * @param resp Target response.
 * @param body Body bytes; pointer stored, not copied.
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
