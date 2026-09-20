/**
 * @file mcpkit.h
 * Umbrella header: pulls in every public API by layer.
 *
 * core -> logging -> json -> protocol -> server -> transport -> client ->
 * runtime -> apps -> plugin. Including this one header is enough for
 * applications that use several layers; library authors that need only a
 * slice can include the individual headers directly.
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
