include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_version unit/test_version.c)
target_link_libraries(test_version PRIVATE mcpkit_core)
target_include_directories(test_version PRIVATE "${CMAKE_BINARY_DIR}/include")
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

add_executable(test_client_providers unit/test_client_providers.c)
target_link_libraries(test_client_providers PRIVATE mcpkit_core)
add_test(NAME test_client_providers COMMAND test_client_providers)

add_executable(test_executor unit/test_executor.c)
target_link_libraries(test_executor PRIVATE mcpkit_core)
add_test(NAME test_executor COMMAND test_executor)

add_executable(test_timer unit/test_timer.c)
target_link_libraries(test_timer PRIVATE mcpkit_core)
add_test(NAME test_timer COMMAND test_timer)

add_executable(test_loop unit/test_loop.c)
target_link_libraries(test_loop PRIVATE mcpkit_core)
add_test(NAME test_loop COMMAND test_loop)

add_executable(test_plugin unit/test_plugin.c)
target_link_libraries(test_plugin PRIVATE mcpkit_core)
add_test(NAME test_plugin COMMAND test_plugin)

add_executable(test_alloc_fail unit/test_alloc_fail.c)
target_link_libraries(test_alloc_fail PRIVATE mcpkit_core)
add_test(NAME test_alloc_fail COMMAND test_alloc_fail)

if(MCPKIT_BUILD_HTTP)
  add_executable(test_http unit/test_http.c)
  target_link_libraries(test_http PRIVATE mcpkit_core)
  add_test(NAME test_http COMMAND test_http)
  add_executable(test_http_serve unit/test_http_serve.c)
  target_link_libraries(test_http_serve PRIVATE mcpkit_core)
  add_test(NAME test_http_serve COMMAND test_http_serve)
endif()

add_executable(test_json_fuzz fuzz/test_json_fuzz.c)
target_link_libraries(test_json_fuzz PRIVATE mcpkit_core)
add_test(NAME test_json_fuzz COMMAND test_json_fuzz)

add_executable(test_schema_fuzz fuzz/test_schema_fuzz.c)
target_link_libraries(test_schema_fuzz PRIVATE mcpkit_core)
add_test(NAME test_schema_fuzz COMMAND test_schema_fuzz)

add_executable(test_message_fuzz fuzz/test_message_fuzz.c)
target_link_libraries(test_message_fuzz PRIVATE mcpkit_core)
add_test(NAME test_message_fuzz COMMAND test_message_fuzz)

if(MCPKIT_BUILD_HTTP)
  add_executable(test_http_fuzz fuzz/test_http_fuzz.c)
  target_link_libraries(test_http_fuzz PRIVATE mcpkit_core)
  add_test(NAME test_http_fuzz COMMAND test_http_fuzz)
endif()

if(MCPKIT_BUILD_TOOLS AND MCPKIT_BUILD_EXAMPLES)
  add_test(NAME cli_acceptance
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/cli_accept.sh
                   ${CMAKE_BINARY_DIR}/tools/mcpkit-cli
                   ${CMAKE_BINARY_DIR}/examples/stdio-server
                   ${CMAKE_BINARY_DIR}/examples/threadpool-server
                   ${CMAKE_BINARY_DIR}/examples/apps-host)
endif()

if(MCPKIT_BUILD_FUZZ)
  add_executable(fuzz_json_stdin fuzz/fuzz_json_stdin.c)
  target_link_libraries(fuzz_json_stdin PRIVATE mcpkit_core)
  add_executable(fuzz_schema_stdin fuzz/fuzz_schema_stdin.c)
  target_link_libraries(fuzz_schema_stdin PRIVATE mcpkit_core)
  add_executable(fuzz_message_stdin fuzz/fuzz_message_stdin.c)
  target_link_libraries(fuzz_message_stdin PRIVATE mcpkit_core)
  if(MCPKIT_BUILD_HTTP)
    add_executable(fuzz_http_stdin fuzz/fuzz_http_stdin.c)
    target_link_libraries(fuzz_http_stdin PRIVATE mcpkit_core)
  endif()
endif()

if(MCPKIT_BUILD_SOCKET)
  add_executable(test_socket unit/test_socket.c)
  target_link_libraries(test_socket PRIVATE mcpkit_core)
  add_test(NAME test_socket COMMAND test_socket)
  add_executable(test_timeout unit/test_timeout.c)
  target_link_libraries(test_timeout PRIVATE mcpkit_core)
  add_test(NAME test_timeout COMMAND test_timeout)
  add_executable(test_socket_serve unit/test_socket_serve.c)
  target_link_libraries(test_socket_serve PRIVATE mcpkit_core)
  add_test(NAME test_socket_serve COMMAND test_socket_serve)
endif()

add_executable(test_pagination unit/test_pagination.c)
target_link_libraries(test_pagination PRIVATE mcpkit_core)
add_test(NAME test_pagination COMMAND test_pagination)

add_executable(test_shutdown unit/test_shutdown.c)
target_link_libraries(test_shutdown PRIVATE mcpkit_core)
add_test(NAME test_shutdown COMMAND test_shutdown)

add_executable(test_observability unit/test_observability.c)
target_link_libraries(test_observability PRIVATE mcpkit_core)
add_test(NAME test_observability COMMAND test_observability)
add_executable(test_notify_client unit/test_notify_client.c)
target_link_libraries(test_notify_client PRIVATE mcpkit_core)
add_test(NAME test_notify_client COMMAND test_notify_client)
add_executable(test_advanced_methods unit/test_advanced_methods.c)
target_link_libraries(test_advanced_methods PRIVATE mcpkit_core)
add_test(NAME test_advanced_methods COMMAND test_advanced_methods)

add_executable(test_stdio_serve_with_client unit/test_stdio_serve_with_client.c)
target_link_libraries(test_stdio_serve_with_client PRIVATE mcpkit_core)
add_test(NAME test_stdio_serve_with_client COMMAND test_stdio_serve_with_client)

add_executable(test_protocol_statelessness unit/test_protocol_statelessness.c)
target_link_libraries(test_protocol_statelessness PRIVATE mcpkit_core)
add_test(NAME test_protocol_statelessness COMMAND test_protocol_statelessness)

add_executable(test_mrtr unit/test_mrtr.c)
target_link_libraries(test_mrtr PRIVATE mcpkit_core)
add_test(NAME test_mrtr COMMAND test_mrtr)

add_executable(test_subscriptions_stream unit/test_subscriptions_stream.c)
target_link_libraries(test_subscriptions_stream PRIVATE mcpkit_core)
add_test(NAME test_subscriptions_stream COMMAND test_subscriptions_stream)

add_executable(test_trace_context unit/test_trace_context.c)
target_link_libraries(test_trace_context PRIVATE mcpkit_core)
add_test(NAME test_trace_context COMMAND test_trace_context)

add_executable(test_tasks_extension unit/test_tasks_extension.c)
target_link_libraries(test_tasks_extension PRIVATE mcpkit_core)
add_test(NAME test_tasks_extension COMMAND test_tasks_extension)

if(MCPKIT_BUILD_TLS)
  # Generate a self-signed cert in the build directory for the TLS test.
  # The cert is written to the tests/ subdirectory of the build tree.
  add_custom_target(tls_test_cert ALL
    COMMAND ${CMAKE_COMMAND} -E env openssl req -x509 -newkey rsa:2048
            -nodes
            -keyout ${CMAKE_CURRENT_BINARY_DIR}/tls_test.key
            -out    ${CMAKE_CURRENT_BINARY_DIR}/tls_test.crt
            -days 1 -subj "/CN=localhost" 2>&1
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating self-signed cert for TLS test"
  )
  add_executable(test_tls_transport unit/test_tls_transport.c)
  target_link_libraries(test_tls_transport PRIVATE mcpkit_core)
  add_dependencies(test_tls_transport tls_test_cert)
  add_test(NAME test_tls_transport COMMAND test_tls_transport
           ${CMAKE_CURRENT_BINARY_DIR}/tls_test.crt
           ${CMAKE_CURRENT_BINARY_DIR}/tls_test.key)
endif()
