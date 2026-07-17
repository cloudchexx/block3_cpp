# Phase 5 benchmark evidence archive

> Date: 2026-07-17  
> Scope: Block3D Phase 5 default-path validation and dispatch A/B decision  
> Data source: selected logs copied from `Z:\wutan\block3d-data\logs` plus derived summaries generated from those logs.

This directory is the report-ready archive for the Phase 5 experiment. It intentionally stores text logs and derived CSV/TXT summaries only. Large benchmark output `.raw` files under `Z:\wutan\block3d-data\phase5_outputs` are not copied here.

## Experiment configuration

The validated Phase 5 default path is:

```text
batch_read=fused
pipeline=on
pipeline_memory=256 MiB payload
pipeline_window=auto
writer_threads=1
persistent_reader_pool=on
read_dispatch=round-robin
auto_block_size=unchanged
output_sync=requested
warmup_scope=workload
```

Common benchmark conditions:

```text
cache_mode=both
cold_method=scrub
cold_isolation=suite
cold_scrub_ratio=1.5
cold_scrub_passes=1
cold_settle_ms=1000
timing_scope=read_write_total
random_count=100
seq_count=10
threads=16 (auto)
```

Correctness gates recorded in the source run:

- `CACHE_VALIDITY state=scrub_success` in every archived full benchmark run.
- `OUTPUT_VERIFY_RESULT ok=1` in every Phase 5 `run_test both` run.
- Independent raw verification before the formal Phase 5 runs:
  - `test18`: 20,000 random points passed in 0.710162 s.
  - `test50`: 20,000 random points passed in 1.11964 s.

## Directory layout

```text
experiments/批量融合读取与读写流水线优化开发方案实验数据归档/
  README.md
  logs/
    test18_previous/              # 2026-07-16/early 2026-07-17 comparable baseline logs
    test18_phase5_round_robin/    # Phase 5 default dispatch, 3 rounds
    test18_phase5_contiguous/     # Phase 5 A/B candidate, 3 rounds
    test50_phase5_round_robin/    # Phase 5 default dispatch, 3 rounds
    test50_phase5_contiguous/     # Phase 5 A/B candidate, 3 rounds
  derived/
    summarize_phase5_logs.py      # deterministic parser used to regenerate CSV summaries
    benchmark_results.csv         # one row per BENCHMARK_RESULT
    case_medians.csv              # per dataset/group/cache/mode/axis medians
    run_totals.csv                # one row per archived log file
    run_summary.csv               # run-total median/mean/min/max by group
    pipeline_diagnostics.csv      # one row per PIPELINE_RESULT
    test18_dispatch_ab.txt        # compare_dispatch_ab.py output
    test50_dispatch_ab.txt        # compare_dispatch_ab.py output
```

## Summary results

### test18 vs previous logs

The previous test18 logs are comparable `run_test` logs using scrub cold/hot and workload warm-up, but they predate the Phase 5 default-path log fields (`BATCH_READ`, `PIPELINE`, `READ_DISPATCH`, `PIPELINE_RESULT`). They are therefore treated as a pre-Phase5-default baseline rather than the earliest implementation baseline.

| Group | Runs | Median full-run total |
|---|---:|---:|
| `test18_previous` | 4 | 27.7775 s |
| `test18_phase5_round_robin` | 3 | 24.9400 s |
| `test18_phase5_contiguous` | 3 | 27.6220 s |

Phase 5 round-robin improves the comparable test18 full-run median from 27.7775 s to 24.9400 s:

```text
speedup = 27.7775 / 24.9400 = 1.1138x
reduction = 10.2%
```

### Dispatch A/B conclusion

| Dataset | Round-robin mean of medians | Contiguous mean of medians | Ratio contiguous / round-robin | Decision |
|---|---:|---:|---:|---|
| test18 | 2.086833 s | 2.214583 s | 1.061217 | keep round-robin |
| test50 | 12.213083 s | 15.725417 s | 1.287588 | keep round-robin |

The final default is therefore:

```text
read_dispatch=round-robin
```

`contiguous` remains available only as a diagnostic/A/B/rollback flag.

### test50 Phase 5 A/B

There is no pre-Phase5 test50 baseline in the archived `block3d-data/logs` set. The available test50 evidence is the Phase 5 internal A/B:

| Group | Runs | Median full-run total |
|---|---:|---:|
| `test50_phase5_round_robin` | 3 | 147.8500 s |
| `test50_phase5_contiguous` | 3 | 195.6420 s |

Contiguous is substantially worse for test50 and must not be promoted to default.

## Regenerating derived files

From the project root:

```bash
python experiments/批量融合读取与读写流水线优化开发方案实验数据归档/derived/summarize_phase5_logs.py
python tools/compare_dispatch_ab.py \
  --round-robin experiments/批量融合读取与读写流水线优化开发方案实验数据归档/logs/test18_phase5_round_robin/*.log \
  --contiguous experiments/批量融合读取与读写流水线优化开发方案实验数据归档/logs/test18_phase5_contiguous/*.log \
  > experiments/批量融合读取与读写流水线优化开发方案实验数据归档/derived/test18_dispatch_ab.txt
python tools/compare_dispatch_ab.py \
  --round-robin experiments/批量融合读取与读写流水线优化开发方案实验数据归档/logs/test50_phase5_round_robin/*.log \
  --contiguous experiments/批量融合读取与读写流水线优化开发方案实验数据归档/logs/test50_phase5_contiguous/*.log \
  > experiments/批量融合读取与读写流水线优化开发方案实验数据归档/derived/test50_dispatch_ab.txt
```

## Report notes

- Use `total_sec` as the main metric for `run_test`, because it includes read + output file create/write/close + requested sync.
- Do not add `read_sec + write_sec` for Phase 5 pipeline runs; `TIMING_MODEL value=overlapped_pipeline` means read and write overlap.
- Use `PLAN_HASH` fields to verify that compared cold/hot and A/B cases used the same request plan.
- The archived logs are small enough for version control; the generated raw benchmark outputs are not.
