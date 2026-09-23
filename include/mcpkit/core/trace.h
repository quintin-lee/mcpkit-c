/**
 * @file trace.h
 * @brief W3C Trace Context propagation helpers for MCP _meta (SEP-414).
 * @ingroup mcpkit-core
 */

#ifndef MCPKIT_CORE_TRACE_H
#define MCPKIT_CORE_TRACE_H

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Represents W3C Trace Context headers.
 */
typedef struct mcp_trace_context {
    char traceparent[64];
    char tracestate[256];
    char baggage[256];
} mcp_trace_context_t;

/**
 * @brief Extracts W3C Trace Context fields from a message's `_meta` JSON object.
 *
 * Reads "traceparent", "tracestate", and "baggage" string fields if present.
 *
 * @param ctx   Context.
 * @param meta  `_meta` JSON value (must be a JSON object).
 * @param out   Output trace context structure to populate.
 * @return MCP_OK on success (even if fields are absent/empty);
 *         MCP_ERR_INVALID_ARGUMENT if `meta` is NULL/not-an-object or `out` is NULL.
 */
mcp_status_t mcp_trace_extract_from_meta(mcp_context_t *ctx,
                                         const mcp_json_value_t *meta,
                                         mcp_trace_context_t *out);

/**
 * @brief Injects non-empty W3C Trace Context fields into a `_meta` JSON object.
 *
 * Sets "traceparent", "tracestate", and "baggage" in `meta` if their values in `trace`
 * are non-empty.
 *
 * @param ctx   Context.
 * @param meta  `_meta` JSON value to modify (must be a JSON object).
 * @param trace Trace context to read from.
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if arguments are NULL or `meta` is not an object;
 *         MCP_ERR_NOMEM on memory allocation failure.
 */
mcp_status_t mcp_trace_inject_into_meta(mcp_context_t *ctx,
                                        mcp_json_value_t *meta,
                                        const mcp_trace_context_t *trace);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_TRACE_H */
