/*
 * 02_arena_contention.cpp —— 多线程下 glibc arena 锁竞争复现（A/B/C 三组对比）
 *
 * 实验设计：
 *   A 组：只有 control 线程（基线，无竞争）
 *   B 组：control + storage 线程，M_ARENA_MAX=8（多 arena，分散竞争）
 *   C 组：control + storage 线程，M_ARENA_MAX=1（强制共享 arena，竞争最激烈）
 *
 * 角色映射：
 *   storage（SCHED_FIFO 10）：模拟 LiDAR 存储，高频 malloc(64KB)+free
 *   control（SCHED_FIFO 80）：MPC 控制，1ms 周期，malloc(256)+free
 *
 * 预期现象：
 *   A 组 P99 在 µs 级；B 组略升；C 组 P99 显著抬升（可能 ms 级）。
 *   若 C 组 P99 跳到 ms 级，说明 Arena 竞争是业务尖刺的根因之一。
 *
 * 运行：sudo ./build/src/02_arena_contention
 *   无需参数，三组自动顺序运行。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <pthread.h>
#include <thread>

#include "latency_stats.h"
#include "pin_cpu.h"

using namespace std::chrono;

constexpr int kIterations = 20000;
constexpr size_t kBlockSize = 64 * 1024;

static std::atomic<bool> g_stop{false};
static latency_recorder_t g_rec_a, g_rec_b, g_rec_c;

/* storage 线程：高频 malloc(64KB)+free，模拟 LiDAR 存储 */
static void *storage_thread(void *)
{
    mjl_set_sched(pthread_self(), SCHED_FIFO, 10);
    while (!g_stop.load(std::memory_order_relaxed)) {
        void *p = malloc(kBlockSize);
        memset(p, 0xAB, kBlockSize);
        free(p);
    }
    return nullptr;
}

/* control 线程：1ms 周期，测每次 malloc(256) 的延迟 */
static void *control_thread(void *arg)
{
    int group = (int)(long)arg;
    auto &rec = (group == 1) ? g_rec_a : (group == 2) ? g_rec_b : g_rec_c;
    mjl_set_sched(pthread_self(), SCHED_FIFO, 80);

    const auto period = microseconds(1000);
    auto next = steady_clock::now();

    for (int i = 0; i < kIterations; ++i) {
        next += period;
        uint64_t t0 = mjl_now_ns();
        void *p = malloc(256);
        uint64_t t1 = mjl_now_ns();
        free(p);
        mjl_record(&rec, t1 - t0);
        std::this_thread::sleep_until(next);
    }
    return nullptr;
}

static void run_group(int group_id, long arena_max)
{
    g_stop.store(false);
    mjl_recorder_init(&g_rec_a, kIterations);
    mjl_recorder_init(&g_rec_b, kIterations);
    mjl_recorder_init(&g_rec_c, kIterations);

    if (mallopt(M_ARENA_MAX, (int)arena_max) == 0)
        fprintf(stderr, "[group %d] mallopt M_ARENA_MAX=%ld failed\n", group_id, arena_max);

    const char *label = (group_id == 1) ? "A: control only" :
                        (group_id == 2) ? "B: control+storage, ARENA_MAX=8" :
                        "C: control+storage, ARENA_MAX=1";
    printf("=== %s ===\n", label);
    fflush(stdout);

    pthread_t ctrl;
    pthread_create(&ctrl, nullptr, control_thread, (void *)(long)group_id);

    pthread_t storages[2];
    if (group_id >= 2) {
        for (int i = 0; i < 2; i++)
            pthread_create(&storages[i], nullptr, storage_thread, nullptr);
    }

    void *r;
    pthread_join(ctrl, &r);
    g_stop.store(true);
    if (group_id >= 2) {
        for (int i = 0; i < 2; i++)
            pthread_join(storages[i], &r);
    }

    latency_recorder_t *rec = (group_id == 1) ? &g_rec_a :
                              (group_id == 2) ? &g_rec_b : &g_rec_c;
    const char *rec_label = (group_id == 1) ? "control (A)" :
                            (group_id == 2) ? "control (B)" : "control (C)";
    printf("[%s] n=%zu\n", rec_label, rec->count);
    mjl_report(rec_label, rec);
    printf("\n");
    fflush(stdout);

    mjl_recorder_free(&g_rec_a);
    mjl_recorder_free(&g_rec_b);
    mjl_recorder_free(&g_rec_c);
}

int main(void)
{
    printf("arena contention: A/B/C comparison\n");
    printf("A = control only | B = +storage, M_ARENA_MAX=8 | C = +storage, M_ARENA_MAX=1\n");
    printf("run as root for SCHED_FIFO effect\n\n");

    run_group(1, 8);
    run_group(2, 8);
    run_group(3, 1);

    printf("=== summary ===\n");
    printf("If C >> B >> A: arena contention is the root cause of P99 spikes\n");
    return 0;
}
