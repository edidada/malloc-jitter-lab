# 实验说明：测试方法与知识点

对应 `readme.md` 的 6 个实验，编译后位于 `build/src/`，冒烟测试用 `ctest --test-dir build` 跑，一键完整跑 `sudo ./scripts/run_all.sh`。

## 通用测试方法（所有实验共用）

| 要点 | 做法 | 为什么 |
| :--- | :--- | :--- |
| 单调时钟 | `clock_gettime(CLOCK_MONOTONIC)` | `REALTIME` 会被 NTP 跳变污染，测出假尖刺 |
| 尾延迟分位数 | 全量采样 → 排序 → P50/P99/P999 | 尖刺是稀疏事件，平均值/流式统计会掩盖 |
| CPU 绑核 | `pthread_setaffinity_np` | 线程迁移自身引入数十 us 抖动，污染结果 |
| 预热 | 先跑 1000 次丢弃 | 首次分配触发 arena 初始化属冷启动噪声 |
| 触页 | 分配后 `memset` | 首次访问缺页中断远慢于 malloc 本身 |
| 实时调度 | `SCHED_FIFO`，需 root | 无 root 自动降级，现象减弱但仍可见 |

## 各实验知识点

### 01_malloc_latency（C 基线）
- malloc 平均 ~20ns，但 MAX 可达几十 us —— "平均很快、P99 尖刺"是常态。
- 路径由块尺寸决定：tcache(≤1032B) 快；>128KB 走 mmap/munmap；中间尺寸走切分/合并。
- **动态 mmap 阈值**：glibc 连续 free 大块时会自动把阈值抬到 32MB，之后同尺寸改走 brk —— 这解释了"第一次尖刺、后面不尖刺"。
- free 触发 `malloc_consolidate` / `M_TRIM_THRESHOLD` 收缩（madvise/brk）是慢路径大头。

### 02_arena_contention（Arena 锁竞争）
- `mallopt(M_ARENA_MAX, 1)` 或环境变量 `MALLOC_ARENA_MAX=1` 强制共享 arena → P99 劣化。
- 锁竞争线程陷在 `__lll_lock_wait`（futex），用 `strace -c -f` 数 futex 调用。

### 03_priority_inversion（优先级反转，核心）
- 复现三要素：低优持锁 + 中优抢占吃 CPU + 高优等锁。
- `pthread_mutex` 默认 `PRIO_NONE`；修复 = `PTHREAD_PRIO_INHERIT`（对照参数 `1`）或换内存池（实验 06）。
- glibc 的 arena 锁本身不支持优先级继承 —— 换 allocator 才是根治。
- 运行：`sudo ./03_priority_inversion 0`（bug）vs `1`（修复）。

### 04_false_sharing（伪共享）
- 4 个 `atomic<uint64_t>` 挤一条 cache line：本机实测 **6x+ 减速**。
- `alignas(64)` 隔离热点计数器；定位用 `perf c2c`。
- Apple Silicon 是 128B line，跨平台注意。

### 05_stl_hidden_alloc（STL 隐式分配）
- `std::string` 拼接（日志！）、`std::vector` 扩容都是隐形 malloc，控制路径上必须零分配。
- 实测：单线程日志格式化 ~50ns，加一个并发存储线程后同一操作 P99 ≈ 10us、MAX ≈ 127us。
- 修复：`reserve()` 预分配 / 固定容量 ring buffer。

### 06_memory_pool_fix（修复对照）
- 每线程独享池（tcmalloc thread-cache 模式）：无共享 → 无锁 → 无抖动传导。
- **真实踩坑记录**：全局 CAS freelist（Treiber stack）存在 ABA 竞态，同一块被双重分配，`memset` 填充被当 `next` 解引用 → SIGSEGV（gdb core 证实）。教训：lockfree 链表要 ABA 计数/hazard pointer，实时场景首选 thread-cache。
- 过渡方案（止血）：`mallopt(M_MMAP_THRESHOLD, 32MB)`、`MALLOC_TRIM_THRESHOLD` 禁收缩。
- 替换 allocator：`LD_PRELOAD=libjemalloc.so.2` / `libtcmalloc.so` 对比见 `scripts/run_all.sh`。

## 测试策略
- 本地冒烟：`ctest --test-dir build`（6 条，各 60s 超时）。
- 延迟断言（P99 < 阈值）只在独占 CI 机器上做，脚本解析输出分位数与阈值比较；共享机器上跑会因邻居噪声假失败。
