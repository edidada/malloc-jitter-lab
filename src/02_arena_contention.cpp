/*
 * 02_arena_contention.cpp —— 多线程下 glibc arena 锁竞争复现
 *
 * 背景：glibc ptmalloc2 为每个线程维护独立的 arena（默认上限
 * 8 * CPU 核数），每个 arena 有独立锁。但当线程数 > arena 数、
 * 或用 M_ARENA_MAX 限制后，多个线程被迫共享同一个 arena，
 * malloc 变成串行化 —— 高吞吐存储线程与控制线程互相排队。
 *
 * 知识点：
 * 1. mallopt(M_ARENA_MAX, N) 限制 arena 数量 → 复现"共享 arena"。
 *    也可用环境变量 MALLOC_ARENA_MAX=N（对整进程生效，更常用）。
 * 2. futex 等待：锁竞争时线程会陷在 __lll_lock_wait 里被内核挂起，
 *    唤醒延迟 ~1-10us，若被_O(其他负载)抢占则可达 ms 级。
 *    用 `strace -c -f ./02_arena_contention` 观察 futex 调用次数。
 * 3. 伪共享加剧：各线程本想访问独立 arena，若 arena 结构紧邻同一
 *    cache line，锁的乒乓效应会进一步放大抖动。
 * 4. perf lock / perf sched 也能看到锁持有时间：
 *    perf record -e 'lock:lock_acquire' ./02_arena_contention
 *
 * 观察：
 *   - ARENA_MAX=1（全部共享栅栏）时 P99 显著劣化；
 *   - 默认（多 arena）时几乎无竞争。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <pthread.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#include "latency_stats.h"
#include "pin_cpu.h"

constexpr int kThreads      = 4;
constexpr int kCPUBase      = 2;    /* 绑核 从 CPU2 开始 */
constexpr size_t kBlockSize = 64 * 1024;  /* 模拟 protobuf/lz4 压缩缓冲 */
constexpr int kIterations   = 20000;

static std::atomic<bool> g_stop{false};
static latency_recorder_t g_recs[kThreads];

/* 压力线程：模拟 LiDAR 存储线程，疯狂 malloc/free */
static void *storage_thread(void *arg)
{
    long id = (long)arg;
    mjl_pin_to_cpu(pthread_self(), kCPUBase + (int)id);

    /* 与控制线程抢同一个 arena：限制 arena 后必然共享 */
    while (!g_stop.load(std::memory_order_relaxed)) {
        void *p = malloc(kBlockSize);
        /* touch pages，避免缺页中断主导测量结果 */
        memset(p, 0xAB, kBlockSize);
        free(p);
    }
    return nullptr;
}

/* 控制线程：1ms 周期，模拟 MPC/LNN，测每次 malloc 的延迟 */
static void *control_thread(void *)
{
    mjl_pin_to_cpu(pthread_self(), kCPUBase - 1);
    auto &rec = g_recs[0];

    const auto period = std::chrono::microseconds(1000);
    auto next = std::chrono::steady_clock::now();

    for (int i = 0; i < kIterations; ++i) {
        next += period;
        uint64_t t0 = mjl_now_ns();
        void *p = malloc(256);   /* 控制路径上的小分配 */
        uint64_t t1 = mjl_now_ns();
        free(p);
        mjl_record(&rec, t1 - t0);
        std::this_thread::sleep_until(next);
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    long arena_max = 8;  /* 默认 8*CORES */
    if (argc > 1) arena_max = atol(argv[1]);

    printf("M_ARENA_MAX = %ld\n", arena_max);
    if (mallopt(M_ARENA_MAX, (int)arena_max) == 0)
        printf(" (mallopt M_ARENA_MAX failed, using env)\n");

    /* 控制线程的统计槽位 */
    mjl_recorder_init(&g_recs[0], kIterations);

    pthread_t ctrl;
    pthread_create(&ctrl, nullptr, control_thread, nullptr);

    pthread_t storages[kThreads];
    for (long i = 0; i < kThreads; ++i)
        pthread_create(&storages[i], nullptr, storage_thread, (void *)i);

    /* 主线程当控制线程跑完 */
    void *ret;
    pthread_join(ctrl, &ret);
    g_stop.store(true);
    for (int i = 0; i < kThreads; ++i)
        pthread_join(storages[i], &ret);

    printf("\n== control thread malloc latency (small 256B) ==\n");
    mjl_report(" control", &g_recs[0]);

    for (int i = 0; i < kThreads; ++i)
        mjl_recorder_free(&g_recs[i]);
    return 0;
}
