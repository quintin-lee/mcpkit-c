add_executable(mcpkit-cli mcpkit-cli/main.c)
target_link_libraries(mcpkit-cli PRIVATE mcpkit_core)
