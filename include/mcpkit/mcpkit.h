/**
 * @file mcpkit.h
 * Umbrella header: pulls in every public API by layer.
 *
 * Layers (in include order):
 * @see mcpkit-core
 * @see mcpkit-json
 * @see mcpkit-protocol
 * @see mcpkit-server
 * @see mcpkit-transport
 * @see mcpkit-client
 * @see mcpkit-runtime
 * @see mcpkit-apps
 * @see mcpkit-plugin
 * @see mcpkit-logging
 *
 * Including this one header is enough for applications that use several
 * layers; library authors that need only a slice can include the
 * individual headers directly.
 *
 * Minimal stdio server sketch:
 * @code
 * #include "mcpkit/mcpkit.h"
 *
 * static mcp_status_t echo(mcp_context_t *ctx, mcp_session_t *s,
 *                          const mcp_json_value_t *args, void *ud,
 *                          mcp_json_value_t **out) {
 *     (void)ctx; (void)s; (void)ud;
 *     *out = mcp_json_clone(ctx, args);   // echo arguments back
 *     return *out ? MCP_OK : MCP_ERR_NOMEM;
 * }
 *
 * int main(void) {
 *     mcp_server_t *srv = mcp_server_create(NULL, "demo", "0.1.0");
 *     mcp_tool_t *t = mcp_tool_new(NULL, "echo", "Echo args", NULL, echo, NULL);
 *     mcp_server_add_tool(NULL, srv, t);
 *     FILE *in = stdin, *out = stdout;
 *     mcp_transport_t *tr = mcp_stdio_transport_create(NULL, in, out);
 *     mcp_stdio_serve(NULL, srv, tr, NULL);
 *     mcp_server_destroy(NULL, srv);
 *     return 0;
 * }
 * @endcode
 */
#ifndef MCPKIT_H
#define MCPKIT_H

#include "mcpkit/core/types.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/result.h"
#include "mcpkit/core/version.h"
#include "mcpkit/core/capability.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/shutdown.h"
#include "mcpkit/logging/log.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/schema.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/validate.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/tool.h"
#include "mcpkit/server/resource.h"
#include "mcpkit/server/prompt.h"
#include "mcpkit/server/session.h"
#include "mcpkit/server/dispatcher.h"
#include "mcpkit/transport/transport.h"
#include "mcpkit/transport/stdio.h"
#include "mcpkit/transport/http.h"
#include "mcpkit/transport/streamable_http.h"
#include "mcpkit/transport/socket.h"
#include "mcpkit/client/client.h"
#include "mcpkit/runtime/task.h"
#include "mcpkit/runtime/executor.h"
#include "mcpkit/runtime/sync.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/runtime/timer.h"
#include "mcpkit/runtime/loop.h"
#include "mcpkit/apps/csp.h"
#include "mcpkit/apps/ui.h"
#include "mcpkit/plugin/plugin.h"

#endif
