#ifndef MJL_LATENCY_STATS_H
#define MJL_LATENCY_STATS_H

/*
 * latency_stats.h —— 统一的延迟统计工具
 *
 * 知识点：
 * 1. 测量延迟必须用 CLOCK_MONOTONIC（单调时钟），不要用 CLOCK_REALTIME，
 *    后者会被 NTP 校准/跳变污染，测出的尖刺可能是时间跳变而非真实抖动。
 * 2. P99 / P999（尾延迟）比平均值更有意义：malloc 平均耗时几十 ns，
 *    但一旦触发 brk / madvise / mmap / malloc_consolidate，单次可能达到
 *    数百 us 甚至 ms 级——平均值完全掩盖了这些尖刺。
 * 3. 先把所有样本存下来再排序取分位数，不要在线计算：
 *    尖刺是稀疏事件，流式平均无法捕捉。
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t *samples;   /* 纳秒 */
    size_t count;
    size_t capacity;
} latency_recorder_t;

static inline uint64_t mjl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void mjl_recorder_init(latency_recorder_t *r, size_t capacity)
{
    r->samples = (uint64_t *)malloc(capacity * sizeof(uint64_t));
    r->count = 0;
    r->capacity = capacity;
}

static inline void mjl_record(latency_recorder_t *r, uint64_t ns)
{
    if (r->count < r->capacity)
        r->samples[r->count++] = ns;
}

static inline int mjl_u64_cmp(const void *a, const void *b)
{
    uint64_t ua = *(const uint64_t *)a, ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

/* 分位数：p 为 0~1，如 0.99 表示 P99 */
static inline uint64_t mjl_percentile(latency_recorder_t *r, double p)
{
    if (r->count == 0) return 0;
    qsort(r->samples, r->count, sizeof(uint64_t), mjl_u64_cmp);
    size_t idx = (size_t)(p * (double)(r->count - 1) + 0.5);
    return r->samples[idx];
}

static inline uint64_t mjl_max(latency_recorder_t *r)
{
    uint64_t mx = 0;
    for (size_t i = 0; i < r->count; ++i)
        if (r->samples[i] > mx) mx = r->samples[i];
    return mx;
}

static inline double mjl_mean(latency_recorder_t *r)
{
    if (r->count == 0) return 0.0;
    uint64_t sum = 0;
    for (size_t i = 0; i < r->count; ++i) sum += r->samples[i];
    return (double)sum / (double)r->count;
}

static inline void mjl_report(const char *label, latency_recorder_t *r)
{
    uint64_t p50 = mjl_percentile(r, 0.50);
    uint64_t p90 = mjl_percentile(r, 0.90);
    uint64_t p99 = mjl_percentile(r, 0.99);
    uint64_t p999 = mjl_percentile(r, 0.999);
    printf("[%s] n=%zu  mean=%.1fns  P50=%luns  P90=%luns  P99=%luns  P999=%luns  MAX=%luns\n",
           label, r->count, mjl_mean(r),
           (unsigned long)p50, (unsigned long)p90,
           (unsigned long)p99, (unsigned long)p999, (unsigned long)mjl_max(r));
}

static inline void mjl_recorder_free(latency_recorder_t *r)
{
    free(r->samples);
    r->samples = NULL;
    r->count = r->capacity = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* MJL_LATENCY_STATS_H */
