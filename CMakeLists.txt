cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" MCPKIT_VERSION_FILE)
string(STRIP "${MCPKIT_VERSION_FILE}" MCPKIT_VERSION_FILE)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _mcpkit_version_re "${MCPKIT_VERSION_FILE}")
if(NOT _mcpkit_version_re)
  message(FATAL_ERROR "VERSION file must contain a value of the form MAJOR.MINOR.PATCH")
endif()
set(MCPKIT_VERSION "${_mcpkit_version_re}")
string(REPLACE "." ";" _mcpkit_version_parts "${MCPKIT_VERSION}")
list(GET _mcpkit_version_parts 0 MCPKIT_VERSION_MAJOR)
list(GET _mcpkit_version_parts 1 MCPKIT_VERSION_MINOR)
list(GET _mcpkit_version_parts 2 MCPKIT_VERSION_PATCH)

project(mcpkit VERSION "${MCPKIT_VERSION}" LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Always emit compile_commands.json for LSP/clangd; harmless for other generators.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

configure_file(cmake/mcpkit_version_generated.h.in
               "${CMAKE_BINARY_DIR}/include/mcpkit_version_generated.h")

include(cmake/MCPKitOptions.cmake)
include(cmake/MCPKitSanitizers.cmake)
mcpkit_apply_sanitizers()

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
target_include_directories(mcpkit_core PRIVATE "${CMAKE_BINARY_DIR}/include")

if(MCPKIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MCPKIT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MCPKIT_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# Packaging
install(TARGETS mcpkit_core LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/mcpkit DESTINATION include)
set(CPACK_PACKAGE_NAME "mcpkit")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "TGZ")
include(CPack)
