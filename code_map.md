# Code Map

根目录只提供全局导航；模块细节已下沉到各目录的 `code_map.md`。

## 模块导航

| 区域 | 职责 | 详细地图 |
|---|---|---|
| `include/block3d/` | `.b3d` 格式、Morton/块布局、转换与读取公共 API | [`include/block3d/code_map.md`](include/block3d/code_map.md) |
| `src/` | 块转换、mmap reader、CLI、大数据测试实现 | [`src/code_map.md`](src/code_map.md) |
| `tests/` | 库功能、并发回归和真实 CLI 进程测试 | [`tests/code_map.md`](tests/code_map.md) |
| `tools/` | Phase 4/5 辅助脚本，例如 dispatch A/B 日志中位数比较 | 本文“辅助工具” |
| `experiments/` | 已归档的正式实验日志、派生 CSV 和报告说明；不存放大型 `.raw` 输出，以及归档文档 | [`experiments/CLAUDE.md`](experiments/CLAUDE.md), [`experiments/code_map.md`](experiments/code_map.md) |
| `generator_py/` | 随机三维数据生成器及 CLI/GUI/TUI | [`generator_py/code_map.md`](generator_py/code_map.md) |
| `CMakeLists.txt` | C++17、OpenMP 和库/CLI/测试构建目标 | 本文“构建目标” |
| `项目需求文档.md` | 数据格式、官方环境、功能和评分要求 | 原始需求文档 |

## 端到端数据流

```text
纯 float32 原始 .dat（无头，X-Y-Z；Z 连续）
  -> convert_raw_to_blocked()
  -> 64-byte header + logical offset table + aligned Morton-ordered blocks
  -> .b3d
  -> BlockedFileReader + mmap
  -> slices / columns / subvolume / point / full volume / verify
```

Python 生成测试数据时必须使用 `--no-header`，否则文件前会包含 C++ 转换器不识别的 `3DDF` 头。

## `.b3d` 核心结构

```text
FileHeader (64 B)
uint64_t block_offsets[total_blocks]  # 逻辑 X-Y-Z 块顺序
padding to 4096-byte boundary
fixed-size blocks                     # Morton/Z-order 物理顺序
```

- 索引值：块的绝对文件字节偏移。
- 块内布局：局部 X-Y-Z，Z 连续。
- 边界块：固定大小并补零。
- 当前格式：magic `3DBK`，version `1`。

## 构建目标

| 目标 | 作用 |
|---|---|
| `block3d` | `core.cpp`、`reader.cpp`、`converter.cpp` 库 |
| `block3d_benchmark_cache` | cold/hot 参数、scrub 文件准备与逐页冲刷的 benchmark 私有库 |
| `block3d_cli` | `convert/info/cache-prepare/bench/verify/extract` CLI |
| `test_block3d` | 库级测试可执行文件 |
| `test_cli` | 启动真实 CLI 的集成测试 |
| `run_test` | 大数据转换、写盘和性能工具；不属于 CTest |
| `test_dispatch_compare_tool` | Python 可用时注册的 Phase 4 A/B 日志比较脚本 smoke 测试 |

## 辅助工具

| 工具 | 作用 |
|---|---|
| `tools/compare_dispatch_ab.py` | 读取多轮 `BENCHMARK_RESULT` 日志，按 `(cache, mode, axis)` 校验 `PLAN_HASH`、计算 round-robin/contiguous 中位数并给出默认策略决策建议。它只分析已生成日志，不自动运行 benchmark。 |
| `tools/run_phase5_validation.py` | 顺序驱动 test18/test50 形式化 Phase 5 验证：raw verify、round-robin/contiguous 多轮 `run_test`、dispatch A/B 比较。 |

## 实验归档

| 目录 | 内容 |
|---|---|
| `experiments/批量融合读取与读写流水线优化开发方案实验数据归档/` | 批量融合读取与读写流水线优化开发方案实验数据归档正式验收归档：选定 benchmark 日志、派生 CSV、dispatch A/B 文本输出和报告说明。 |

该归档只保存文本日志和汇总结果，不保存 `benchmark_output` / `phase5_outputs` 下的大型 `.raw` 切片输出。

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 关键跨模块约束

- `block3d_cli bench` 只计读取；`run_test` 计读取与输出写盘/同步。两者都支持 `cold`/`hot`/`both` 缓存阶段，计时口径不能混用。
- `run_test` 默认正式路径为 fused batch + pipeline on（256 MiB payload 软预算、auto window、单 writer）+ output sync requested + round-robin dispatch；round-robin 已由 2026-07-17 test18/test50 各 3 轮 A/B 中位数固定，`--pipeline off` 和 `--read-dispatch contiguous` 保留为诊断/A/B/回滚开关。
- `run_test both` 在同一固定计划上依次执行 scrub cold 与 warm-up hot，写入独立 phase 目录，比较 `total_sec`，并逐文件校验 cold/hot 输出一致；运行日志默认写入当前工作目录的 `logs/`。
- `max_memory_mb` 是局部软预算，不是整个进程内存上限。
- reader 的共享块列表缓存必须在线程锁外使用副本。
- 性能结果必须结合 OS 页缓存状态解释。
- Morton 位扩展使用 64-bit code，每轴保留低 21 位。
- `convert` 默认自适应块大小（存储探测 + `auto_block_size()`）；`--block-size 0` 或不传即为自动，显式 N 可覆盖。
- `read_x/y/z_slice` 和 fused `read_slices_batch(_stream)` 复用 reader 生命周期持久线程池；块分发可用 `round-robin` 或 `contiguous` physical chunks 做 A/B。
- `block3d_cli cache-prepare` 创建可复用 scrub 文件；`bench --cold-method scrub` 逐页读取该文件作为本地 `cold_scrubbed` 口径。
- `--warm-up` 标志（`cli bench` 和 `run_test`）在计时前将数据区预加载到 OS 页缓存；`cli bench` 中它是 `--cache-mode hot` 的兼容别名。