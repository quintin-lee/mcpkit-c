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

add_executable(test_json_value unit/test_json_value.c)
target_link_libraries(test_json_value PRIVATE mcpkit_core)
add_test(NAME test_json_value COMMAND test_json_value)

add_executable(test_json_roundtrip unit/test_json_roundtrip.c)
target_link_libraries(test_json_roundtrip PRIVATE mcpkit_core)
add_test(NAME test_json_roundtrip COMMAND test_json_roundtrip)

add_executable(test_json_negative unit/test_json_negative.c)
target_link_libraries(test_json_negative PRIVATE mcpkit_core)
add_test(NAME test_json_negative COMMAND test_json_negative)

add_executable(test_json_backend unit/test_json_backend.c)
target_link_libraries(test_json_backend PRIVATE mcpkit_core)
add_test(NAME test_json_backend COMMAND test_json_backend)
