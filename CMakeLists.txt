cmake_minimum_required(VERSION 3.28)
project(mcpkit VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Always emit compile_commands.json for LSP/clangd; harmless for other generators.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/MCPKitOptions.cmake)

add_library(mcpkit_core
  src/core/version.c
  src/core/error.c
  src/core/allocator.c
  src/core/context.c
  src/logging/logger.c
  src/json/builtin.c
  src/json/api.c
  src/json/schema.c
  src/protocol/message.c
  src/protocol/initialize.c
  src/protocol/validate.c
  src/server/server.c
  src/server/registry.c
  src/server/session.c
  src/server/dispatcher.c
  src/apps/csp.c
  src/apps/ui.c
  src/transport/transport.c
  src/transport/stdio.c
  src/client/client.c
  src/runtime/executor.c
  src/runtime/sync.c
  src/runtime/threadpool.c
  src/runtime/timer.c
  src/runtime/loop.c
  src/plugin/plugin.c
)

if(MCPKIT_BUILD_HTTP)
  target_sources(mcpkit_core PRIVATE
    src/transport/http.c
    src/transport/streamable_http.c
  )
endif()
target_include_directories(mcpkit_core PUBLIC include)

if(MCPKIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MCPKIT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
