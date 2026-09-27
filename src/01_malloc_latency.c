/*
 * 01_malloc_latency.c —— 用纯 C 建立基线：直接测 malloc/free 的尾延迟
 *
 * 为什么用 C：
 *   排除 C++ 运行时干扰（异常表、RTTI、构造/析构），得到最干净的
 *   Glibc malloc 基线，方便与后续 C++ 实验对比，证明
 *   "C++ 的 new 只是 malloc 的包装，问题同源"。
 *
 * 知识点：
 * 1. malloc 平均耗时只有几十~几百 ns（从 fastbin/tcache 直接取块），
 *   但 hitting 慢路径时（top chunk 扩展、bin 合并、arena 扩容、
 *   mmap 新映射），单次可达数百 us ~ ms 级。平均值掩盖一切，
 *   P99/P999 才能暴露尖刺。
 * 2. free 并非"即时归还"：小 chunk 进 tcache/fastbin（快），
 *   大 chunk 触发 munmap 或 malloc_consolidate（慢），
 *   跨 M_TRIM_THRESHOLD 还会触发 madvise/brk 收缩归还 OS（最慢）。
 * 3. 块尺寸决定路径：request 落在 tcache 范围(<=1032B)走快速路径，
 *   > mmap_threshold(默认128KB) 走 mmap/munmap，
 *   中间尺寸轮流走 chunk 切分与合并，是尖刺高发区。
 *   注意：glibc 会"动态上调" mmap_threshold——若连续 free 的大块
 *   都来自 mmap，阈值最高抬到 32MB，之后同尺寸改走 brk 快路径，
 *   所以本实验 huge 档看起来反而最快，这是 glibc 的自适应行为，
 *   也是"第一次尖刺、后面不尖刺"这类现象的常见解释。
 * 4. 预热(warm-up)很重要：第一次 malloc 会触发 arena 初始化，
 *   这一两次的巨慢样本属于"冷启动"，应剔除以免污染统计。
 *
 * 编译运行：cmake --build build && ./build/src/01_malloc_latency
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "latency_stats.h"

#define WARMUP_ROUNDS   1000
#define SAMPLE_ROUNDS   100000
#define SMALL_SIZE   256          /* tcache 快路径          */
#define MID_SIZE     (64 * 1024)  /* 64KB, chunk 合并/切分  */
#define HUGE_SIZE    (4 * 1024 * 1024) /* > mmap_threshold */

static void bench_size(size_t size, const char *label)
{
    /* 大块走 mmap/munmap 系统调用，样本数降下来控制总时长 */
    size_t rounds = (size > 1024 * 1024) ? (SAMPLE_ROUNDS / 100) : SAMPLE_ROUNDS;
    latency_recorder_t alloc_lat, free_lat;
    mjl_recorder_init(&alloc_lat, rounds);
    mjl_recorder_init(&free_lat, rounds);

    /* 预热：触发 arena 初始化和 tcache 建立，剔除冷启动样本 */
    for (int i = 0; i < WARMUP_ROUNDS; i++) {
        void *p = malloc(size);
        free(p);
    }

    for (size_t i = 0; i < rounds; i++) {
        uint64_t t0 = mjl_now_ns();
        void *p = malloc(size);
        uint64_t t1 = mjl_now_ns();
        free(p);
        uint64_t t2 = mjl_now_ns();
        mjl_record(&alloc_lat, t1 - t0);
        mjl_record(&free_lat, t2 - t1);
    }

    printf("== %s (%zu bytes) ==\n", label, size);
    mjl_report("  malloc", &alloc_lat);
    mjl_report("  free  ", &free_lat);

    mjl_recorder_free(&alloc_lat);
    mjl_recorder_free(&free_lat);
}

/* 生命周期模拟:先分配一批"常驻"数据,再分配/释放"临时"数据,
 * 观察 free 归还 OS 时(brk 收缩)的慢路径是否传导给随后的 malloc */
static void bench_mixed_lifecycle(void)
{
    enum { RESIDENT = 50, TEMP = 200 };
    void *resident[RESIDENT];
    for (int i = 0; i < RESIDENT; i++)
        resident[i] = malloc(MID_SIZE);   /* 模拟常驻控制数据 */

    latency_recorder_t rec;
    mjl_recorder_init(&rec, SAMPLE_ROUNDS);

    for (int i = 0; i < SAMPLE_ROUNDS; i++) {
        uint64_t t0 = mjl_now_ns();
        void *p = malloc(MID_SIZE);
        free(p);
        uint64_t t1 = mjl_now_ns();
        mjl_record(&rec, t1 - t0);
    }
    /* 乱序释放：先奇数后偶数，让 resident 之间的空隙交错合并，
     * 触发 malloc_consolidate / top chunk 收缩慢路径 */
    for (int i = RESIDENT - 1; i >= 0; i -= 2) free(resident[i]);
    for (int i = RESIDENT - 2; i >= 0; i -= 2) free(resident[i]);

    printf("== mixed lifecycle (resident + temp) ==\n");
    mjl_report("  malloc+free", &rec);
    mjl_recorder_free(&rec);
}

/* 压力注入：高频交替分配/释放不同尺寸，制造锁竞争和碎片，
 * 让尖刺可控复现 */
static void stress_consolidate(int iters)
{
    latency_recorder_t rec;
    mjl_recorder_init(&rec, iters);

    for (int i = 0; i < iters; i++) {
        uint64_t t0 = mjl_now_ns();
        void *a = malloc(4096);
        void *b = malloc(65536);
        void *c = malloc(256);
        free(a);
        free(b);
        free(c);
        uint64_t t1 = mjl_now_ns();
        mjl_record(&rec, t1 - t0);
    }

    printf("== stress consolidate (%d iters) ==\n", iters);
    mjl_report("  malloc+free(x3)", &rec);
    mjl_recorder_free(&rec);
}

int main(void)
{
    printf("glibc malloc latency baseline\n");
    printf("watch P99/P999 vs mean — this is 'memory jitter'\n\n");
    bench_size(SMALL_SIZE, "small  (tcache fast path)");
    bench_size(MID_SIZE,   "mid    (chunk split/coalesce)");
    bench_size(HUGE_SIZE,  "huge   (> mmap_threshold)");
    bench_mixed_lifecycle();

    printf("\n== stress consolidate (forcing malloc_consolidate) ==\n");
    stress_consolidate(200);
    return 0;
}
