/*
 * 03_priority_inversion.cpp —— 优先级反转复现（核心实验）
 *
 * 场景还原：
 *   三个角色共享一把 pthread_mutex，模拟 LiDAR 存储 + MPC 控制 + LNN 推理。
 *   storage 低优先级持锁做 malloc/free，lnn 中优先级抢占，
 *   control 高优先级等锁 → 优先级反转。
 *
 * 角色映射：
 *   storage  (SCHED_FIFO 10)：LiDAR 存储，malloc(64KB)+free，频繁
 *   lnn      (SCHED_FIFO 60)：LNN 推理，每 10ms 一次，malloc(1MB)+free
 *   control  (SCHED_FIFO 80)：MPC 控制，每 1ms 一个周期，malloc(256)+free
 *
 * 预期现象：
 *   无锁竞争时 control 的 P99 在 µs 级；
 *   共享 pthread_mutex（默认无优先级继承）时 control 的 P99 跳到 ms 级，
 *   且和 storage 的 free 尖刺时间戳对齐；
 *   换成 PTHREAD_PRIO_INHERIT 后 P99 回落。
 *
 * 运行：sudo ./build/src/03_priority_inversion [1]
 *   参数 1 = 启用 PTHREAD_PRIO_INHERIT 修复对照
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

static std::atomic<bool> g_stop{false};
static latency_recorder_t g_rec;
static pthread_mutex_t g_alloc_lock;

/* 锁属性：0 = 无优先级继承（默认，反转复现）；1 = PRIO_INHERIT（修复对照） */
static int g_use_pi = 0;

/* storage：低优先级，模拟 LiDAR mcap 写入，频繁 malloc(64KB)+free */
static void *storage_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 10);
    while (!g_stop.load(std::memory_order_relaxed)) {
        pthread_mutex_lock(&g_alloc_lock);
        void *p = malloc(64 * 1024);
        memset(p, 0xCD, 64 * 1024);
        free(p);
        pthread_mutex_unlock(&g_alloc_lock);
    }
    return nullptr;
}

/* lnn：中优先级，每 10ms 一次，malloc(1MB)+free */
static void *lnn_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 60);
    auto period = microseconds(10000);
    auto next = steady_clock::now();

    while (!g_stop.load(std::memory_order_relaxed)) {
        next += period;
        auto t0 = steady_clock::now();
        pthread_mutex_lock(&g_alloc_lock);
        void *p = malloc(1 << 20);
        memset(p, 0xAB, 1 << 20);
        free(p);
        pthread_mutex_unlock(&g_alloc_lock);
        auto t1 = steady_clock::now();
        mjl_record(&g_rec, duration_cast<nanoseconds>(t1 - t0).count());
        std::this_thread::sleep_until(next);
    }
    return nullptr;
}

/* control：高优先级，每 1ms 一个周期，malloc(256)+free */
static void *control_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 80);
    const auto period = microseconds(1000);
    auto next = steady_clock::now();

    for (int i = 0; i < 5000; ++i) {
        next += period;
        auto t0 = steady_clock::now();
        pthread_mutex_lock(&g_alloc_lock);
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
    printf("roles: storage(SCHED_FIFO 10) | lnn(SCHED_FIFO 60) | control(SCHED_FIFO 80)\n");

    if (g_use_pi) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&g_alloc_lock, &attr);
        pthread_mutexattr_destroy(&attr);
    } else {
        pthread_mutex_init(&g_alloc_lock, nullptr);
    }

    mjl_recorder_init(&g_rec, 5000);

    pthread_t t_storage, t_lnn, t_control;
    pthread_create(&t_control, nullptr, control_thread, nullptr);
    pthread_create(&t_lnn,  nullptr, lnn_thread, nullptr);
    pthread_create(&t_storage, nullptr, storage_thread, nullptr);

    void *r;
    pthread_join(t_control, &r);
    g_stop = true;
    pthread_join(t_lnn, &r);
    pthread_join(t_storage, &r);

    printf("\n== control thread lock+malloc latency ==\n");
    mjl_report(g_use_pi ? " PI-ON " : " PI-OFF", &g_rec);
    mjl_recorder_free(&g_rec);
    pthread_mutex_destroy(&g_alloc_lock);

    printf("\nexpected: PI-OFF 时 P99/P999 明显高于 PI-ON\n"
           "复现项目现象：'IO 没满、算法没变、P99 却飙升'\n");
    return 0;
}
