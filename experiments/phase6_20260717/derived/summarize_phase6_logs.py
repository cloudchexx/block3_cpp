#!/usr/bin/env python3
"""Summarize Phase 6 micro-tiled v2 A/B run_test logs.

Run from this directory or repository root:
  python experiments/phase6_20260717/derived/summarize_phase6_logs.py
"""
from __future__ import annotations

import csv
import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOG_GROUPS = {
    "test18_legacy": ROOT / "logs" / "test18_legacy",
    "test18_micro8": ROOT / "logs" / "test18_micro8",
    "test50_legacy": ROOT / "logs" / "test50_legacy",
    "test50_micro8": ROOT / "logs" / "test50_micro8",
}

BENCH_RE = re.compile(
    r"BENCHMARK_RESULT cache=(\w+) mode=(\w+) axis=(\w+) "
    r"total_sec=([0-9.]+) read_sec=([0-9.]+) write_sec=([0-9.]+) "
    r"avg_ms=([0-9.]+).*?plan_hash=([0-9a-f]+)"
)
FMT_RE = re.compile(
    r"B3D_FORMAT dataset=(\S+) file=(\S+) version=(\d+) layout=([\w-]+) "
    r"micro_size=(\d+) block_size=(\d+)"
)
DATASET_RE = re.compile(r"Dataset: (\w+)")
TOTAL_RE = re.compile(r"Total elapsed:\s*([0-9.]+)s")
SUITE_HASH_RE = re.compile(r"PLAN_HASH_SUITE value=([0-9a-f]+)")


def median(values):
    return statistics.median(values) if values else ""


def parse_log(path: Path):
    text = path.read_text(encoding="utf-8", errors="replace")
    dataset = DATASET_RE.search(text)
    total = TOTAL_RE.search(text)
    suite_hash = SUITE_HASH_RE.search(text)
    fmt = FMT_RE.search(text)
    records = []
    for match in BENCH_RE.finditer(text):
        cache, mode, axis, total_sec, read_sec, write_sec, avg_ms, plan_hash = match.groups()
        records.append({
            "file": path.name,
            "cache": cache,
            "mode": mode,
            "axis": axis,
            "total_sec": float(total_sec),
            "read_sec": float(read_sec),
            "write_sec": float(write_sec),
            "avg_ms": float(avg_ms),
            "plan_hash": plan_hash,
        })
    return {
        "file": path.name,
        "dataset": dataset.group(1) if dataset else "",
        "total_elapsed_sec": float(total.group(1)) if total else "",
        "suite_hash": suite_hash.group(1) if suite_hash else "",
        "format": fmt.groups() if fmt else None,
        "records": records,
        "workload_total_sec": sum(record["total_sec"] for record in records),
    }


def write_csv(path: Path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parsed = []
    bench_rows = []
    run_rows = []
    for group, directory in LOG_GROUPS.items():
        for path in sorted(directory.glob("*.log")):
            info = parse_log(path)
            parsed.append((group, info))
            layout = "micro8" if group.endswith("micro8") else "legacy"
            for record in info["records"]:
                bench_rows.append({
                    "dataset": info["dataset"],
                    "layout": layout,
                    "group": group,
                    **record,
                })
            fmt = info["format"]
            run_rows.append({
                "dataset": info["dataset"],
                "layout": layout,
                "group": group,
                "file": info["file"],
                "total_elapsed_sec": info["total_elapsed_sec"],
                "workload_total_sec": info["workload_total_sec"],
                "suite_hash": info["suite_hash"],
                "format_version": fmt[2] if fmt else "1",
                "format_layout": fmt[3] if fmt else "legacy",
                "micro_size": fmt[4] if fmt else "0",
                "block_size": fmt[5] if fmt else "",
            })

    write_csv(ROOT / "derived" / "benchmark_results.csv", bench_rows,
              ["dataset", "layout", "group", "file", "cache", "mode", "axis",
               "total_sec", "read_sec", "write_sec", "avg_ms", "plan_hash"])
    write_csv(ROOT / "derived" / "run_totals.csv", run_rows,
              ["dataset", "layout", "group", "file", "total_elapsed_sec", "workload_total_sec",
               "suite_hash", "format_version", "format_layout", "micro_size", "block_size"])

    med_rows = []
    keys = sorted({(r["dataset"], r["layout"], r["group"], r["cache"], r["mode"], r["axis"])
                   for r in bench_rows})
    for dataset, layout, group, cache, mode, axis in keys:
        subset = [r for r in bench_rows if (r["dataset"], r["layout"], r["group"], r["cache"], r["mode"], r["axis"]) ==
                  (dataset, layout, group, cache, mode, axis)]
        med_rows.append({
            "dataset": dataset,
            "layout": layout,
            "group": group,
            "cache": cache,
            "mode": mode,
            "axis": axis,
            "runs": len(subset),
            "total_median_sec": median([r["total_sec"] for r in subset]),
            "read_median_sec": median([r["read_sec"] for r in subset]),
            "write_median_sec": median([r["write_sec"] for r in subset]),
            "avg_median_ms": median([r["avg_ms"] for r in subset]),
            "plan_hashes": ";".join(sorted({r["plan_hash"] for r in subset})),
        })
    write_csv(ROOT / "derived" / "case_medians.csv", med_rows,
              ["dataset", "layout", "group", "cache", "mode", "axis", "runs",
               "total_median_sec", "read_median_sec", "write_median_sec", "avg_median_ms", "plan_hashes"])

    summary_rows = []
    for group in sorted({r["group"] for r in run_rows}):
        subset = [r for r in run_rows if r["group"] == group]
        summary_rows.append({
            "dataset": subset[0]["dataset"],
            "layout": subset[0]["layout"],
            "group": group,
            "runs": len(subset),
            "total_elapsed_median_sec": median([r["total_elapsed_sec"] for r in subset]),
            "workload_total_median_sec": median([r["workload_total_sec"] for r in subset]),
            "workload_total_mean_sec": statistics.mean([r["workload_total_sec"] for r in subset]) if subset else "",
        })
    write_csv(ROOT / "derived" / "run_summary.csv", summary_rows,
              ["dataset", "layout", "group", "runs", "total_elapsed_median_sec",
               "workload_total_median_sec", "workload_total_mean_sec"])


if __name__ == "__main__":
    main()
