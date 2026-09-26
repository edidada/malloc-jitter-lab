# CTest 集成：每个实验作为一条冒烟测试
#
# 知识点：
# - 性能类实验的结果受机器负载影响，这里只做"能跑通、无崩溃"的冒烟验证；
# - 严格的延迟断言应放到独占的 CI 机器上，用脚本解析 P99 输出与阈值比较；
# - 03 优先级反转无 root 时 SCHED_FIFO 设置失败但仍能降级运行，所以不设
#   sudo，保持测试在任何环境可通过。
set(EXP_NAMES
  01_malloc_latency
  02_arena_contention
  03_priority_inversion
  04_false_sharing
  05_stl_hidden_alloc
  06_memory_pool_fix
)

set(EXP_ARGS
  ""
  "8"
  "0"
  ""
  ""
  ""
)

set(idx 0)
foreach(name ${EXP_NAMES})
  list(GET EXP_ARGS ${idx} arg)
  if(arg STREQUAL "")
    add_test(NAME smoke_${name} COMMAND ${name})
  else()
    add_test(NAME smoke_${name} COMMAND ${name} ${arg})
  endif()
  # 60s 上限：防止样本量过大拖死 CI
  set_tests_properties(smoke_${name} PROPERTIES TIMEOUT 60)
  math(EXPR idx "${idx} + 1")
endforeach()
