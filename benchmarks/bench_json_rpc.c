#include "bench_common.h"
#include "mcpkit/mcpkit.h"
#include "mcpkit/core/arena.h"
#include "mcpkit/protocol/json_stream.h"

int main(int argc, char **argv) {
    size_t iterations = 100000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            iterations = 1000;
        } else if (strncmp(argv[i], "--iterations=", 13) == 0) {
            iterations = (size_t)strtoul(argv[i] + 13, NULL, 10);
            if (iterations == 0) iterations = 1000;
        }
    }

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create mcp_context\n");
        return 1;
    }

    uint64_t *latencies = (uint64_t *)malloc(iterations * sizeof(uint64_t));
    if (latencies == NULL) {
        fprintf(stderr, "Failed to allocate latency buffer\n");
        mcp_context_destroy(ctx);
        return 1;
    }

    /* -------------------------------------------------------------
     * 1. Benchmark: Message Serialization Throughput
     * ------------------------------------------------------------- */
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *name = mcp_json_string_new(ctx, "calculate_sum");
    mcp_json_value_t *args = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, args, "a", mcp_json_number_new(ctx, 42.0));
    mcp_json_object_set_take(ctx, args, "b", mcp_json_number_new(ctx, 99.0));
    mcp_json_object_set_take(ctx, params, "name", name);
    mcp_json_object_set_take(ctx, params, "arguments", args);

    mcp_message_t *req = mcp_request_new_number_id(ctx, 1.0, "tools/call", params);
    if (req == NULL) {
        fprintf(stderr, "Failed to create request message\n");
        free(latencies);
        mcp_context_destroy(ctx);
        return 1;
    }

    /* Warm-up */
    for (size_t i = 0; i < 100; i++) {
        char *warm = mcp_message_serialize(ctx, req);
        mcp_json_free_string(ctx, warm);
    }

    uint64_t t_start = bench_now_ns();
    for (size_t i = 0; i < iterations; i++) {
        uint64_t i_start = bench_now_ns();
        char *str = mcp_message_serialize(ctx, req);
        uint64_t i_end = bench_now_ns();
        latencies[i] = i_end - i_start;
        mcp_json_free_string(ctx, str);
    }
    uint64_t t_end = bench_now_ns();
    double duration_sec = (double)(t_end - t_start) / 1000000000.0;
    size_t rss_kb = bench_get_rss_kb(0);

    bench_stats_t stats_ser;
    bench_calc_stats("json_rpc_message_serialize", latencies, iterations, duration_sec, rss_kb, &stats_ser);
    bench_print_stats(&stats_ser);

    /* -------------------------------------------------------------
     * 2. Benchmark: Message Parsing Throughput
     * ------------------------------------------------------------- */
    char *wire_str = mcp_message_serialize(ctx, req);
    size_t wire_len = strlen(wire_str);
    mcp_message_destroy(ctx, req);

    /* Warm-up */
    for (size_t i = 0; i < 100; i++) {
        mcp_message_t *warm_msg = mcp_message_parse(ctx, wire_str, wire_len);
        mcp_message_destroy(ctx, warm_msg);
    }

    t_start = bench_now_ns();
    for (size_t i = 0; i < iterations; i++) {
        uint64_t i_start = bench_now_ns();
        mcp_message_t *msg = mcp_message_parse(ctx, wire_str, wire_len);
        uint64_t i_end = bench_now_ns();
        latencies[i] = i_end - i_start;
        mcp_message_destroy(ctx, msg);
    }
    t_end = bench_now_ns();
    duration_sec = (double)(t_end - t_start) / 1000000000.0;
    rss_kb = bench_get_rss_kb(0);

    bench_stats_t stats_parse;
    bench_calc_stats("json_rpc_message_parse", latencies, iterations, duration_sec, rss_kb, &stats_parse);
    bench_print_stats(&stats_parse);

    /* -------------------------------------------------------------
     * 3. Benchmark: Direct Streaming Serializer
     * ------------------------------------------------------------- */
    mcp_arena_t *stream_arena = mcp_arena_create(NULL, 16384);
    mcp_json_stream_t stream;
    mcp_json_rpc_id_t rpc_id = mcp_json_rpc_id_from_int(1);
    const char *result_raw = "{\"sum\":141.0,\"status\":\"ok\"}";

    /* Warm-up */
    for (size_t i = 0; i < 100; i++) {
        mcp_json_stream_init_arena(&stream, stream_arena);
        mcp_serialize_response_stream(&stream, &rpc_id, result_raw);
        mcp_arena_reset(stream_arena);
    }

    t_start = bench_now_ns();
    for (size_t i = 0; i < iterations; i++) {
        uint64_t i_start = bench_now_ns();
        mcp_json_stream_init_arena(&stream, stream_arena);
        mcp_serialize_response_stream(&stream, &rpc_id, result_raw);
        uint64_t i_end = bench_now_ns();
        latencies[i] = i_end - i_start;
        mcp_arena_reset(stream_arena);
    }
    t_end = bench_now_ns();
    duration_sec = (double)(t_end - t_start) / 1000000000.0;
    rss_kb = bench_get_rss_kb(0);

    bench_stats_t stats_stream;
    bench_calc_stats("direct_streaming_serialize", latencies, iterations, duration_sec, rss_kb, &stats_stream);
    bench_print_stats(&stats_stream);

    /* -------------------------------------------------------------
     * 4. Benchmark: Arena Allocation & O(1) Bulk Reset
     * ------------------------------------------------------------- */
    mcp_arena_t *bench_arena = mcp_arena_create(NULL, 16384);

    /* Warm-up */
    for (size_t i = 0; i < 100; i++) {
        for (int k = 0; k < 10; k++) {
            void *p = mcp_arena_alloc(bench_arena, 64);
            (void)p;
        }
        mcp_arena_reset(bench_arena);
    }

    t_start = bench_now_ns();
    for (size_t i = 0; i < iterations; i++) {
        uint64_t i_start = bench_now_ns();
        for (int k = 0; k < 10; k++) {
            void *p = mcp_arena_alloc(bench_arena, 64);
            (void)p;
        }
        mcp_arena_reset(bench_arena);
        uint64_t i_end = bench_now_ns();
        latencies[i] = i_end - i_start;
    }
    t_end = bench_now_ns();
    duration_sec = (double)(t_end - t_start) / 1000000000.0;
    rss_kb = bench_get_rss_kb(0);

    bench_stats_t stats_arena;
    bench_calc_stats("arena_alloc_10x_and_reset", latencies, iterations, duration_sec, rss_kb, &stats_arena);
    bench_print_stats(&stats_arena);

    mcp_arena_destroy(stream_arena);
    mcp_arena_destroy(bench_arena);

    mcp_json_free_string(ctx, wire_str);
    free(latencies);
    mcp_context_destroy(ctx);
    return 0;
}
