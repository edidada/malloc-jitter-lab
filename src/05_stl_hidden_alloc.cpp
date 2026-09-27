/*
 * 05_stl_hidden_alloc.cpp —— STL / protobuf-like / mcap 隐式分配 会污染控制路径
 *
 * 背景：
 *   你的消费端用 PB + lz4 序列化压缩后写 mcap。这些库内部会做
 *   大量临时堆分配：std::string 拼接、std::vector 扩容、
 *   mcap::Writer 内部缓冲增长。若这些操作发生在控制线程
 *   （哪怕只是日志、telemetry、诊断包格式化），就走了 glibc
 *   malloc 的慢路径，抖动便传导到 MPC/LNN 控制周期。
 *
 * 知识点：
 * 1. C++ 的 new == operator new == malloc + 构造函数。
 *    问题 100% 同源于 glibc —— 换汤不换药。
 * 2. std::vector 扩容策略是 1.5x 或 2x 增长：
 *      - push_back 触发 realloc → 拷贝 + 旧块 free
 *      - 扩到 巨大时触发 brk/ mmap 慢路径
 *    reserve() 预分配能完全消除运行期扩容。
 * 3. std::string += 在短串走 SSO（Small String Optimization，
 *    栈上内联 ≤15B），超过就 heap alloc —— 拼日志最常见陷阱。
 * 4. protobuf SerializeToString / lz4 压缩输出缓冲 一般
 *    new 一块新内存，RapidJSON/flatbuffers 会用 arena。
 * 5. 计数器方法：用 malloc_hook / gperf/heap profiler (jemalloc
 *    的 MALLOC_CONF=prof:true) 统计每个线程的分配数，
 *    控制线程应为 0。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "latency_stats.h"
#include "pin_cpu.h"

constexpr int kWarmupMsg  = 1000;
constexpr int kMsgs       = 10000;
constexpr size_t kMsgSize = 8 * 1024;   /* 模拟一帧点云序列化结果 */

/* 模拟一次"序列化+压缩+写缓冲"：
 * 使用与 protobuf/lz4 类似的模式 —— 拼接串 & vector 扩容 */
static size_t fake_serialize(std::string &out, size_t payload)
{
    out.clear();
    for (size_t i = 0; i < 8; i++) {
        out += std::string(payload / 8, 'x');   /* 8 次拼接 → 8 次 realloc */
    }
    return out.size();
}

/* 版本2：预分配后拼接 —— 没有堆分配 */
static size_t fake_serialize_prealloc(std::string &out, size_t payload)
{
    if (out.capacity() < payload) out.reserve(payload);
    out.resize(payload);
    return out.size();
}

/* 控制路径：1ms 周期，做一次"无辜"的日志格式化 */
static void *control_loop(void *arg)
{
    auto *rec = static_cast<latency_recorder_t *>(arg);
    const auto period = std::chrono::microseconds(1000);
    auto next = std::chrono::steady_clock::now();

    for (int i = 0; i < kMsgs; ++i) {
        next += period;
        auto t0 = std::chrono::steady_clock::now();

        /* 就是这一行 —— 看似无害的日志，实际多次 heap alloc */
        std::string log = "cloud frame seq=" + std::to_string(i) +
                          " lidar_ts_ns=" + std::to_string(mjl_now_ns()) +
                          " latency_ms=" + std::to_string(i * 1.0 / kMsgs);
        (void)log;

        auto t1 = std::chrono::steady_clock::now();
        mjl_record(rec, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        std::this_thread::sleep_until(next);
    }
    return nullptr;
}

/* 存储路径：模拟 LiDAR 压缩写入，同时控制线程在跑 */
static void *storage_loop(void *)
{
    std::string buf;
    for (int i = 0; i < kMsgs + kWarmupMsg; ++i)
        fake_serialize(buf, kMsgSize);
    return nullptr;
}

int main()
{
    latency_recorder_t rec;
    mjl_recorder_init(&rec, kMsgs);

    /* 先证明单线程下日志拼接本身就会分配 */
    printf("== log string format cost (single thread) ==\n");
    for (int pass = 0; pass < 3; ++pass) {
        latency_recorder_t r;
        mjl_recorder_init(&r, kMsgs / 10);
        for (int i = 0; i < kMsgs / 10; ++i) {
            auto t0 = mjl_now_ns();
            std::string s = "frame seq=" + std::to_string(i) +
                            " ts=" + std::to_string(i * 1000L);
            auto t1 = mjl_now_ns();
            mjl_record(&r, t1 - t0);
            if (s.empty()) printf("impossible\n");
        }
        mjl_report("  log-format", &r);
        mjl_recorder_free(&r);
    }

    /* 修复对照：容量在进入控制段前预留，后续序列化不再扩容。 */
    printf("\n== preallocated serialization cost ==\n");
    std::string preallocated;
    preallocated.reserve(kMsgSize);
    latency_recorder_t prealloc_rec;
    mjl_recorder_init(&prealloc_rec, kMsgs / 10);
    for (int i = 0; i < kMsgs / 10; ++i) {
        const uint64_t t0 = mjl_now_ns();
        const size_t serialized = fake_serialize_prealloc(preallocated, kMsgSize);
        const uint64_t t1 = mjl_now_ns();
        mjl_record(&prealloc_rec, t1 - t0);
        if (serialized != kMsgSize) printf("unexpected serialized size\n");
    }
    mjl_report("  reserve+resize", &prealloc_rec);
    mjl_recorder_free(&prealloc_rec);

    /* 再证明：并发存储线程会让控制线程的日志路径 P99 变差 */
    printf("\n== concurrent storage thread pollutes control path ==\n");
    pthread_t tc, ts;
    pthread_create(&tc, nullptr, control_loop, &rec);
    pthread_create(&ts, nullptr, storage_loop, nullptr);
    void *r;
    pthread_join(tc, &r);
    pthread_join(ts, &r);
    mjl_report("  ctrl-log", &rec);
    mjl_recorder_free(&rec);

    printf("\n知识点：\n"
           " 1. std::string += / vector push_back 都可能触发 realloc。\n"
           " 2. 控制路径上任何 STL 操作都可能是隐形 malloc。\n"
           " 3. 修复：reserve() 预分配 + 固定容量 ring buffer，见实验 06。\n");
    return 0;
}
