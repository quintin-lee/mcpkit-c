/**
 * @file async_server.h
 * @brief High-concurrency unified async server for Streamable HTTP/SSE and WebSocket.
 * @ingroup mcpkit-transport
 */

#ifndef MCPKIT_TRANSPORT_ASYNC_SERVER_H
#define MCPKIT_TRANSPORT_ASYNC_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/server/server.h"
#include "mcpkit/runtime/executor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_async_server mcp_async_server_t;

typedef struct {
    uint16_t port;                /**< Port to listen on (0 = ephemeral). */
    const char *bind_addr;        /**< Interface to bind (default "0.0.0.0" if NULL). */
    size_t max_connections;       /**< Max concurrent connections (default 10000 if 0). */
    uint32_t keepalive_timeout_s; /**< Idle session timeout in seconds (default 60 if 0). */
    bool enable_websocket;        /**< Accept WebSocket upgrade requests (default true). */
    bool enable_sse;              /**< Accept SSE streaming requests (default true). */
    size_t outbox_high_watermark; /**< Outbox backpressure high watermark in bytes (default 1MB if 0). */
    size_t outbox_low_watermark;  /**< Outbox backpressure low watermark in bytes (default 256KB if 0). */
    size_t outbox_max_drop_bytes; /**< Max outbox size before slow-client stall eviction (default 4MB if 0). */
    uint32_t stall_timeout_ms;    /**< Write stall timeout in ms before eviction (default 5000 if 0). */
    size_t socket_sndbuf;         /**< Kernel SO_SNDBUF override in bytes (0 for system default). */
    size_t sse_event_ring_capacity; /**< SSE event ring buffer capacity per session (default 128 if 0). */
} mcp_async_server_config_t;

/**
 * @brief Creates an async server instance.
 *
 * pool is required and must not be NULL (returns MCP_ERR_INVALID_ARGUMENT).
 * Caller retains ownership of server and pool, and must keep them alive until
 * after mcp_async_server_destroy returns.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server instance to dispatch requests to.
 * @param pool Executor threadpool for non-blocking task processing.
 * @param cfg Configuration options; may be NULL for defaults.
 * @return Owned mcp_async_server_t handle or NULL on failure.
 */
mcp_async_server_t *mcp_async_server_create(mcp_context_t *ctx, mcp_server_t *server,
                                            mcp_executor_t *pool,
                                            const mcp_async_server_config_t *cfg);

/**
 * @brief Returns the actual bound TCP port (useful when port 0 was passed).
 *
 * @param s Server handle.
 * @return Bound TCP port, or 0 on error.
 */
uint16_t mcp_async_server_get_port(const mcp_async_server_t *s);

/**
 * @brief Returns the number of connections currently in backpressured (read paused) state.
 *
 * @param s Server handle.
 * @return Count of connections exceeding outbox high watermark.
 */
size_t mcp_async_server_get_backpressured_count(const mcp_async_server_t *s);

/**
 * @brief Returns total queued outbox bytes across all active connections.
 *
 * @param s Server handle.
 * @return Total queued bytes.
 */
size_t mcp_async_server_get_queued_write_bytes(const mcp_async_server_t *s);

/**
 * @brief Sends an event/payload to the active SSE connection of a session.
 *
 * @param ctx Context; may be NULL.
 * @param s Server handle.
 * @param session_id Target session ID.
 * @param data Data buffer to send.
 * @param len Length of data in bytes.
 * @return MCP_OK on success, MCP_ERR_NOT_FOUND if session doesn't exist,
 *         MCP_ERR_DISCONNECTED if no active SSE stream, or error status.
 */
mcp_status_t mcp_async_server_push_sse(mcp_context_t *ctx, mcp_async_server_t *s,
                                       const char *session_id, const char *data, size_t len);

/**
 * @brief Destroys an async server and releases all network and session resources.
 *
 * @param ctx Context; may be NULL.
 * @param s Server handle to destroy; NULL is a no-op.
 */
void mcp_async_server_destroy(mcp_context_t *ctx, mcp_async_server_t *s);

/**
 * @brief Runs the server event loop on the calling thread until stopped.
 *
 * @param ctx Context; may be NULL.
 * @param s Server handle.
 * @return MCP_OK when stopped cleanly, error status otherwise.
 */
mcp_status_t mcp_async_server_run(mcp_context_t *ctx, mcp_async_server_t *s);

/**
 * @brief Single-steps the event loop for at most timeout_ms milliseconds.
 *
 * @param ctx Context; may be NULL.
 * @param s Server handle.
 * @param timeout_ms Max wait in milliseconds (-1 for indefinite).
 * @return MCP_OK on step completion.
 */
mcp_status_t mcp_async_server_step(mcp_context_t *ctx, mcp_async_server_t *s, int timeout_ms);

/**
 * @brief Signals the server event loop to stop. Thread-safe.
 *
 * @param ctx Context; may be NULL.
 * @param s Server handle.
 */
void           mcp_async_server_stop(mcp_context_t *ctx, mcp_async_server_t *s);

/**
 * @brief Gracefully drains an async server, closing listener and waiting for in-flight requests.
 *
 * Stops accepting new connections immediately by closing the listen socket.
 * Signals existing clients to close:
 *   - HTTP: pending responses carry `Connection: close`.
 *   - WebSocket: sends RFC 6455 1001 (Going Away) Close frame.
 *   - SSE: terminates event streams.
 *
 * If timeout_ms > 0, waits up to timeout_ms for active connections to reach 0.
 * If timeout_ms <= 0, waits indefinitely.
 * If timeout expires before all connections close, forcefully closes remaining connections
 * and returns MCP_ERR_TIMEOUT.
 *
 * @param ctx Context; may be NULL.
 * @param s Server handle.
 * @param timeout_ms Maximum time to wait in ms (<= 0 for indefinite).
 * @return MCP_OK if all connections drained cleanly, MCP_ERR_TIMEOUT if deadline exceeded,
 *         or other error status.
 */
mcp_status_t mcp_async_server_drain(mcp_context_t *ctx, mcp_async_server_t *s, int timeout_ms);

/**
 * @brief Returns the current number of active connections tracked by the server.
 * @param s Async server handle; may be NULL.
 * @return Active connection count, or 0 if s is NULL.
 */
size_t mcp_async_server_active_connections(const mcp_async_server_t *s);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_TRANSPORT_ASYNC_SERVER_H */
