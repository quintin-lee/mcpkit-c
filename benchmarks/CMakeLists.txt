cmake_minimum_required(VERSION 3.20)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_executable(bench_json_rpc bench_json_rpc.c)
target_link_libraries(bench_json_rpc PRIVATE mcpkit_core)

add_executable(bench_http_throughput bench_http_throughput.c)
target_link_libraries(bench_http_throughput PRIVATE mcpkit_core pthread)

add_executable(bench_ws_throughput bench_ws_throughput.c)
target_link_libraries(bench_ws_throughput PRIVATE mcpkit_core pthread)
