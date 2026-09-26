# 实验目标列表（每个对应 src/ 下一个编号实验）
set(MJL_EXPERIMENTS
  01_malloc_latency
  02_arena_contention
  03_priority_inversion
  04_false_sharing
  05_stl_hidden_alloc
  06_memory_pool_fix
)

# ------- 01：C 基线，直接测 malloc/free 尾延迟 -------
add_executable(01_malloc_latency 01_malloc_latency.c)
target_include_directories(01_malloc_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(01_malloc_latency PRIVATE Threads::Threads m)

# ------- 02：C++ 多线程 Arena 锁竞争 -------
add_executable(02_arena_contention 02_arena_contention.cpp)
target_include_directories(02_arena_contention PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(02_arena_contention PRIVATE Threads::Threads m)

# ------- 03：C++ 优先级反转复现（依赖 pthread 实时调度）-------
add_executable(03_priority_inversion 03_priority_inversion.cpp)
target_include_directories(03_priority_inversion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(03_priority_inversion PRIVATE Threads::Threads m)
# 需要 SCHED_FIFO 实时调度，建议以 root 或赋予 cap_ipc_lock/cap_sys_nice 运行
set_property(TARGET 03_priority_inversion PROPERTY ENABLE_EXPORTS TRUE)

# ------- 04：C++ 伪共享（cacheline 对齐 + atomic）-------
add_executable(04_false_sharing 04_false_sharing.cpp)
target_include_directories(04_false_sharing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(04_false_sharing PRIVATE Threads::Threads m)

# ------- 05：C++ STL/protobuf-like 隐式分配 -------
add_executable(05_stl_hidden_alloc 05_stl_hidden_alloc.cpp)
target_include_directories(05_stl_hidden_alloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(05_stl_hidden_alloc PRIVATE Threads::Threads m)

# ------- 06：内存池修复对照实验 -------
add_executable(06_memory_pool_fix 06_memory_pool_fix.cpp)
target_include_directories(06_memory_pool_fix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(06_memory_pool_fix PRIVATE Threads::Threads m)

# 安装规则
install(TARGETS ${MJL_EXPERIMENTS} RUNTIME DESTINATION bin)
