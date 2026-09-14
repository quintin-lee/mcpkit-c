add_executable(test_version unit/test_version.c)
target_link_libraries(test_version PRIVATE mcpkit_core)
add_test(NAME test_version COMMAND test_version)

add_executable(test_error unit/test_error.c)
target_link_libraries(test_error PRIVATE mcpkit_core)
add_test(NAME test_error COMMAND test_error)

add_executable(test_logger unit/test_logger.c)
target_link_libraries(test_logger PRIVATE mcpkit_core)
add_test(NAME test_logger COMMAND test_logger)

add_executable(test_context unit/test_context.c)
target_link_libraries(test_context PRIVATE mcpkit_core)
add_test(NAME test_context COMMAND test_context)
