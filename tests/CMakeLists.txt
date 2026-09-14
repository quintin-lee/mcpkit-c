add_executable(test_version unit/test_version.c)
target_link_libraries(test_version PRIVATE mcpkit_core)
add_test(NAME test_version COMMAND test_version)

add_executable(test_error unit/test_error.c)
target_link_libraries(test_error PRIVATE mcpkit_core)
add_test(NAME test_error COMMAND test_error)
