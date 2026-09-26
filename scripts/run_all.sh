#!/usr/bin/env bash
# run_all.sh —— 一键跑完 6 个实验，可选输出 CSV 供 plot.py 画图
#
# 用法：
#   ./scripts/run_all.sh            # 默认参数
#   sudo ./scripts/run_all.sh       # 建议 root：03 需要 SCHED_FIFO
#
# 对比第三方分配器（可选，需已安装）：
#   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./scripts/run_all.sh
#   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so    ./scripts/run_all.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/src"

echo "== [1/6] 01_malloc_latency: glibc malloc/free 基线 =="
"$BIN/01_malloc_latency"

echo
echo "== [2/6] 02_arena_contention: arena 锁竞争（默认 vs M_ARENA_MAX=1）=="
"$BIN/02_arena_contention" 8
"$BIN/02_arena_contention" 1

echo
echo "== [3/6] 03_priority_inversion: 优先级反转（PI-OFF vs PI-ON）=="
"$BIN/03_priority_inversion" 0 || true
"$BIN/03_priority_inversion" 1 || true

echo
echo "== [4/6] 04_false_sharing: 伪共享 vs alignas(64) 隔离 =="
"$BIN/04_false_sharing"

echo
echo "== [5/6] 05_stl_hidden_alloc: STL 隐式分配污染控制路径 =="
"$BIN/05_stl_hidden_alloc"

echo
echo "== [6/6] 06_memory_pool_fix: 内存池修复对照 =="
"$BIN/06_memory_pool_fix"

echo
echo "全部实验完成。画图：python3 scripts/plot.py <实验输出的分位数数据>"
