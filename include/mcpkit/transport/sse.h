/**
 * @file sse.h
 * @brief W3C Server-Sent Events (SSE) streaming frame parser.
 *
 * Implements a streaming chunk parser conforming to the W3C Server-Sent Events
 * specification. Handles event, data, id, and retry fields, arbitrary chunk boundaries,
 * multiline data concatenation, and Last-Event-ID tracking.
 *
 * @ingroup mcpkit-transport
 */

#ifndef MCPKIT_TRANSPORT_SSE_H
#define MCPKIT_TRANSPORT_SSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_sse_parser mcp_sse_parser_t;

/**
 * @brief Dispatched SSE event structure.
 *
 * Strings are borrowed and valid only for the duration of the callback.
 */
typedef struct mcp_sse_event {
    const char *event;      /**< Event type (e.g. "message", "endpoint", etc.), defaults to "message" if unset. */
    const char *data;       /**< Event data payload; multiline data is concatenated with '\n'. */
    const char *id;         /**< Event ID string, or NULL if not set. */
    int64_t retry_ms;       /**< Reconnection retry time in ms, or -1 if not set. */
} mcp_sse_event_t;

/**
 * @brief Callback invoked when a complete SSE event is parsed (on empty line).
 *
 * @param ev        Borrowed event pointer; do not retain after callback returns.
 * @param userdata  User-provided pointer passed to mcp_sse_parser_create.
 */
typedef void (*mcp_sse_event_cb)(const mcp_sse_event_t *ev, void *userdata);

/**
 * @brief Creates a streaming SSE parser.
 *
 * @param ctx       Context; may be NULL (default allocator).
 * @param on_event  Callback invoked when an event is received; must not be NULL.
 * @param userdata  User-provided context pointer.
 * @return Owned parser instance, or NULL on OOM or invalid arguments.
 */
mcp_sse_parser_t *mcp_sse_parser_create(mcp_context_t *ctx,
                                        mcp_sse_event_cb on_event,
                                        void *userdata);

/**
 * @brief Feeds a raw byte chunk from the HTTP stream into the SSE parser.
 *
 * Can be called with arbitrary chunk sizes (e.g. 1 byte to tens of kilobytes).
 *
 * @param ctx     Context; may be NULL.
 * @param parser  Parser instance; must not be NULL.
 * @param chunk   Input buffer.
 * @param len     Number of bytes in chunk.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if parser or chunk is NULL;
 *         MCP_ERR_NOMEM on memory allocation failure.
 */
mcp_status_t mcp_sse_parser_feed(mcp_context_t *ctx,
                                 mcp_sse_parser_t *parser,
                                 const char *chunk,
                                 size_t len);

/**
 * @brief Returns the Last-Event-ID tracked by the parser for stream resumption.
 *
 * @param ctx     Context; may be NULL.
 * @param parser  Parser instance; must not be NULL.
 * @return Borrowed Last-Event-ID string, or NULL if no event with an id has been received.
 */
const char *mcp_sse_parser_last_event_id(mcp_context_t *ctx,
                                        const mcp_sse_parser_t *parser);

/**
 * @brief Destroys an SSE parser instance and frees all associated buffers.
 *
 * @param ctx     Context; may be NULL.
 * @param parser  Parser instance; NULL is safe.
 */
void mcp_sse_parser_destroy(mcp_context_t *ctx, mcp_sse_parser_t *parser);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_TRANSPORT_SSE_H */
