#ifndef MCPKIT_BENCH_COMMON_H
#define MCPKIT_BENCH_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>

/**
 * @brief High-resolution monotonic timestamp in nanoseconds.
 */
static inline uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Comparator for sorting 64-bit latency values.
 */
static inline int bench_cmp_u64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

/**
 * @brief Samples process resident set size (RSS) in kilobytes.
 *
 * If pid <= 0, samples current process.
 */
static inline size_t bench_get_rss_kb(pid_t pid) {
    char path[64];
    if (pid <= 0) {
        snprintf(path, sizeof(path), "/proc/self/statm");
    } else {
        snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    }
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        unsigned long size = 0, resident = 0;
        if (fscanf(f, "%lu %lu", &size, &resident) == 2) {
            fclose(f);
            long page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0) page_size = 4096;
            return (size_t)((resident * (unsigned long)page_size) / 1024UL);
        }
        fclose(f);
    }
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return (size_t)ru.ru_maxrss;
    }
    return 0;
}

/**
 * @brief Aggregated benchmark statistics.
 */
typedef struct {
    const char *name;
    size_t count;
    double duration_sec;
    double qps;
    uint64_t min_ns;
    uint64_t p50_ns;
    uint64_t p90_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
    size_t rss_kb_peak;
} bench_stats_t;

/**
 * @brief Computes statistics (percentiles and QPS) from sorted latency array.
 */
static inline void bench_calc_stats(const char *name, uint64_t *latencies_ns, size_t count,
                                    double duration_sec, size_t rss_kb, bench_stats_t *out) {
    out->name = name;
    out->count = count;
    out->duration_sec = duration_sec;
    out->qps = (duration_sec > 0.0) ? ((double)count / duration_sec) : 0.0;
    out->rss_kb_peak = rss_kb;
    if (count == 0) {
        out->min_ns = out->p50_ns = out->p90_ns = out->p99_ns = out->max_ns = 0;
        return;
    }
    qsort(latencies_ns, count, sizeof(uint64_t), bench_cmp_u64);
    out->min_ns = latencies_ns[0];
    out->p50_ns = latencies_ns[(size_t)(count * 0.50)];
    out->p90_ns = latencies_ns[(size_t)(count * 0.90)];
    out->p99_ns = latencies_ns[(size_t)(count * 0.99)];
    out->max_ns = latencies_ns[count - 1];
}

/**
 * @brief Prints both human-readable statistics table and machine-parseable JSON line.
 */
static inline void bench_print_stats(const bench_stats_t *s) {
    printf("\n===================================================\n");
    printf("Benchmark: %s\n", s->name);
    printf("---------------------------------------------------\n");
    printf("Iterations: %zu\n", s->count);
    printf("Duration:   %.3f s\n", s->duration_sec);
    printf("Throughput: %.2f ops/sec (QPS)\n", s->qps);
    printf("Latency:\n");
    printf("  Min:      %.2f us\n", (double)s->min_ns / 1000.0);
    printf("  P50:      %.2f us\n", (double)s->p50_ns / 1000.0);
    printf("  P90:      %.2f us\n", (double)s->p90_ns / 1000.0);
    printf("  P99:      %.2f us\n", (double)s->p99_ns / 1000.0);
    printf("  Max:      %.2f us\n", (double)s->max_ns / 1000.0);
    printf("Resident RSS Peak: %zu KB\n", s->rss_kb_peak);
    printf("JSON: {\"benchmark\":\"%s\",\"total_requests\":%zu,\"duration_sec\":%.4f,\"qps\":%.2f,"
           "\"latency_us\":{\"min\":%.1f,\"p50\":%.1f,\"p90\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
           "\"rss_kb_peak\":%zu}\n",
           s->name, s->count, s->duration_sec, s->qps,
           (double)s->min_ns / 1000.0, (double)s->p50_ns / 1000.0,
           (double)s->p90_ns / 1000.0, (double)s->p99_ns / 1000.0,
           (double)s->max_ns / 1000.0, s->rss_kb_peak);
    printf("===================================================\n\n");
}

#endif /* MCPKIT_BENCH_COMMON_H */
