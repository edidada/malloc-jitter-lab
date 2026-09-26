#!/usr/bin/env python3
"""plot.py —— 读取实验 stdout，画 P50/P99/P999 延迟对比直方图

用法：
    ./scripts/run_all.sh | tee run.log
    python3 scripts/plot.py run.log

依赖：matplotlib（无则只打印解析结果）
"""
import re
import sys

LINE = re.compile(
    r"^\[(?P<label>.+?)\]\s+n=(?P<n>\d+)\s+mean=(?P<mean>[\d.]+)ns\s+"
    r"P50=(?P<p50>\d+)ns\s+P90=(?P<p90>\d+)ns\s+"
    r"P99=(?P<p99>\d+)ns\s+P999=(?P<p999>\d+)ns\s+MAX=(?P<max>\d+)ns"
)


def parse(path):
    rows = []
    with open(path) as f:
        for line in f:
            m = LINE.match(line.strip())
            if m:
                d = m.groupdict()
                d.update({k: int(v) if k != "mean" else float(v)
                          for k, v in d.items() if k not in ("label", "n")})
                d["n"] = int(d["n"])
                rows.append(d)
    return rows


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    rows = parse(sys.argv[1])
    if not rows:
        print("no latency rows found")
        sys.exit(1)

    for r in rows:
        print(f"{r['label']:>12}: P99={r['p99']}ns P999={r['p999']}ns MAX={r['max']}ns")

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed, skip plotting")
        return

    labels = [r["label"] for r in rows]
    fig, ax = plt.subplots(figsize=(10, 5))
    for i, key in enumerate(("p50", "p99", "p999")):
        ax.plot(labels, [r[key] for r in rows], marker="o", label=key.upper())
    ax.set_yscale("log")
    ax.set_ylabel("latency (ns, log scale)")
    ax.set_title("malloc jitter: tail latency per experiment")
    ax.legend()
    ax.tick_params(axis="x", rotation=30)
    fig.tight_layout()
    fig.savefig("latency_summary.png", dpi=150)
    print("saved latency_summary.png")


if __name__ == "__main__":
    main()
