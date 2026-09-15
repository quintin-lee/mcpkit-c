add_executable(minimal-server minimal-server/main.c)
target_link_libraries(minimal-server PRIVATE mcpkit_core)

add_executable(multi-session multi-session/main.c)
target_link_libraries(multi-session PRIVATE mcpkit_core)
