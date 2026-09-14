cmake_minimum_required(VERSION 3.28)
project(mcpkit VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(cmake/MCPKitOptions.cmake)

add_library(mcpkit_core
  src/core/version.c
  src/core/error.c
  src/core/allocator.c
  src/logging/logger.c
)
target_include_directories(mcpkit_core PUBLIC include)

if(MCPKIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
