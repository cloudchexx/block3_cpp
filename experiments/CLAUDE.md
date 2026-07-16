# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with experiment archives in this directory.

## 本目录职责

`experiments/` 保存可复核、可写入报告的实验归档。它不是运行时输出目录，也不是测试夹具目录。

## 归档原则

- 只保存文本日志、派生 CSV/TXT、汇总脚本和说明文档。
- 不保存大型 `.dat`、`.b3d`、`.raw`、`benchmark_output/` 或 `phase5_outputs/` 内容。
- 新增正式实验时创建新的日期目录，例如 `phase6_YYYYMMDD/`，不要覆盖既有归档。
- 每个正式归档目录应至少包含：`README.md`、原始文本日志副本、可重新生成派生结果的脚本或命令说明。
- 引用 `run_test` 性能时以 `total_sec` 为主指标；pipeline 模式下不要把 `read_sec + write_sec` 当成总耗时。
- A/B 结果必须能追溯到相同 `(cache, mode, axis)` 的 `PLAN_HASH`。

## 当前归档

- `phase5_20260717/`：Phase 5 默认路径收敛、test18/test50 正式验证和 round-robin/contiguous dispatch A/B 证据。
