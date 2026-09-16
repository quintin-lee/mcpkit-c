add_executable(minimal-server minimal-server/main.c)
target_link_libraries(minimal-server PRIVATE mcpkit_core)

add_executable(multi-session multi-session/main.c)
target_link_libraries(multi-session PRIVATE mcpkit_core)

add_executable(stdio-server stdio-server/main.c)
target_link_libraries(stdio-server PRIVATE mcpkit_core)

add_executable(prompt-server prompt-server/main.c)
target_link_libraries(prompt-server PRIVATE mcpkit_core)

add_executable(client client/main.c)
target_link_libraries(client PRIVATE mcpkit_core)

add_executable(threadpool-server threadpool-server/main.c)
target_link_libraries(threadpool-server PRIVATE mcpkit_core)

add_executable(apps-host apps-host/main.c)
target_link_libraries(apps-host PRIVATE mcpkit_core)

if(MCPKIT_BUILD_HTTP)
  add_executable(http-server http-server/main.c)
  target_link_libraries(http-server PRIVATE mcpkit_core)
endif()

if(MCPKIT_BUILD_SOCKET)
  add_executable(socket-server socket-server/main.c)
  target_link_libraries(socket-server PRIVATE mcpkit_core)
endif()
