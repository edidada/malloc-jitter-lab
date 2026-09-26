/*
 * 03_priority_inversion.cpp —— 优先级反转复现（核心实验）
 *
 * 场景还原：
 *   低优先级存储线程持锁 malloc/free（模拟 LiDAR mcap 写入），
 *   高优先级 MPC/LNN 控制线程等同一把锁。
 *   glibc malloc 内部锁（arena lock）不支持优先级继承，
 *   再叠加我们自己显式使用的 pthread_mutex（默认也无 PI），即发生反转。
 *
 * 知识点：
 * 1. 优先级反转三要素：低 H 持锁、中 M 抢占 L、H 等 L 的锁。
 *    本实验用第三个"中优先级干扰线程"把 H 的等待时间放大到 100ms 级。
 * 2. pthread_mutex 默认 PRIO_NONE（无优先级继承）→ H 会全速被阻塞。
 *    两种修复：
 *      a) pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)
 *         —— L 持锁期间临时继承 H 的优先级，尽快放锁；
 *      b) 无锁内存池（见实验 06）—— 根本不持Allocator锁。
 * 3. SCHED_FIFO 是抢占式实时策略，一旦 H 可运行就立即抢占 M/L，
 *   但 H 在等锁时无法表现优先级 —— 正是反转的成因。
 * 4. 需要 root 运行才能设置 SCHED_FIFO；无权限自动降级，
 *    反转幅度会缩小但趋势仍可观察。
 *
 * 运行：sudo ./build/src/03_priority_inversion
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <pthread.h>

#include "latency_stats.h"
#include "pin_cpu.h"

using namespace std::chrono;

static void busy_work(int dummy)
{
    (void)dummy;
    volatile double x = 0.0;
    for (int i = 0; i < 200000; ++i) x += i * 0.000001;
    (void)x;
}

static std::atomic<bool> g_stop{false};
static latency_recorder_t g_rec;
static pthread_mutex_t g_alloc_lock;

/* 锁属性：0 = 无优先级继承（默认，反转复现）；1 = PRIO_INHERIT（修复对照） */
static int g_use_pi = 0;

/* 低优先级：模拟存储线程，持锁做大块 malloc/free */
static void *storage_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 10);
    while (!g_stop.load(std::memory_order_relaxed)) {
        pthread_mutex_lock(&g_alloc_lock);
        void *p = malloc(64 * 1024);         /* 模拟 lz4 压缩缓冲 */
        memset(p, 0xCD, 64 * 1024);
        free(p);
        pthread_mutex_unlock(&g_alloc_lock);
        /* 稍放一手，避免把 CPU吃死导致实验完全不可测 */
        std::this_thread::sleep_for(microseconds(50));
    }
    return nullptr;
}

/* 中优先级干扰线程：纯 CPU 循环，专吃掉 L 被抢占后的时间片，
 * 制造"M 插队"的经典反转放大器 */
static void *interference_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 40);
    while (!g_stop.load(std::memory_order_relaxed))
        busy_work(1000);
    return nullptr;
}

/* 高优先级：1ms MPC 周期，必须 <5ms 完成 */
static void *control_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 80);
    const auto period = microseconds(1000);
    auto next = steady_clock::now();

    for (int i = 0; i < 5000; ++i) {
        next += period;
        auto t0 = steady_clock::now();
        pthread_mutex_lock(&g_alloc_lock);    /* 被低优线程阻塞 → 反转 */
        void *p = malloc(256);
        free(p);
        pthread_mutex_unlock(&g_alloc_lock);
        auto t1 = steady_clock::now();
        mjl_record(&g_rec, duration_cast<nanoseconds>(t1 - t0).count());
        std::this_thread::sleep_until(next);
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    g_use_pi = (argc > 1 && atoi(argv[1]) == 1);
    printf("priority inversion demo, PI=%s\n", g_use_pi ? "ON (fix)" : "OFF (bug)");

    if (g_use_pi) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&g_alloc_lock, &attr);
        pthread_mutexattr_destroy(&attr);
    } else {
        pthread_mutex_init(&g_alloc_lock, nullptr);   /* 默认无 PI */
    }

    mjl_recorder_init(&g_rec, 5000);

    pthread_t t_low, t_mid, t_high;
    pthread_create(&t_high, nullptr, control_thread, nullptr);
    pthread_create(&t_mid,  nullptr, interference_thread, nullptr);
    pthread_create(&t_low,  nullptr, storage_thread, nullptr);

    void *r;
    pthread_join(t_high, &r);
    g_stop = true;
    pthread_join(t_mid, &r);
    pthread_join(t_low, &r);

    printf("\n== control thread lock+malloc latency ==\n");
    mjl_report(g_use_pi ? " PI-ON " : " PI-OFF", &g_rec);
    mjl_recorder_free(&g_rec);
    pthread_mutex_destroy(&g_alloc_lock);

    printf("\nexpected: PI-OFF 时 P99/P999 明显高于 PI-ON\n"
           "复现项目现象：'IO 没满、算法没变、P99 却飙升'\n");
    return 0;
}
