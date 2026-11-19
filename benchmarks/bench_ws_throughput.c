#include "bench_common.h"
#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/ws.h"

#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_WS_THREADS 64

typedef struct {
    size_t thread_id;
    size_t target_frames;
    uint64_t *latencies;
    size_t count;
} ws_stream_worker_arg_t;

static void *ws_stream_worker_fn(void *arg) {
    ws_stream_worker_arg_t *w = (ws_stream_worker_arg_t *)arg;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) return NULL;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        mcp_context_destroy(ctx);
        return NULL;
    }

    mcp_transport_t *srv_t = mcp_ws_transport_create(ctx, sv[0], true);
    mcp_transport_t *cli_t = mcp_ws_transport_create(ctx, sv[1], false);
    if (srv_t == NULL || cli_t == NULL) {
        if (srv_t) mcp_transport_destroy(ctx, srv_t);
        if (cli_t) mcp_transport_destroy(ctx, cli_t);
        close(sv[0]);
        close(sv[1]);
        mcp_context_destroy(ctx);
        return NULL;
    }

    (void)mcp_transport_start(ctx, srv_t);
    (void)mcp_transport_start(ctx, cli_t);

    const char *ping_msg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";
    size_t ping_len = strlen(ping_msg);
    const char *pong_msg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
    size_t pong_len = strlen(pong_msg);

    for (size_t i = 0; i < w->target_frames; i++) {
        uint64_t t0 = bench_now_ns();
        /* Client sends frame (masked) */
        if (mcp_transport_send(ctx, cli_t, ping_msg, ping_len) != MCP_OK) break;
        /* Server receives */
        char *recv_line = NULL;
        if (mcp_transport_recv(ctx, srv_t, &recv_line) != MCP_OK) break;
        mcp_json_free_string(ctx, recv_line);

        /* Server replies (unmasked) */
        if (mcp_transport_send(ctx, srv_t, pong_msg, pong_len) != MCP_OK) break;
        /* Client receives */
        recv_line = NULL;
        if (mcp_transport_recv(ctx, cli_t, &recv_line) != MCP_OK) break;
        mcp_json_free_string(ctx, recv_line);

        uint64_t t1 = bench_now_ns();
        w->latencies[w->count++] = (t1 - t0);
    }

    mcp_transport_stop(ctx, srv_t);
    mcp_transport_stop(ctx, cli_t);
    mcp_transport_destroy(ctx, srv_t);
    mcp_transport_destroy(ctx, cli_t);
    mcp_context_destroy(ctx);
    return NULL;
}

int main(int argc, char **argv) {
    size_t concurrency = 8;
    size_t total_frames = 50000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            concurrency = 2;
            total_frames = 1000;
        } else if (strncmp(argv[i], "--concurrency=", 14) == 0) {
            concurrency = (size_t)strtoul(argv[i] + 14, NULL, 10);
            if (concurrency == 0) concurrency = 1;
            if (concurrency > MAX_WS_THREADS) concurrency = MAX_WS_THREADS;
        } else if (strncmp(argv[i], "--frames=", 9) == 0) {
            total_frames = (size_t)strtoul(argv[i] + 9, NULL, 10);
            if (total_frames == 0) total_frames = 1000;
        }
    }

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) return 1;

    /* -------------------------------------------------------------
     * 1. In-Memory Frame Codec Microbenchmark
     * ------------------------------------------------------------- */
    const char *payload_sample = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}";
    size_t payload_sample_len = strlen(payload_sample);

    mcp_ws_frame_t frame_in = {
        .fin = true,
        .opcode = MCP_WS_OP_TEXT,
        .masked = true,
        .mask_key = { 0x12, 0x34, 0x56, 0x78 },
        .payload_len = payload_sample_len,
        .payload = (char *)payload_sample
    };

    uint8_t *encoded = NULL;
    size_t encoded_len = 0;
    if (mcp_ws_encode_frame(ctx, &frame_in, &encoded, &encoded_len) != MCP_OK || encoded == NULL) {
        fprintf(stderr, "Failed to encode initial ws frame\n");
        mcp_context_destroy(ctx);
        return 1;
    }

    size_t codec_iterations = total_frames;
    uint64_t *codec_latencies = (uint64_t *)malloc(codec_iterations * sizeof(uint64_t));
    if (codec_latencies == NULL) {
        mcp_json_free_string(ctx, (char *)encoded);
        mcp_context_destroy(ctx);
        return 1;
    }

    uint64_t c_start = bench_now_ns();
    for (size_t i = 0; i < codec_iterations; i++) {
        uint64_t t0 = bench_now_ns();
        mcp_ws_frame_t parsed;
        size_t consumed = 0;
        mcp_status_t st = mcp_ws_parse_frame(ctx, encoded, encoded_len, &parsed, &consumed);
        uint64_t t1 = bench_now_ns();
        codec_latencies[i] = (t1 - t0);
        if (st == MCP_OK) {
            mcp_ws_frame_destroy(ctx, &parsed);
        }
    }
    uint64_t c_end = bench_now_ns();
    double codec_dur = (double)(c_end - c_start) / 1000000000.0;
    size_t rss_kb = bench_get_rss_kb(0);

    bench_stats_t codec_stats;
    bench_calc_stats("ws_frame_parse_codec", codec_latencies, codec_iterations, codec_dur, rss_kb, &codec_stats);
    bench_print_stats(&codec_stats);
    free(codec_latencies);
    mcp_json_free_string(ctx, (char *)encoded);

    /* -------------------------------------------------------------
     * 2. Concurrent Full WebSocket Transport Streaming
     * ------------------------------------------------------------- */
    ws_stream_worker_arg_t *args = (ws_stream_worker_arg_t *)calloc(concurrency, sizeof(ws_stream_worker_arg_t));
    pthread_t *tids = (pthread_t *)calloc(concurrency, sizeof(pthread_t));

    size_t frames_per_thread = total_frames / concurrency;
    for (size_t i = 0; i < concurrency; i++) {
        args[i].thread_id = i;
        args[i].target_frames = frames_per_thread;
        args[i].latencies = (uint64_t *)malloc(frames_per_thread * sizeof(uint64_t));
        args[i].count = 0;
    }

    uint64_t stream_start = bench_now_ns();
    for (size_t i = 0; i < concurrency; i++) {
        pthread_create(&tids[i], NULL, ws_stream_worker_fn, &args[i]);
    }

    for (size_t i = 0; i < concurrency; i++) {
        pthread_join(tids[i], NULL);
    }
    uint64_t stream_end = bench_now_ns();
    double stream_dur = (double)(stream_end - stream_start) / 1000000000.0;

    size_t total_stream_samples = 0;
    for (size_t i = 0; i < concurrency; i++) {
        total_stream_samples += args[i].count;
    }

    uint64_t *all_stream_latencies = (uint64_t *)malloc((total_stream_samples > 0 ? total_stream_samples : 1) * sizeof(uint64_t));
    size_t offset = 0;
    for (size_t i = 0; i < concurrency; i++) {
        if (args[i].count > 0 && args[i].latencies != NULL) {
            memcpy(all_stream_latencies + offset, args[i].latencies, args[i].count * sizeof(uint64_t));
            offset += args[i].count;
        }
        free(args[i].latencies);
    }

    rss_kb = bench_get_rss_kb(0);

    bench_stats_t stream_stats;
    bench_calc_stats("ws_transport_roundtrip_streaming", all_stream_latencies, total_stream_samples, stream_dur, rss_kb, &stream_stats);
    bench_print_stats(&stream_stats);

    free(all_stream_latencies);
    free(args);
    free(tids);
    mcp_context_destroy(ctx);
    return 0;
}
