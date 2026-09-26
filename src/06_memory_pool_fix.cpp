/*
 * 06_memory_pool_fix.cpp —— 修复对照：内存池 / 预分配 后 P99 是否回落
 *
 * 对照实验逻辑：
 *   左手：run_raw —— 裸 malloc/free 的多线程竞争（与实验 02/03 同款现象）
 *   右手：run_pool —— 用无锁固定块内存池，同一负载下测 P99
 *   期望：P99 / P999 显著回落，这就是"因果链"的最后一环。
 *
 * 知识点：
 * 1. 自研 pool 的最简形态：freelist（把空闲块的 next 指针存在
 *    块自己头部），alloc = O(1) pop，free = O(1) push。
 *    完全无 malloc，也完全无 arena 锁。
 * 2. 也常用 mallopt(M_MMAP_THRESHOLD, 32MB) 抑制 madvise，
 *    或 MALLOC_TRIM_THRESHOLD=-1 禁止 brk 回收 —— 属于"止血"
 *    而非"根治"，适合来不及换 allocator 的过渡阶段。
 * 3. 换 allocator 三件套：
 *      LD_PRELOAD=libjemalloc.so.2  ./app
 *      LD_PRELOAD=libtcmalloc.so    ./app
 *      （详见 scripts/run_all.sh 中的对比注释）
 * 4. 硬实时里池要"预分配 + 独占 arena"（额外 mmap 一大块，
 *    不走 glibc），池内部用无锁 stack 或 per-thread cache。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <vector>

#include "latency_stats.h"
#include "pin_cpu.h"

using Clock = std::chrono::steady_clock;

constexpr int    kThreads     = 4;
constexpr size_t kBlockSize   = 64 * 1024;
constexpr int    kItersPerThd = 20000;

/* ---------------- 方案 A：每线程内存池（thread-cache 模式） ----------------
 *
 * 教训（本实验曾真实踩坑）：
 *   第一版用全局 freelist + CAS（Treiber stack），在多线程下存在经典
 *   ABA 竞态——T1 读 head 后被抢占，T2 完成整轮 alloc/memset/dealloc
 *   使 head"看起来没变"，T1 的 CAS 误判成功，同一块被分配给两个线程，
 *   memset 0xEF 填充被当成 next 指针解引用 → SIGSEGV（gdb core 已证实）。
 *
 *   正确且更实时的做法是 tcmalloc 的 thread-cache 模式：
 *   每个线程独享一个池，零共享、零 CAS、零 ABA，天然无锁。
 */
struct FreeNode { FreeNode *next; };

class SimplePool {
public:
    explicit SimplePool(size_t block, size_t count)
        : block_size_(block), count_(count)
    {
        pool_ = std::malloc(block * count);    /* 一次性 mmap/brk */
        head_ = static_cast<FreeNode *>(pool_);
        /* 串成 freelist：每块头部存 next 指针 */
        for (size_t i = 0; i < count - 1; ++i)
            node_at(i)->next = node_at(i + 1);
        node_at(count - 1)->next = nullptr;
    }
    ~SimplePool() { std::free(pool_); }
    SimplePool(const SimplePool &) = delete;
    SimplePool &operator=(const SimplePool &) = delete;

    /* 单线程使用，无需原子操作 —— O(1) pop/push */
    void *alloc()
    {
        FreeNode *h = head_;
        if (!h) return nullptr;   /* 池耗尽 —— 实时系统应报警 */
        head_ = h->next;
        return h;
    }

    void dealloc(void *p)
    {
        FreeNode *n = static_cast<FreeNode *>(p);
        n->next = head_;
        head_ = n;
    }

private:
    FreeNode *node_at(size_t i)
    {
        return reinterpret_cast<FreeNode *>(
            static_cast<char *>(pool_) + i * block_size_);
    }
    size_t block_size_, count_;
    void *pool_ = nullptr;
    FreeNode *head_ = nullptr;
};

/* 共享负载：worker 循环 malloc/free，控制线程测 256B 小分配延迟 */
static std::atomic<bool> g_stop{false};

static void *worker_raw(void *)
{
    while (!g_stop.load(std::memory_order_relaxed)) {
        void *p = std::malloc(kBlockSize);
        memset(p, 0xEF, kBlockSize);
        std::free(p);
    }
    return nullptr;
}

static SimplePool *g_pools[kThreads];

static void *worker_pool(void *arg)
{
    long id = (long)arg;
    SimplePool *pool = g_pools[id];   /* 每线程独享，消除共享 */
    while (!g_stop.load(std::memory_order_relaxed)) {
        void *p = pool->alloc();
        memset(p, 0xEF, kBlockSize);
        pool->dealloc(p);
    }
    return nullptr;
}

static void *control_loop(void *arg)
{
    auto *rec = static_cast<latency_recorder_t *>(arg);
    const auto period = std::chrono::microseconds(1000);
    auto next = Clock::now();
    for (int i = 0; i < kItersPerThd / 10; ++i) {
        next += period;
        auto t0 = Clock::now();
        void *p = std::malloc(256);
        std::free(p);
        auto t1 = Clock::now();
        mjl_record(rec, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        std::this_thread::sleep_until(next);
    }
    return nullptr;
}

/* ---------------- Driver ---------------- */

static void run_phase(const char *label, void *(*worker)(void *))
{
    latency_recorder_t ctrl_rec;
    mjl_recorder_init(&ctrl_rec, kItersPerThd / 10);

    pthread_t ctrl;
    pthread_create(&ctrl, nullptr, control_loop, &ctrl_rec);

    pthread_t w[kThreads];
    for (int i = 0; i < kThreads; ++i)
        pthread_create(&w[i], nullptr, worker, (void *)(intptr_t)i);

    void *r;
    pthread_join(ctrl, &r);
    g_stop = true;
    for (int i = 0; i < kThreads; ++i) pthread_join(w[i], &r);

    printf("\n-- phase: %s --\n", label);
    mjl_report(" control", &ctrl_rec);
    mjl_recorder_free(&ctrl_rec);
    g_stop = false;   /* 复位，供下一 phase 复用 */
}

int main()
{
    printf("== memory pool vs glibc malloc ==\n");

    /* 每线程一个池，预分配好（thread-cache 模式） */
    for (int i = 0; i < kThreads; ++i)
        g_pools[i] = new SimplePool(kBlockSize, 4);

    run_phase("raw glibc malloc", worker_raw);
    run_phase("thread-cache pool", worker_pool);

    for (int i = 0; i < kThreads; ++i) delete g_pools[i];
    printf("\n知识点结论：\n"
           " 1. 每线程独享池（thread-cache）完全绕过 glibc arena 锁，无 CAS、无 ABA。\n"
           " 2. 全局 CAS freelist 有 Treiber-stack ABA 竞态，本实验真实踩坑：\n"
           "    同一块被双重分配，memset 填充 0xEF 被当作 next 指针 → SIGSEGV。\n"
           " 3. 生产方案：tcmalloc/jemalloc 的 thread cache，或 boost::pool。\n"
           " 4. 别忘了：先 reserve/预分配，再进入实时控制段。\n");
    return 0;
}
