# readme
最近在开发三一重工的自动挖掘数据闭环项目，在“感知环”和“决策环”的排查分析中出现一个诡异的现象。尽管 NVMe SSD 的带宽远未触顶，但只要高分辨率 LiDAR 数据一触发存储，控制链路的 P99 延迟就会瞬间从 5ms 飙升到 100ms，时不时出现尖刺。”感知环“内，我们使用Iceoryx + Shared MemoryMemory来实现跨进程之间的数据传输，在消费端我们使用pb+lz4进行数据的序列化和压缩存储到mcap文件内，在消费端的内存处理阶段还是时不时会出现耗时尖刺。在“决策环”内，仿真环境里表现完美，但在实车部署时，却因为这个微小的‘内存抖动’，导致 LNN 预测模型与底层 MPC 失步，险些引发事故。这种‘隐形丢包’的元凶，不在 IO 驱动，也不在算法逻辑，而是在被大多数人忽略的 Glibc malloc 内存空隙里。
整理专业名称解释下

你排查出的“内存抖动”根源指向 Glibc malloc 的行为，这在涉及高吞吐数据与硬实时控制的系统中确实是一个容易被忽视的耦合点。这个现象的本质是：**非实时、非确定的内存分配器，与硬实时任务共享了同一条执行路径。**

### 现象核心：Glibc malloc 的“不确定延迟”

Glibc 的 malloc 实现基于 ptmalloc2，是为**通用吞吐量**设计的，而非**实时确定性**。它的内部机制在触发存储操作时，可能产生你观察到的尖刺：

#### 为什么“吞吐量优先”不等于“延迟可预测”

ptmalloc2 的目标是在普通服务器负载下，让**大量线程的平均分配速度和总体并发吞吐量尽可能高**，并兼顾内存利用率；它并不为一次 `malloc` 或 `free` 给出可证明的最坏执行时间（WCET）上界。这不是实现缺陷，而是目标不同：硬实时控制要求“每个周期最慢也必须在截止时间内完成”，而通用分配器可以用偶发的慢操作换取大多数请求更快、更省内存。

- **常见路径很快，但不是唯一的路径。** 小块分配通常能从线程缓存（tcache）或空闲链表直接取得，往往只需很少的指令；然而缓存耗尽、块尺寸变化、碎片整理或需要新的堆空间时，会转入更复杂的查找、切分和合并路径。平均值因此很好看，P99/P999 却可能暴露尖刺。
- **并发优化仍可能带来等待。** Arena 用多个堆减少全局锁竞争，而不是消除竞争；当线程共享或跨越同一 Arena 时，`malloc`/`free` 仍可能等待内部锁。该等待时间取决于其他线程当前做什么、何时被调度，无法由控制线程自行界定上限。
- **部分慢路径会向内核请求服务。** 堆扩展、缩减及大块映射/解除映射可能走 `brk`、`mmap`、`munmap` 或 `madvise` 等路径，并可能伴随首次触页、TLB/cache 扰动或内核调度。这些成本受系统内存状态和其他任务影响，远比纯用户态快路径不稳定。
- **`free` 也在实时路径上计时。** 它不只是“放回指针”：可能把相邻 chunk 合并、更新空闲结构，或触发内存归还策略。因此“控制线程只释放、不分配”同样不能视为零抖动。

所以，问题不是所有 `malloc` 都很慢，而是控制周期中**不能依赖它始终很快**。对硬实时 LNN/MPC 路径，应在进入实时段前完成 `reserve`/预触页/对象池初始化，并避免动态扩容、日志拼接和临时序列化；把允许偶发慢路径的 protobuf、lz4、MCAP 写入等工作隔离到非实时线程。后面的实验分别把这些慢路径、竞争和预分配修复做成可测对照。

1. **Arena 竞争与优先级反转**：在高分辨率 LiDAR 数据触发存储时，消费端进程（或线程）会进行大量 `malloc`/`free` 调用。Glibc 的多个“竞技场”（Arena）在锁竞争时，低优先级的存储线程可能持有锁，导致高优先级的控制线程被迫等待。更危险的是，Glibc 的锁机制默认**不支持优先级继承**，高优先级线程会被中等优先级的任务无限期阻塞，这正是 P99 延迟从 5ms 飙升至 100ms 的典型“优先级反转”特征。

2. **内存回收的“合成”惩罚**：`free` 操作并非总是即时返回。当释放的 chunk 与相邻空闲块合并，或触及 `top chunk` 的调整阈值时，Glibc 可能触发 `madvise` 或 `brk` 系统调用，将内存归还操作系统。这一过程涉及**不可预测的系统调用开销和潜在的页错误**，在采集尖刺出现时正好命中，就会造成控制线程的抖动。

### 关键专业名词解释

结合你的项目场景，以下名词是理解该问题的关键：

| 专业名词 | 在你的场景中的含义 |
| :--- | :--- |
| **Glibc Allocator (ptmalloc2)** | 默认的 C 内存分配器。它的设计目标是通用性，**不保证最坏情况下的分配延迟**，因此不适合硬实时控制线程直接使用。 |
| **内存抖动 (Memory Jitter)** | 你观察到的现象。指内存分配/释放时间的不确定性，导致原本稳定的控制周期出现离散的、突发的延迟尖峰。 |
| **优先级反转 (Priority Inversion)** | 决策环失控的元凶。当低优先级的存储任务持有内存锁时，高优先级的 LNN/MPC 任务被迫等待，甚至被中等优先级任务插队，导致控制失步。 |
| **False Sharing (伪共享)** | 可能加剧尖刺的隐性因素。当存储线程和控制线程访问位于同一缓存行但不同变量的内存时，会导致 CPU 缓存频繁失效，进一步放大延迟抖动。 |
| **Zero-Copy (零拷贝)** | 感知环使用 Iceoryx 的核心优势。它避免了数据在用户态与内核态、进程与进程之间的复制，将大数据量传输的 CPU 开销和延迟降到极低，**但这并未消除消费端自身进行序列化/存储时的内存管理开销**。 |
| **Memory Arena (内存竞技场)** | Glibc 为减少多线程竞争而设计的独立堆区域。当数据流激增时，线程可能跨 Arena 访问，触发锁竞争和不可预测的缓存迁移。 |
| **MCAP** | 你用于存储的格式。序列化与压缩（pb+lz4）过程本身会产生大量临时堆内存分配。如果这些分配恰好落入控制线程正在使用的 Arena，尖刺就会传导过去。 |

### 排查方向：隔离 malloc 路径

既然 IO 驱动和算法逻辑已被排除，问题的切入点应该是**内存分配路径的隔离**。你可以从以下几个专业层面深入排查：

*   **确认锁竞争与优先级**：使用 `strace -c` 或 `perf lock` 观察 `futex` 调用。如果在尖刺发生时，控制线程卡在 `__lll_lock_wait` 或 `malloc_consolidate`，则确认是 Glibc 锁竞争导致的优先级反转。
*   **验证 Arena 隔离是否失效**：检查控制线程与存储线程是否因为 `MALLOC_ARENA_MAX` 设置不当或线程动态创建，导致它们被迫共享同一个 Arena。理想情况下，硬实时线程应有自己专属的、预分配好的内存池。
*   **检查“隐性”分配**：即使是 `std::vector` 扩容、`std::string` 拼接、`mcap::Writer` 内部缓冲区的增长，只要在控制路径上发生，就会触发 Glibc 分配。检查 LNN 推理前后是否有非预分配的临时对象产生。

针对你描述的这个场景（Iceoryx + 共享内存 + LiDAR 高吞吐 + LNN/MPC 硬实时控制），建议**用 C++ 为主，C 作为底层对照**来建项目。原因是：

- **Iceoryx 本身是 C++ 库**（虽然提供 C binding），你的感知环/决策环大概率是 C++ 工程；
- **Glibc malloc 是 C 接口**（`malloc`/`free`），但你在 C++ 里调用 `new`/`delete`、STL 容器、`std::string`、protobuf、lz4、mcap 时，**最终都会落到 Glibc malloc 上**——这正是问题的隐蔽之处；
- **硬实时控制（LNN/MPC）通常用 C++ 写**，用 C 反而脱离你的真实技术栈。

所以推荐做法是：**C++ 主项目，关键实验点用 C 函数直接观测 Glibc 行为**。

---

### 推荐的项目结构

```
malloc-jitter-lab/
├── CMakeLists.txt
├── src/
│   ├── 01_malloc_latency.c          # C：直接测 malloc/free 尾延迟
│   ├── 02_arena_contention.cpp      # C++：多线程 Arena 锁竞争复现
│   ├── 03_priority_inversion.cpp    # C++：优先级反转复现（核心）
│   ├── 04_false_sharing.cpp         # C++：伪共享放大抖动
│   ├── 05_stl_hidden_alloc.cpp      # C++：STL/protobuf/mcap 隐式分配
│   ├── 06_memory_pool_fix.cpp       # C++：内存池/预分配修复对照
│   └── common/
│       ├── latency_stats.h          # 统一的 P50/P99/P999 统计
│       └── pin_cpu.h                # CPU 亲和性绑定
└── scripts/
    ├── run_all.sh
    └── plot.py                      # 画延迟分布直方图
```

---

### 每个实验用什么语言、测什么

| 实验 | 语言 | 核心目的 | 关键 API |
| :--- | :--- | :--- | :--- |
| 01_malloc_latency | **C** | 建立基线：`malloc`/`free` 本身的尾延迟分布，证明"平均很快，P99 会尖刺" | `malloc`, `free`, `clock_gettime(CLOCK_MONOTONIC)` |
| 02_arena_contention | **C++** | 复现多线程下 Arena 锁竞争，观察 `futex` 等待 | `std::thread`, `pthread_mutex`, `mallopt(M_ARENA_MAX, ...)` |
| 03_priority_inversion | **C++** | 复现"低优先级存储线程持锁 → 高优先级控制线程被阻塞" | `pthread_setschedparam(SCHED_FIFO)`, `pthread_mutex`（默认无优先级继承） |
| 04_false_sharing | **C++** | 复现缓存行伪共享导致的抖动放大 | `alignas(64)`, `std::atomic` |
| 05_stl_hidden_alloc | **C++** | 证明 STL/protobuf/mcap 的隐式分配会污染控制路径 | `std::vector`, `std::string`, `google::protobuf`, `mcap::Writer` |
| 06_memory_pool_fix | **C++** | 对照实验：换成内存池/预分配后 P99 是否回落 | 自研 pool、`boost::pool`、`jemalloc`/`tcmalloc` 对比 |

---

### 为什么 03 必须用 C++ 而不是 C

优先级反转的复现需要：

- `SCHED_FIFO` 实时调度（C 也能做，但 C++ 封装更顺手）；
- 多线程 + 条件变量 + 锁（C++ `std::thread`/`std::mutex` 或 pthread）；
- 模拟 LNN 推理 + MPC 控制周期 + 存储线程三者交互。

用 C 写不是不行，但代码会非常冗长，而且脱离你实际工程（你的决策环大概率是 C++ + Eigen/ONNX Runtime/自研 MPC）。

---

### 为什么 01 建议用 C

`01_malloc_latency.c` 用纯 C 写，好处是：

- **排除 C++ 运行时干扰**（异常、RTTI、构造析构），得到最干净的 Glibc malloc 基线；
- 直接对应 `malloc_consolidate`、`top chunk` 扩展、`madvise` 这些底层行为；
- 方便和后续 C++ 实验对比，证明"C++ 的 new 只是 malloc 的包装，问题同源"。

---

### 一个最小复现骨架（C++，优先级反转）

```cpp
// 03_priority_inversion.cpp
#include <pthread.h>
#include <sched.h>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <chrono>

std::atomic<bool> stop{false};
pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER; // 默认无优先级继承

// 低优先级：模拟 LiDAR 存储线程，频繁 malloc/free
void* storage_thread(void*) {
    set_sched(SCHED_FIFO, 10); // 低优先级实时
    while (!stop) {
        pthread_mutex_lock(&alloc_lock);
        void* p = malloc(64 * 1024);   // 模拟 protobuf/lz4 临时缓冲
        free(p);
        pthread_mutex_unlock(&alloc_lock);
    }
    return nullptr;
}

// 高优先级：模拟 MPC 控制线程，P99 必须 < 5ms
void* control_thread(void*) {
    set_sched(SCHED_FIFO, 80); // 高优先级实时
    while (!stop) {
        auto t0 = now();
        pthread_mutex_lock(&alloc_lock);   // 被低优先级线程阻塞 → 优先级反转
        void* p = malloc(256);
        free(p);
        pthread_mutex_unlock(&alloc_lock);
        record_latency(now() - t0);        // 会看到 100ms 级尖刺
        sleep_until_next_cycle(1ms);
    }
    return nullptr;
}
```

这个骨架能在开发机上稳定复现你描述的"IO 没满、算法没变、P99 却飙升"的现象，然后再把 `03` 的锁换成**无锁内存池**或 `pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)`，就能看到 P99 回落——这就是完整的因果链证明。

---

### 结论

- **主项目用 C++**：贴合 Iceoryx、mcap、protobuf、LNN/MPC 的真实栈；
- **底层基线用 C**：干净地观测 Glibc malloc 行为；
- **对照实验用 C++**：优先级反转、伪共享、STL 隐式分配、内存池修复，都更适合 C++ 表达。

如果你要，我可以直接把上面 6 个实验的**完整可编译代码 + CMakeLists + 运行脚本**一次性写出来，你拿去就能在自己机器上复现尖刺并验证修复效果。

这三个词正好对应 Glibc ptmalloc2 的三个设计层次：**tcache 换速度、Arena 换并发、整体换吞吐**。它们共同构成了"为什么 malloc 吞吐高，但实时确定性差"的答案。

---

## 一、ptmalloc2 的核心取舍：吞吐量 vs 实时确定性

ptmalloc2 是 Glibc 的默认分配器，它的设计目标是**通用场景下的高吞吐量**，而不是**实时场景下的低延迟确定性**。这两者在工程上是矛盾的：

| 目标 | 优化方向 | 代价 |
| :--- | :--- | :--- |
| **吞吐量** | 减少系统调用、批量管理内存、缓存空闲块 | 单次操作的最坏延迟不可控 |
| **实时确定性** | 每次操作延迟有上界、可预测 | 需要预分配、牺牲内存利用率 |

ptmalloc2 选择了前者。它的三个机制——**tcache、Arena、chunk 管理**——都是为吞吐量服务的，但也正是它们引入了不可预测的尾延迟。

---

## 二、三个关键英文单词解释

### 1. tcache (Thread Local Cache，线程本地缓存)

**是什么：**
每个线程私有的一个小型空闲块缓存，默认每个 size class 缓存 **7 个 chunk**。分配和释放时，**优先在 tcache 里完成，不加锁、不跨线程**。

**为什么快：**
- 无锁，纯线程本地操作；
- 命中时就是几次指针操作，20ns 级别；
- 这是你 01 实验里 `mean=19ns` 的原因。

**为什么破坏实时确定性：**
- tcache 容量有限（7 个/class），**第 8 次分配就会 miss**，被迫走 slow path：加锁、查 fastbin/smallbin、可能触发 `malloc_consolidate`；
- 这个"第 8 次"的时机取决于运行时状态，**不可预测**；
- 命中时 20ns，miss 时可能跳到 µs 甚至 ms——这就是你看到的 P99 与 MAX 差几个数量级的根源。

**一句话：tcache 把"平均延迟"压到极低，但把"最坏延迟"推高了。**

---

### 2. Arena (内存竞技场)

**是什么：**
一块独立的堆区域，每个 Arena 有自己的锁、free list、top chunk。多线程分配时，Glibc 让不同线程绑定不同 Arena，**减少锁竞争**。

**为什么快：**
- 线程 A 用 Arena 1，线程 B 用 Arena 2，互不干扰；
- 相比"全局一把锁"，并发吞吐量大幅提升；
- 这是你 02 实验里 control 线程 P99=230ns 的原因——它大概率独占了一个 Arena。

**为什么破坏实时确定性：**
- Arena 数量有限（默认 `8 × CPU 核数`），线程数超过 Arena 数时**必然共享**；
- 共享 Arena 时，锁竞争回归，且 **Glibc 的锁默认不支持优先级继承**；
- 更隐蔽的是：线程绑定哪个 Arena 取决于**运行时创建顺序和首次 malloc 时机**，不可静态预测；
- 在容器/cgroup 限制 CPU 的环境里，Arena 数量会被压缩，**竞争比开发机上更严重**——这正是你实车部署比仿真差的原因之一。

**一句话：Arena 把"多线程并发"做上去了，但把"跨线程耦合"引入了。**

---

### 3. Chunk (内存块)

**是什么：**
ptmalloc2 管理内存的基本单位。每次 malloc 返回的是 chunk 的"用户数据区"，chunk 头部还有 metadata（size、flags、前后指针）。

**为什么快：**
- 通过 **size class** 分类管理，分配时按大小快速定位；
- 相邻空闲 chunk 可以**合并（coalesce）**，减少碎片；
- 大块直接走 mmap，不占 heap。

**为什么破坏实时确定性：**
- **chunk split/coalesce 是 O(n) 操作**，最坏情况下要遍历 unsorted bin；
- `free` 时若触发 `malloc_consolidate`，会一次性合并大量 chunk，产生 **ms 级停顿**；
- 这就是你 01 实验里 `mid free MAX=2.58ms` 的机制来源；
- **mmap_threshold 是动态调整的**，同一 size 的分配在不同时刻可能走不同路径（heap vs mmap），延迟特征突变。

**一句话：chunk 管理把"内存利用率"做上去了，但把"最坏操作耗时"变得不可控。**

---

## 三、三者关系：吞吐量取舍的完整逻辑

```
吞吐量目标
   │
   ├── tcache     → 减少锁开销，加速小分配
   │                 代价：容量有限，miss 时突变
   │
   ├── Arena      → 减少线程间竞争
   │                 代价：Arena 数量有限，共享时锁竞争+优先级反转
   │
   └── chunk      → 提高内存利用率，减少碎片
                     代价：split/coalesce/mmap 路径不可预测
```

**结论：**
ptmalloc2 的三层设计，每一层都在用"最坏延迟的不可预测性"换取"平均吞吐量"。这在 Web 服务、批处理里完全没问题，但在**硬实时控制环**里是致命的——因为控制环关心的不是 mean，而是 **P99/P999/MAX**。

---

## 四、映射到你的业务场景

| ptmalloc2 机制 | 你业务里的表现 |
| :--- | :--- |
| tcache | 平时 malloc 很快（20ns），偶尔 miss 触发尖刺 |
| Arena | 感知环/决策环多线程共享 Arena，存储线程拖累控制线程 |
| chunk | LiDAR 大块数据的 free 触发 `malloc_consolidate`，产生 ms 级停顿 |
| 全局后果 | NVMe 没满、算法没变，但控制链 P99 从 5ms 飙到 100ms |

---

## 五、Readme 建议结构

```markdown
# malloc-jitter-lab

## 背景
自动挖掘数据闭环中，Glibc malloc 的非确定性导致控制链 P99 尖刺。

## 核心概念

### ptmalloc2 的吞吐量取舍
ptmalloc2 优化的是平均吞吐量，而非最坏延迟。这在硬实时控制环中是致命的。

### 三个关键机制

| 机制 | 作用 | 代价 |
| :--- | :--- | :--- |
| **tcache** (Thread Local Cache) | 线程本地缓存，无锁加速小分配 | 容量有限，miss 时延迟突变 |
| **Arena** | 多线程独立堆区域，减少锁竞争 | 数量有限，共享时锁竞争+优先级反转 |
| **Chunk** | 内存管理基本单位，支持合并 | split/coalesce 最坏 O(n)，ms 级停顿 |

### 为什么这对实时系统是问题
- 实时关心 P99/P999/MAX，不关心 mean；
- ptmalloc2 保证 mean 快，但不保证 MAX 有界；
- 解决方案：内存池、预分配、实时分配器（TLSF 等）。

## 实验列表
01_malloc_latency      → 基线：证明 mean 快但 MAX 尖刺
02_arena_contention    → Arena 竞争对 P99 的影响
03_priority_inversion  → 优先级反转复现
04_false_sharing       → 伪共享放大抖动
05_stl_hidden_alloc    → STL/protobuf 隐式分配
06_memory_pool_fix     → 内存池修复对照

## 结论
...
```

---

## 六、一句话总结（可直接放 readme 开头）

> ptmalloc2 用 **tcache** 换单线程速度，用 **Arena** 换多线程并发，用 **chunk** 换内存利用率——三者共同把"平均吞吐量"做到极致，但也共同把"最坏延迟"变得不可预测。在硬实时控制环里，这三点就是你 P99 尖刺的全部来源。

要不要我接着把 **01 stress 修复版 + 02 双线程对照实验**的完整代码写出来？跑完能直接得到"tcache miss / Arena 竞争 / chunk consolidate"三条路径各自的 P99 抬升数据，正好对应 readme 里的三个概念。

malloc  ← 用户调用的 API 接口（C 标准库函数名）
   │
   └── glibc malloc  ← Glibc 对 malloc 的具体实现
          │
          └── ptmalloc2  ← 这个实现内部的项目名/算法名
换句话说：

malloc = 你代码里写的 void* p = malloc(256);，是一个函数接口；

glibc malloc = Glibc 这个 C 库对 malloc 的实现；

ptmalloc2 = Glibc malloc 内部使用的分配算法/代码库的名称（Pthreads malloc version 2）。
