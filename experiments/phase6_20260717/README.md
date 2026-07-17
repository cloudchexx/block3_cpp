# Phase 6：块内微分块 v2 / micro8 A/B 归档

归档日期：2026-07-17

## 目的

验证 `.b3d` v2 micro-tiled 块内布局在正式 `run_test` 读写流水线口径下，相对 v1 legacy 是否能改善 test50 random Y/Z，同时不让 test18 总体明显退化。

原设计文档已从项目根目录移入本目录：

- `块内微分块v2实现方案.md`

## 目录

| 路径 | 内容 |
|---|---|
| `commands/run_phase6_ab.sh` | 可复跑 A/B 的命令草案；v2 文件通过 `--b3d-file` 显式指定。 |
| `logs/test18_legacy/` | test18 legacy round-robin 三轮正式日志。 |
| `logs/test18_micro8/` | test18 micro8 三轮正式日志。 |
| `logs/test50_legacy/` | test50 legacy round-robin 三轮正式日志。 |
| `logs/test50_micro8/` | test50 micro8 两轮正式日志。 |
| `derived/summarize_phase6_logs.py` | 从归档日志生成 CSV 的脚本。 |
| `derived/benchmark_results.csv` | 每条 `BENCHMARK_RESULT` 一行。 |
| `derived/case_medians.csv` | 每个 dataset/layout/cache/mode/axis 的中位数。 |
| `derived/run_totals.csv` | 每个日志文件的总 elapsed、12 case workload 总和和格式信息。 |
| `derived/run_summary.csv` | 每个实验组的总 elapsed/workload 中位数。 |

本归档只保存文本日志和派生结果，不保存 `.dat`、`.b3d`、`.raw` 或 `benchmark_output/` 大型输出目录。

## 统一实验口径

所有正式日志均为：

```text
CACHE_CONFIG mode=both cold_method=scrub cold_isolation=suite warmup_scope=workload timing_scope=read_write_total
BATCH_READ mode=fused window_slices=4
PIPELINE mode=on buffer_mb=256
READ_DISPATCH strategy=round_robin
output_sync=requested
```

主要比较口径使用 `run_test` 的 `BENCHMARK_RESULT total_sec` 和每组 workload 总和；pipeline 模式下不要用 `read_sec + write_sec` 代替总耗时。

同一 dataset 的 legacy 与 micro8 日志 `PLAN_HASH_SUITE` 一致：

| dataset | PLAN_HASH_SUITE |
|---|---|
| test18 | `0c5cecf06c8ed881` |
| test50 | `5bc463ef8b4289a1` |

micro8 日志均包含格式确认行，例如：

```text
B3D_FORMAT dataset=test18 file=Z:/wutan/block3d-data/test18_micro8.b3d version=2 layout=micro-tiled micro_size=8 block_size=56
B3D_FORMAT dataset=test50 file=Z:/wutan/block3d-data/test50_micro8.b3d version=2 layout=micro-tiled micro_size=8 block_size=64
```

## 关键结果

### 组级总耗时

来自 `derived/run_summary.csv`：

| dataset | layout | runs | Total elapsed median | workload total median |
|---|---:|---:|---:|---:|
| test18 | legacy | 3 | `125.7s` | `24.94s` |
| test18 | micro8 | 3 | `123.3s` | `21.397s` |
| test50 | legacy | 3 | `316.4s` | `147.85s` |
| test50 | micro8 | 2 | `247.1s` | `97.083s` |

说明：`Total elapsed` 包含 scrub/warm-up 等固定流程时间；`workload total` 为 12 条 `BENCHMARK_RESULT total_sec` 的运行内总和，更直接反映读写 workload。

### test18 median

| cache | mode | axis | legacy avg | micro8 avg | 结论 |
|---|---|---|---:|---:|---|
| cold | random | X | `57.611ms` | `77.307ms` | micro8 变慢约 34% |
| cold | random | Y | `77.587ms` | `27.585ms` | micro8 变快约 64% |
| cold | random | Z | `42.041ms` | `32.027ms` | micro8 变快约 24% |
| hot | random | X | `19.086ms` | `23.440ms` | micro8 变慢约 23% |
| hot | random | Y | `17.335ms` | `18.065ms` | 基本持平/小幅变慢 |
| hot | random | Z | `27.936ms` | `27.827ms` | 基本持平 |

`test18` 三轮中位数不再显示总体退化：`Total elapsed` 小幅下降约 1.9%，workload total 下降约 14%。

### test50 median

| cache | mode | axis | legacy avg | micro8 avg | 结论 |
|---|---|---|---:|---:|---|
| cold | random | X | `56.636ms` | `80.340ms` | micro8 变慢约 42% |
| cold | random | Y | `411.210ms` | `151.521ms` | micro8 变快约 63% |
| cold | random | Z | `258.378ms` | `159.632ms` | micro8 变快约 38% |
| hot | random | X | `45.435ms` | `84.879ms` | micro8 变慢约 87% |
| hot | random | Y | `402.214ms` | `217.205ms` | micro8 变快约 46% |
| hot | random | Z | `266.087ms` | `247.850ms` | micro8 小幅变快 |

`test50` 主目标达成：random Y/Z 明显下降，workload total 中位数约从 `147.85s` 降到 `97.083s`。

## 结论

- v2 micro8 在当前 test18/test50 `axis=all mode=all` 的正式 `run_test` 口径下总体通过守门。
- micro8 明显改善 test50 random Y/Z，是本阶段主要收益来源。
- micro8 会牺牲 X slice，尤其 test50 hot random X 和 test18/test50 cold random X；如果负载 X-heavy，legacy 仍可能更合适。
- 因此代码层继续保留显式 layout 开关：默认转换仍为 legacy v1，`--layout micro-tiled --micro-size 8` 生成 v2。是否默认切换应由上层产品/比赛负载策略单独决定。

## 重新生成派生 CSV

```bash
cd Z:/wutan/block3d-cpp
python experiments/phase6_20260717/derived/summarize_phase6_logs.py
```
