/*
 * 04_false_sharing.cpp —— 缓存行伪共享放大抖动复现
 *
 * 背景：
 *   多线程各自写自己的变量，但这些变量落在同一 64B cache line 里，
 *   CPU 用 MESI 缓存一致性协议把整条 line 在核间来回 invalidate /
 *   reload，写一次 ~10ns 的操作被放大到 ~100ns，在热点路径上
 *   直接转化为延迟抖动。
 *
 * 项目关联：
 *   你的存储线程记录 "已写入字节数" 这样的计数器，与控制线程
 *   访问的周期计数器等恰好在一个 cache line 里 —— IO 没满，
 *   延迟却涨了 10%+，这类"莫名 20%抖动"多半有伪共享贡献。
 *
 * 知识点：
 * 1. x86_64 cache line = 64B（Apple Silicon = 128B，跨平台注意）。
 * 2. alignas(64) 强制对齐，把热点计数器隔离到独立 line；
 *    或用 padding：char pad[64 - sizeof(what)]。
 * 3. perf c2c（cache-to-cache）可以直接找出伪共享行：
 *      perf c2c record ./04_false_sharing && perf c2c report
 * 4. std::atomic 本身不解决伪共享 —— atomic 只保证原子性，
 *    缓存行乒乓依旧；必须配合对齐。
 */
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include <chrono>
#include "latency_stats.h"
#include "pin_cpu.h"

constexpr int kThreads = 4;
constexpr int kIters   = 10'000'000;   /* 每线程自增次数 */

/* ---- 错误示范：4 个计数器挤在同一 cache line ---- */
struct BadCounters {
    std::atomic<uint64_t> c0{0}, c1{0}, c2{0}, c3{0};
};

/* ---- 正确示范：alignas(64) 强制每个计数器独占一条 line ---- */
struct alignas(64) PaddedCounter {
    std::atomic<uint64_t> v{0};
};
static_assert(sizeof(PaddedCounter) == 64, "must occupy a full cache line");

static void spin_worker(std::atomic<uint64_t> *ctr, int iters)
{
    for (int i = 0; i < iters; ++i)
        ctr->fetch_add(1, std::memory_order_relaxed);
}

/* 测量辅助：跑一轮耗时微秒 */
static double run_and_time(std::vector<std::atomic<uint64_t> *> ctrs)
{
    using clk = std::chrono::steady_clock;
    std::vector<std::thread> ts;
    auto t0 = clk::now();
    for (size_t i = 0; i < ctrs.size(); ++i)
        ts.emplace_back(spin_worker, ctrs[i], kIters);
    for (auto &t : ts) t.join();
    auto t1 = clk::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

int main()
{
    printf("cache line size check: sizeof(std::atomic<uint64_t>)=%zu, "
           "assuming 64B line on x86_64\n\n", sizeof(std::atomic<uint64_t>));

    /* --- 伪共享场景 --- */
    BadCounters bad;
    std::vector<std::atomic<uint64_t> *> bad_ptrs{
        &bad.c0, &bad.c1, &bad.c2, &bad.c3 };
    double bad_us = run_and_time(bad_ptrs);

    /* --- 隔离场景 --- */
    static PaddedCounter padded[kThreads];
    std::vector<std::atomic<uint64_t> *> pad_ptrs;
    for (int i = 0; i < kThreads; ++i) pad_ptrs.push_back(&padded[i].v);
    double pad_us = run_and_time(pad_ptrs);

    printf("== false sharing benchmark: %d threads x %d increments ==\n",
           kThreads, kIters);
    printf("  shared-line (bad)  : %8.1f us\n", bad_us);
    printf("  padded        (good): %8.1f us\n", pad_us);
    printf("  speedup: %.2fx\n", bad_us / pad_us);

    printf("\n知识点：alignas(64) 把热点数据隔离到独立 cache line，\n"
           "彻底消除 MESI 乒乓；perf c2c 可在你的项目里直接定位伪共享行。\n");
    return 0;
}
