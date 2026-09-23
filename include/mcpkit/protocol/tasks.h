/**
 * @file tasks.h
 * @brief MCP Tasks extension data structures and manager (SEP-2663).
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_TASKS_H
#define MCPKIT_PROTOCOL_TASKS_H

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCP Task execution status values.
 */
typedef enum mcp_task_status {
    MCP_TASK_STATUS_WORKING = 0,
    MCP_TASK_STATUS_INPUT_REQUIRED,
    MCP_TASK_STATUS_COMPLETED,
    MCP_TASK_STATUS_FAILED,
    MCP_TASK_STATUS_CANCELLED,
} mcp_task_status_t;

/**
 * @brief Returns the canonical string for a task status.
 */
const char *mcp_task_status_name(mcp_task_status_t status);

/**
 * @brief Snapshot of a task's current state.
 */
typedef struct mcp_task_desc {
    char task_id[64];
    mcp_task_status_t status;
    uint64_t ttl_ms;
    uint64_t poll_interval_ms;
    char status_message[256];
    const mcp_json_value_t *result;         /* borrowed */
    const mcp_json_value_t *error;          /* borrowed */
    const mcp_json_value_t *input_requests; /* borrowed */
} mcp_task_desc_t;

/**
 * @brief Opaque server-side task manager.
 */
typedef struct mcp_task_mgr mcp_task_mgr_t;

/**
 * @brief Creates a new task manager.
 */
mcp_task_mgr_t *mcp_task_mgr_new(mcp_context_t *ctx);

/**
 * @brief Destroys a task manager and all stored tasks.
 */
void mcp_task_mgr_free(mcp_context_t *ctx, mcp_task_mgr_t *mgr);

/**
 * @brief Creates a new task in `working` status.
 *
 * @param ctx               Context.
 * @param mgr               Task manager.
 * @param task_id           Unique task identifier.
 * @param ttl_ms            Task time-to-live hint in milliseconds.
 * @param poll_interval_ms  Suggested client polling interval in milliseconds.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if task_id already exists;
 *         MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_task_mgr_create(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                 const char *task_id, uint64_t ttl_ms,
                                 uint64_t poll_interval_ms);

/**
 * @brief Updates status and message of an existing task.
 */
mcp_status_t mcp_task_mgr_set_status(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                     const char *task_id, mcp_task_status_t status,
                                     const char *msg);

/**
 * @brief Sets the terminal result of a task and transitions it to `completed`.
 *
 * Transfers ownership of `result` on MCP_OK. On error, caller retains ownership.
 */
mcp_status_t mcp_task_mgr_set_result(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                     const char *task_id, mcp_json_value_t *result);

/**
 * @brief Sets the terminal error of a task and transitions it to `failed`.
 *
 * Transfers ownership of `error` on MCP_OK. On error, caller retains ownership.
 */
mcp_status_t mcp_task_mgr_set_error(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                    const char *task_id, mcp_json_value_t *error);

/**
 * @brief Sets input requests and transitions task to `input_required`.
 *
 * Transfers ownership of `input_requests` on MCP_OK. On error, caller retains ownership.
 */
mcp_status_t mcp_task_mgr_set_input_requests(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                             const char *task_id, mcp_json_value_t *input_requests);

/**
 * @brief Retrieves a task description snapshot.
 */
mcp_status_t mcp_task_mgr_get(mcp_context_t *ctx, const mcp_task_mgr_t *mgr,
                              const char *task_id, mcp_task_desc_t *out_desc);

/**
 * @brief Transitions a non-terminal task to `cancelled`.
 */
mcp_status_t mcp_task_mgr_cancel(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                 const char *task_id);

/**
 * @brief Serializes a task descriptor to a JSON object conforming to the Tasks extension spec.
 */
mcp_json_value_t *mcp_task_desc_to_json(mcp_context_t *ctx, const mcp_task_desc_t *desc);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_TASKS_H */
