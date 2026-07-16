#!/usr/bin/env python3
"""Summarize archived Phase 5 benchmark logs for report use."""
from __future__ import annotations

import csv
import re
import statistics
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT / "logs"
OUT = ROOT / "derived"

RESULT_RE = re.compile(
    r"^BENCHMARK_RESULT cache=(\S+) mode=(\S+) axis=(\S+) "
    r"total_sec=([0-9.]+) read_sec=([0-9.]+) write_sec=([0-9.]+).*?"
    r"count=(\d+) bytes=(\d+).*?plan_hash=(\S+)",
    re.MULTILINE,
)
PIPELINE_RE = re.compile(
    r"^PIPELINE_RESULT .*?drain_sec=([0-9.]+) producer_wait_sec=([0-9.]+) "
    r"queue_peak_slices=(\d+) queue_peak_bytes=(\d+) payload_peak_bound_bytes=(\d+) "
    r"memory_mb=(\d+) window_slices=(\d+) queue_slices=(\d+) max_live_slices=(\d+)",
    re.MULTILINE,
)


def group_name(path: Path) -> str:
    rel = path.relative_to(LOGS)
    return rel.parts[0]


def dataset_name(path: Path) -> str:
    return "test50" if "test50" in path.name else "test18"


def median(values):
    return statistics.median(values) if values else None


def main() -> None:
    rows = []
    pipeline_rows = []
    file_totals = []

    for path in sorted(LOGS.rglob("*.log")):
        text = path.read_text(errors="ignore")
        group = group_name(path)
        dataset = dataset_name(path)
        case_total = 0.0
        case_count = 0
        for match in RESULT_RE.finditer(text):
            cache, mode, axis = match.group(1), match.group(2), match.group(3)
            total_sec = float(match.group(4))
            read_sec = float(match.group(5))
            write_sec = float(match.group(6))
            count = int(match.group(7))
            byte_count = int(match.group(8))
            plan_hash = match.group(9)
            rows.append({
                "dataset": dataset,
                "group": group,
                "file": str(path.relative_to(ROOT)),
                "cache": cache,
                "mode": mode,
                "axis": axis,
                "total_sec": total_sec,
                "read_sec": read_sec,
                "write_sec": write_sec,
                "count": count,
                "bytes": byte_count,
                "plan_hash": plan_hash,
            })
            case_total += total_sec
            case_count += 1
        file_totals.append({
            "dataset": dataset,
            "group": group,
            "file": str(path.relative_to(ROOT)),
            "cases": case_count,
            "total_sec": case_total,
        })
        for match in PIPELINE_RE.finditer(text):
            pipeline_rows.append({
                "dataset": dataset,
                "group": group,
                "file": str(path.relative_to(ROOT)),
                "drain_sec": float(match.group(1)),
                "producer_wait_sec": float(match.group(2)),
                "queue_peak_slices": int(match.group(3)),
                "queue_peak_bytes": int(match.group(4)),
                "payload_peak_bound_bytes": int(match.group(5)),
                "memory_mb": int(match.group(6)),
                "window_slices": int(match.group(7)),
                "queue_slices": int(match.group(8)),
                "max_live_slices": int(match.group(9)),
            })

    with (OUT / "benchmark_results.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    with (OUT / "run_totals.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(file_totals[0].keys()))
        writer.writeheader()
        writer.writerows(file_totals)

    if pipeline_rows:
        with (OUT / "pipeline_diagnostics.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(pipeline_rows[0].keys()))
            writer.writeheader()
            writer.writerows(pipeline_rows)

    by_case = defaultdict(list)
    for row in rows:
        key = (row["dataset"], row["group"], row["cache"], row["mode"], row["axis"])
        by_case[key].append(row)

    summary_rows = []
    for key, values in sorted(by_case.items()):
        dataset, group, cache, mode, axis = key
        summary_rows.append({
            "dataset": dataset,
            "group": group,
            "cache": cache,
            "mode": mode,
            "axis": axis,
            "runs": len(values),
            "total_median_sec": median([v["total_sec"] for v in values]),
            "read_median_sec": median([v["read_sec"] for v in values]),
            "write_median_sec": median([v["write_sec"] for v in values]),
            "plan_hashes": ";".join(sorted({v["plan_hash"] for v in values})),
        })

    with (OUT / "case_medians.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)

    run_summary = []
    by_run_group = defaultdict(list)
    for row in file_totals:
        by_run_group[(row["dataset"], row["group"])].append(row)
    for (dataset, group), values in sorted(by_run_group.items()):
        totals = [v["total_sec"] for v in values]
        run_summary.append({
            "dataset": dataset,
            "group": group,
            "runs": len(values),
            "run_total_median_sec": median(totals),
            "run_total_mean_sec": statistics.mean(totals),
            "run_total_min_sec": min(totals),
            "run_total_max_sec": max(totals),
        })
    with (OUT / "run_summary.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(run_summary[0].keys()))
        writer.writeheader()
        writer.writerows(run_summary)


if __name__ == "__main__":
    main()
