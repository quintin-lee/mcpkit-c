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

add_executable(test_rpc_envelope unit/test_rpc_envelope.c)
target_link_libraries(test_rpc_envelope PRIVATE mcpkit_core)
add_test(NAME test_rpc_envelope COMMAND test_rpc_envelope)

add_executable(test_initialize unit/test_initialize.c)
target_link_libraries(test_initialize PRIVATE mcpkit_core)
add_test(NAME test_initialize COMMAND test_initialize)

add_executable(test_schema unit/test_schema.c)
target_link_libraries(test_schema PRIVATE mcpkit_core)
add_test(NAME test_schema COMMAND test_schema)

add_executable(test_validate_negative unit/test_validate_negative.c)
target_link_libraries(test_validate_negative PRIVATE mcpkit_core)
add_test(NAME test_validate_negative COMMAND test_validate_negative)

add_executable(test_error_mapping unit/test_error_mapping.c)
target_link_libraries(test_error_mapping PRIVATE mcpkit_core)
add_test(NAME test_error_mapping COMMAND test_error_mapping)

add_executable(test_registry unit/test_registry.c)
target_link_libraries(test_registry PRIVATE mcpkit_core)
add_test(NAME test_registry COMMAND test_registry)

add_executable(test_session unit/test_session.c)
target_link_libraries(test_session PRIVATE mcpkit_core)
add_test(NAME test_session COMMAND test_session)

add_executable(test_dispatcher unit/test_dispatcher.c)
target_link_libraries(test_dispatcher PRIVATE mcpkit_core)
add_test(NAME test_dispatcher COMMAND test_dispatcher)

add_executable(test_visibility unit/test_visibility.c)
target_link_libraries(test_visibility PRIVATE mcpkit_core)
add_test(NAME test_visibility COMMAND test_visibility)

add_executable(test_apps unit/test_apps.c)
target_link_libraries(test_apps PRIVATE mcpkit_core)
add_test(NAME test_apps COMMAND test_apps)

add_executable(test_apps_e2e unit/test_apps_e2e.c)
target_link_libraries(test_apps_e2e PRIVATE mcpkit_core)
add_test(NAME test_apps_e2e COMMAND test_apps_e2e)

add_executable(test_transport unit/test_transport.c)
target_link_libraries(test_transport PRIVATE mcpkit_core)
add_test(NAME test_transport COMMAND test_transport)

add_executable(test_stdio_limits unit/test_stdio_limits.c)
target_link_libraries(test_stdio_limits PRIVATE mcpkit_core)
add_test(NAME test_stdio_limits COMMAND test_stdio_limits)

add_executable(test_stdio_loopback unit/test_stdio_loopback.c)
target_link_libraries(test_stdio_loopback PRIVATE mcpkit_core)
add_test(NAME test_stdio_loopback COMMAND test_stdio_loopback)

add_executable(test_client unit/test_client.c)
target_link_libraries(test_client PRIVATE mcpkit_core)
add_test(NAME test_client COMMAND test_client)

add_executable(test_client_e2e unit/test_client_e2e.c)
target_link_libraries(test_client_e2e PRIVATE mcpkit_core)
add_test(NAME test_client_e2e COMMAND test_client_e2e)

add_executable(test_executor unit/test_executor.c)
target_link_libraries(test_executor PRIVATE mcpkit_core)
add_test(NAME test_executor COMMAND test_executor)

add_executable(test_timer unit/test_timer.c)
target_link_libraries(test_timer PRIVATE mcpkit_core)
add_test(NAME test_timer COMMAND test_timer)

add_executable(test_loop unit/test_loop.c)
target_link_libraries(test_loop PRIVATE mcpkit_core)
add_test(NAME test_loop COMMAND test_loop)
