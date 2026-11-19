/**
 * @file ring_buffer.h
 * @brief Thread-safe event ring buffer for SSE / Streamable HTTP resumption.
 *
 * Stores recent SSE messages with monotonic 64-bit event IDs. Supports
 * history replay from Last-Event-ID and expiration detection (HTTP 410).
 *
 * @ingroup mcpkit-transport
 */

#ifndef MCPKIT_TRANSPORT_RING_BUFFER_H
#define MCPKIT_TRANSPORT_RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_event_ring_buffer mcp_event_ring_buffer_t;

/**
 * @brief Callback invoked during stream replay for each missing event.
 *
 * @param id Monotonic event ID.
 * @param data Event payload (borrowed pointer valid for the duration of the callback).
 * @param len Length of data in bytes.
 * @param userdata Userdata passed to replay function.
 */
typedef void (*mcp_event_replay_fn)(uint64_t id, const char *data, size_t len, void *userdata);

/**
 * @brief Creates an event ring buffer.
 *
 * @param alloc Allocator for entry storage; NULL uses default libc allocator.
 * @param capacity Maximum number of events to retain (e.g. 128). If 0, defaults to 128.
 * @return Owned ring buffer handle, or NULL on allocation failure.
 */
mcp_event_ring_buffer_t *mcp_event_ring_buffer_create(const mcp_allocator_t *alloc,
                                                      size_t capacity);

/**
 * @brief Destroys an event ring buffer and frees all retained event payloads.
 *
 * @param rb Ring buffer handle; NULL is a no-op.
 */
void mcp_event_ring_buffer_destroy(mcp_event_ring_buffer_t *rb);

/**
 * @brief Pushes a new event into the ring buffer, assigning a monotonic event ID.
 *
 * If the payload does not already contain an SSE framing line `id:`, it is automatically
 * formatted as: `id: <id>\ndata: <data>\n\n`. If the ring buffer is at capacity, the oldest
 * event is evicted and freed, advancing `oldest_id`.
 *
 * @param rb Ring buffer handle; must not be NULL.
 * @param data Event payload (raw data or pre-framed SSE data).
 * @param len Length of data in bytes.
 * @param assigned_id Optional out-parameter to receive the assigned event ID; may be NULL.
 * @return MCP_OK on success; error code on allocation failure.
 */
mcp_status_t mcp_event_ring_buffer_push(mcp_event_ring_buffer_t *rb,
                                        const char *data,
                                        size_t len,
                                        uint64_t *assigned_id);

/**
 * @brief Replays all events with `event_id > last_event_id` in FIFO order.
 *
 * @param rb Ring buffer handle; must not be NULL.
 * @param last_event_id The client's last observed event ID.
 * @param replay_fn Callback invoked for each missed event in order.
 * @param userdata Pass-through userdata for replay_fn.
 * @return MCP_OK on success (all missed events replayed or client already up-to-date);
 *         MCP_ERR_EXPIRED if `last_event_id` has already been evicted from the buffer.
 */
mcp_status_t mcp_event_ring_buffer_replay(mcp_event_ring_buffer_t *rb,
                                          uint64_t last_event_id,
                                          mcp_event_replay_fn replay_fn,
                                          void *userdata);

/**
 * @brief Returns the oldest event ID currently preserved in the ring buffer.
 *
 * If no events have been pushed, returns 1.
 *
 * @param rb Ring buffer handle.
 * @return Oldest valid event ID.
 */
uint64_t mcp_event_ring_buffer_oldest_id(mcp_event_ring_buffer_t *rb);

/**
 * @brief Returns the most recently assigned event ID.
 *
 * If no events have been pushed, returns 0.
 *
 * @param rb Ring buffer handle.
 * @return Latest event ID.
 */
uint64_t mcp_event_ring_buffer_latest_id(mcp_event_ring_buffer_t *rb);

/**
 * @brief Returns the count of events currently retained in the buffer.
 *
 * @param rb Ring buffer handle.
 * @return Count of retained events.
 */
size_t mcp_event_ring_buffer_count(mcp_event_ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_TRANSPORT_RING_BUFFER_H */
