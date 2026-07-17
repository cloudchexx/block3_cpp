# Experiments Code Map

本目录保存可用于报告和复核的实验归档。原则是只保存文本日志、派生 CSV/TXT 和说明文档，不保存大型 `.dat`、`.b3d`、`.raw` 或 benchmark 输出目录。

## `批量融合读取与读写流水线优化开发方案实验数据归档/`

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

## `phase6_20260717/`

块内微分块 v2 / micro8 的正式 A/B 归档。

| 路径 | 内容 |
|---|---|
| `README.md` | 实验配置、关键结果、结论和复核注意事项。 |
| `块内微分块v2实现方案.md` | 原实现方案，从项目根目录归档到本实验目录。 |
| `commands/run_phase6_ab.sh` | 复跑 legacy/micro8 A/B 的命令草案，micro8 通过 `--b3d-file` 显式指定。 |
| `logs/test18_legacy/` | test18 legacy round-robin 三轮日志。 |
| `logs/test18_micro8/` | test18 micro8 三轮日志。 |
| `logs/test50_legacy/` | test50 legacy round-robin 三轮日志。 |
| `logs/test50_micro8/` | test50 micro8 两轮日志。 |
| `derived/summarize_phase6_logs.py` | 从归档日志生成 CSV 汇总的脚本。 |
| `derived/benchmark_results.csv` | 每条 `BENCHMARK_RESULT` 一行。 |
| `derived/case_medians.csv` | 每个 dataset/layout/cache/mode/axis 的中位数。 |
| `derived/run_totals.csv` | 每个日志文件的总 elapsed、workload 总和和格式信息。 |
| `derived/run_summary.csv` | 每个实验组的总 elapsed/workload 中位数。 |

## 使用原则

- 报告主指标引用 `run_test` 的 `total_sec`；Phase 5 pipeline 模式下不要用 `read_sec + write_sec` 代替总耗时。
- 复核 A/B 时检查同一 `(cache, mode, axis)` 的 `PLAN_HASH` 是否一致。
- 大型原始输出位于 `Z:\wutan\block3d-data\phase5_outputs` 或 `benchmark_output`，不纳入源码归档；Phase 6 的 `test18_micro8.b3d` / `test50_micro8.b3d` 也保留在数据目录，不纳入源码归档。
- 新增正式实验时建立新的日期目录，不覆盖既有归档。
