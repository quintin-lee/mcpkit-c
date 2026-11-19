/**
 * @file json_stream.h
 * @brief High-speed streaming JSON-RPC 2.0 serializer.
 *
 * Formats JSON-RPC responses and error envelopes directly into pre-allocated
 * arena or heap buffers, bypassing DOM allocation overhead.
 *
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_JSON_STREAM_H
#define MCPKIT_PROTOCOL_JSON_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/arena.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"
#include "mcpkit/protocol/message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mcp_id_type_t type;
    union {
        const char *str;
        int64_t num;
        double num_dbl;
    } val;
} mcp_json_rpc_id_t;

static inline mcp_json_rpc_id_t mcp_json_rpc_id_from_string(const char *str) {
    mcp_json_rpc_id_t id;
    id.type = MCP_ID_STRING;
    id.val.str = str;
    return id;
}

static inline mcp_json_rpc_id_t mcp_json_rpc_id_from_int(int64_t num) {
    mcp_json_rpc_id_t id;
    id.type = MCP_ID_NUMBER;
    id.val.num = num;
    return id;
}

static inline mcp_json_rpc_id_t mcp_json_rpc_id_null(void) {
    mcp_json_rpc_id_t id;
    id.type = MCP_ID_NONE;
    id.val.str = NULL;
    return id;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    mcp_arena_t *arena;
    const mcp_allocator_t *allocator;
} mcp_json_stream_t;

/**
 * @brief Initializes a JSON stream using an arena allocator.
 *
 * Buffer will grow using `mcp_arena_realloc`. Calling `mcp_json_stream_free` is optional
 * (the arena releases all memory at reset/destroy).
 *
 * @param s Stream to initialize.
 * @param arena Arena handle; must not be NULL.
 */
void mcp_json_stream_init_arena(mcp_json_stream_t *s, mcp_arena_t *arena);

/**
 * @brief Initializes a JSON stream using a standard heap allocator.
 *
 * Buffer will grow using `allocator->realloc_fn`. Must be released with `mcp_json_stream_free`.
 *
 * @param s Stream to initialize.
 * @param allocator Pluggable allocator, or NULL for standard libc malloc/free.
 */
void mcp_json_stream_init_allocator(mcp_json_stream_t *s, const mcp_allocator_t *allocator);

/**
 * @brief Releases any heap memory owned by the stream when initialized with allocator.
 *
 * Safe no-op if stream was initialized with an arena or is empty.
 *
 * @param s Stream handle.
 */
void mcp_json_stream_free(mcp_json_stream_t *s);

/**
 * @brief Appends raw unescaped bytes to the stream buffer.
 *
 * @param s Stream handle.
 * @param str Raw string bytes.
 * @param len Length in bytes.
 */
void mcp_json_stream_write_raw(mcp_json_stream_t *s, const char *str, size_t len);

/**
 * @brief Appends a JSON-escaped, quoted string to the stream buffer.
 *
 * @param s Stream handle.
 * @param str String to escape and quote; NULL writes `null`.
 */
void mcp_json_stream_write_string(mcp_json_stream_t *s, const char *str);

/**
 * @brief Appends a signed 64-bit integer to the stream buffer.
 *
 * @param s Stream handle.
 * @param val Signed integer.
 */
void mcp_json_stream_write_int64(mcp_json_stream_t *s, int64_t val);

/**
 * @brief Appends an unsigned 64-bit integer to the stream buffer.
 *
 * @param s Stream handle.
 * @param val Unsigned integer.
 */
void mcp_json_stream_write_uint64(mcp_json_stream_t *s, uint64_t val);

/**
 * @brief Appends a boolean literal (`true` or `false`).
 *
 * @param s Stream handle.
 * @param val Boolean value.
 */
void mcp_json_stream_write_bool(mcp_json_stream_t *s, bool val);

/**
 * @brief Appends `null`.
 *
 * @param s Stream handle.
 */
void mcp_json_stream_write_null(mcp_json_stream_t *s);

/**
 * @brief Serializes a complete JSON-RPC 2.0 success response envelope.
 *
 * Writes: `{"jsonrpc":"2.0","id":<id>,"result":<result_json_raw>}`.
 *
 * @param stream Target stream.
 * @param id RPC ID (string, number, or null).
 * @param result_json_raw Pre-formatted raw JSON text for the "result" property.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_serialize_response_stream(mcp_json_stream_t *stream,
                                           const mcp_json_rpc_id_t *id,
                                           const char *result_json_raw);

/**
 * @brief Serializes a complete JSON-RPC 2.0 error response envelope.
 *
 * Writes: `{"jsonrpc":"2.0","id":<id>,"error":{"code":<code>,"message":"<escaped_message>"}}`.
 *
 * @param stream Target stream.
 * @param id RPC ID (string, number, or null).
 * @param code JSON-RPC error code (e.g. -32601).
 * @param message Human-readable error message.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_serialize_error_stream(mcp_json_stream_t *stream,
                                        const mcp_json_rpc_id_t *id,
                                        int code,
                                        const char *message);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_JSON_STREAM_H */
