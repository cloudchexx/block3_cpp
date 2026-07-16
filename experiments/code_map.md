# Experiments Code Map

本目录保存可用于报告和复核的实验归档。原则是只保存文本日志、派生 CSV/TXT 和说明文档，不保存大型 `.dat`、`.b3d`、`.raw` 或 benchmark 输出目录。

## `phase5_20260717/`

Phase 5 默认路径收敛与 dispatch A/B 的正式证据归档。

| 路径 | 内容 |
|---|---|
| `README.md` | 实验配置、目录说明、关键结果和报告引用注意事项。 |
| `logs/test18_previous/` | test18 可比旧日志，作为 pre-Phase5-default baseline。 |
| `logs/test18_phase5_round_robin/` | test18 Phase 5 默认 round-robin 三轮日志。 |
| `logs/test18_phase5_contiguous/` | test18 contiguous A/B 三轮日志。 |
| `logs/test50_phase5_round_robin/` | test50 Phase 5 默认 round-robin 三轮日志。 |
| `logs/test50_phase5_contiguous/` | test50 contiguous A/B 三轮日志。 |
| `derived/summarize_phase5_logs.py` | 从归档日志生成 CSV 汇总的确定性脚本。 |
| `derived/benchmark_results.csv` | 每条 `BENCHMARK_RESULT` 一行。 |
| `derived/case_medians.csv` | 每个 dataset/group/cache/mode/axis 的中位数。 |
| `derived/run_totals.csv` | 每个日志文件的 12 case 总耗时。 |
| `derived/run_summary.csv` | 每个实验组的运行总耗时中位数、均值、最小值和最大值。 |
| `derived/pipeline_diagnostics.csv` | 每条 `PIPELINE_RESULT` 一行，用于分析队列、等待和 payload budget。 |
| `derived/test18_dispatch_ab.txt` | `tools/compare_dispatch_ab.py` 对 test18 的 A/B 输出。 |
| `derived/test50_dispatch_ab.txt` | `tools/compare_dispatch_ab.py` 对 test50 的 A/B 输出。 |

## 使用原则

- 报告主指标引用 `run_test` 的 `total_sec`；Phase 5 pipeline 模式下不要用 `read_sec + write_sec` 代替总耗时。
- 复核 A/B 时检查同一 `(cache, mode, axis)` 的 `PLAN_HASH` 是否一致。
- 大型原始输出位于 `Z:\wutan\block3d-data\phase5_outputs` 或 `benchmark_output`，不纳入源码归档。
- 新增正式实验时建立新的日期目录，不覆盖既有归档。
