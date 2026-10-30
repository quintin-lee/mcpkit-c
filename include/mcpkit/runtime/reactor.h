/**
 * @file reactor.h
 * @brief Non-blocking I/O event reactor using epoll / poll multiplexing.
 * @ingroup mcpkit-runtime
 *
 * Provides single-threaded event loop multiplexing for socket descriptors.
 * Supports read, write, and error event notifications, timeout-bounded
 * single stepping, run loops, and cross-thread wakeups via eventfd / self-pipe.
 */

#ifndef MCPKIT_RUNTIME_REACTOR_H
#define MCPKIT_RUNTIME_REACTOR_H

#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque forward declaration of the event reactor.
 */
typedef struct mcp_reactor mcp_reactor_t;

/**
 * @brief Event bitmask flags.
 */
typedef uint32_t mcp_event_mask_t;

#define MCP_EVENT_READ   (1u << 0)
#define MCP_EVENT_WRITE  (1u << 1)
#define MCP_EVENT_ERROR  (1u << 2)

/**
 * @brief Callback invoked when monitored events trigger on a registered descriptor.
 *
 * @param ctx Context pointer passed to reactor step / run.
 * @param fd Active file descriptor.
 * @param events Mask of events that fired (subset of MCP_EVENT_*).
 * @param user User-defined pointer supplied at registration.
 */
typedef void (*mcp_reactor_cb_t)(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user);

/**
 * @brief Creates a new non-blocking event reactor.
 *
 * @param ctx Context providing memory allocator; may be NULL to use default allocator.
 * @return Owned pointer to reactor, or NULL on allocation or kernel resource failure.
 */
mcp_reactor_t *mcp_reactor_create(mcp_context_t *ctx);

/**
 * @brief Destroys an event reactor and releases all kernel and memory resources.
 *
 * Registered descriptors are NOT closed by the reactor; caller retains ownership of fds.
 * NULL-safe.
 *
 * @param ctx Context providing memory allocator; may be NULL.
 * @param r Reactor to destroy, or NULL.
 */
void mcp_reactor_destroy(mcp_context_t *ctx, mcp_reactor_t *r);

/**
 * @brief Registers a file descriptor with the reactor.
 *
 * @param ctx Context; may be NULL.
 * @param r Reactor instance.
 * @param fd Non-negative file descriptor.
 * @param events Mask of events to monitor (MCP_EVENT_READ, MCP_EVENT_WRITE, MCP_EVENT_ERROR).
 * @param cb Non-NULL callback function.
 * @param user User pointer passed to callback.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if r is NULL, fd < 0, cb is NULL, or invalid event mask;
 *         MCP_ERR_ALREADY_EXISTS if fd is already registered;
 *         MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_IO on OS poll/epoll control failure.
 */
mcp_status_t mcp_reactor_add(mcp_context_t *ctx, mcp_reactor_t *r, int fd,
                             mcp_event_mask_t events, mcp_reactor_cb_t cb, void *user);

/**
 * @brief Modifies monitored events for an already registered descriptor.
 *
 * @param ctx Context; may be NULL.
 * @param r Reactor instance.
 * @param fd Non-negative file descriptor already registered.
 * @param events New event bitmask.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if r is NULL, fd < 0, or invalid event mask;
 *         MCP_ERR_NOT_FOUND if fd is not registered;
 *         MCP_ERR_IO on OS poll/epoll control failure.
 */
mcp_status_t mcp_reactor_modify(mcp_context_t *ctx, mcp_reactor_t *r, int fd,
                                mcp_event_mask_t events);

/**
 * @brief Deregisters a file descriptor from the reactor.
 *
 * The caller remains responsible for closing the descriptor.
 *
 * @param ctx Context; may be NULL.
 * @param r Reactor instance.
 * @param fd Non-negative file descriptor.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if r is NULL or fd < 0;
 *         MCP_ERR_NOT_FOUND if fd was not registered;
 *         MCP_ERR_IO on OS poll/epoll control failure.
 */
mcp_status_t mcp_reactor_remove(mcp_context_t *ctx, mcp_reactor_t *r, int fd);

/**
 * @brief Executes a single iteration of event polling and dispatches ready callbacks.
 *
 * @param ctx Context passed to callbacks.
 * @param r Reactor instance.
 * @param timeout_ms Maximum time to wait in milliseconds.
 *                   < 0: block indefinitely until an event occurs or woken up.
 *                   0: non-blocking poll.
 *                   > 0: wait up to timeout_ms.
 * @return MCP_OK on successful step (including zero events or wakeup);
 *         MCP_ERR_INVALID_ARGUMENT if r is NULL;
 *         MCP_ERR_CANCELLED if process shutdown was requested;
 *         MCP_ERR_IO on poll error.
 */
mcp_status_t mcp_reactor_step(mcp_context_t *ctx, mcp_reactor_t *r, int timeout_ms);

/**
 * @brief Runs the event loop until mcp_reactor_stop() or mcp_request_shutdown() is called.
 *
 * @param ctx Context passed to callbacks.
 * @param r Reactor instance.
 * @return MCP_OK on clean stop;
 *         MCP_ERR_INVALID_ARGUMENT if r is NULL;
 *         MCP_ERR_CANCELLED if shutdown was requested;
 *         MCP_ERR_IO on unrecoverable poll error.
 */
mcp_status_t mcp_reactor_run(mcp_context_t *ctx, mcp_reactor_t *r);

/**
 * @brief Requests the reactor loop to stop.
 *
 * Safe to call from any thread or from within an event callback.
 * Automatically wakes up the reactor if it is waiting in step/run.
 * NULL-safe.
 *
 * @param ctx Context; may be NULL.
 * @param r Reactor instance, or NULL.
 */
void mcp_reactor_stop(mcp_context_t *ctx, mcp_reactor_t *r);

/**
 * @brief Wakes up the reactor from a blocking wait.
 *
 * Safe to call from any thread or signal handler.
 *
 * @param ctx Context; may be NULL.
 * @param r Reactor instance.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if r is NULL;
 *         MCP_ERR_IO on write failure.
 */
mcp_status_t mcp_reactor_wakeup(mcp_context_t *ctx, mcp_reactor_t *r);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_RUNTIME_REACTOR_H */
