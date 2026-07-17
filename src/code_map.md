# Source Code Map

## 库实现

### `core.cpp`

实现 `BlockLayout3D::block_order()`：枚举所有逻辑块，计算 Morton code，排序后返回 `(bx, by, bz)`。转换器使用该结果决定物理写入顺序。

实现 `detect_storage_medium()` 和 `auto_block_size()`。存储探测执行 5 轮 1MiB 写入+fsync，取中位延迟；块大小选择在最差轴切片触及的目标块数约束下扫描 16–256（步长 8）。

### `converter.cpp`

- `extract_block()`：从无头 X-Y-Z 原始 mmap 中提取固定块；legacy 完整块逐行 `memcpy`，边界块补零；v2 micro-tiled 按 `micro_tiled_local_offset()` 写入固定 8³ micro tile。
- `validate_convert_options()`：校验 legacy 无 micro size，micro-tiled 第一版只允许 `micro_size=8` 且 `block_size % micro_size == 0`。
- `convert_raw_to_blocked()`：

```text
MappedFile(raw)
 -> BlockLayout3D + Morton order
 -> write FileHeader (v1 legacy or v2 micro-tiled metadata)
 -> reserve logical offset table
 -> align data area
 -> double-buffered pipeline:
    extract batch N+1 (std::async + OpenMP) || write batch N (sequential I/O)
 -> backfill logical offset table
```

块提取可并行，文件写入串行。双缓冲使 CPU 提取与磁盘 I/O 重叠。`max_memory_mb` 只参与批次大小计算。

CLI 层（`cli.cpp` 和 `run_test.cpp`）在未显式指定 `--block-size` 时默认调用 `detect_storage_medium()` + `auto_block_size()`，根据实际硬件自适应选择块大小。

### `benchmark_cache.cpp`

提供 benchmark 私有缓存实验支持：解析/表示 `cache-mode`、`cold-method`、`cold-isolation`、`warmup-scope`，查询页面大小和物理内存，计算 scrub 默认容量，创建确定性伪随机 scrub 文件，并用普通 buffered I/O 逐页触碰 scrub 文件生成 `cold_scrubbed`。该模块供 `block3d_cli bench` 和 `run_test` 共用。

公有接口定义在 `include/block3d/benchmark_cache.hpp`，包含 `CacheMode`/`ColdMethod`/`ColdIsolation`/`WarmupScope` 枚举、`CacheOptions`/`CachePrepareOptions`/`CachePrepareResult`/`CacheScrubResult` 结构体，以及对应的解析/格式化/辅助函数。

### `reader.cpp`

#### `MappedFile`

- Windows：file handle + mapping handle + mapped view。
- POSIX：fd + `mmap`。
- `prefault()`：优先请求 OS 预取，失败时按 stride 触页。

#### `BlockedFileReader`

构造：读取头和索引表，严格解析 v1 legacy 或 v2 micro-tiled metadata，再 mmap 整个 `.b3d`，并按 `num_threads_` 创建 reader 生命周期内的持久块处理线程池。

关键内部路径：

```text
read_*_slice()
 -> sorted_block_list(axis, index)
 -> logical block offset lookup
 -> sort by physical offset
 -> for_each_block_parallel(): reuse the reader's persistent worker pool
    when block_count > num_threads × 4, using the reader's dispatch strategy
    (round-robin or contiguous physical chunks)
 -> each worker copies its block fragments into non-overlapping
    result regions (no locks needed)
```

其他路径：

- `read_slices_batch()` / `read_slices_batch_stream()`：先预校验 axis 和整批 index，按 `(axis, index / block_size)` 首次出现顺序分组，再按 `window_slices` 融合遍历同一 layer 的物理排序块；所有单片和批量路径都通过布局感知 local offset/copy helper 兼容 v1/v2；兼容 API 按 `request_pos` 收集结果；`options.num_threads` 非零时作为本次调用的 worker 上限。
- `read_*_column()`：通过 reader 的布局感知 `local_offset()` 读取；batch column 调用单列路径。
- `read_subvolume()`：遍历相交块并通过布局感知 `local_offset()` 复制有效 Z 区间；区间为半开区间。
- `read_full_volume()`：完整范围的 `read_subvolume()`。
- `verify()`：固定随机坐标，多线程对照原始 `.dat`。
- `warm_up()`：同步或异步预热 OS 页缓存；OS 预取提示后会实际触碰 stride 页和末字节再完成。

## 可执行文件

### `cli.cpp` -> `block3d_cli`

| 子命令 | 路径 |
|---|---|
| `convert` | `convert_raw_to_blocked()`；支持 `--layout legacy|micro-tiled` 与 `--micro-size 8` |
| `info` | reader 元数据、version/layout/micro size 和存储比例 |
| `cache-prepare` | 创建可复用冷缓存 scrub 文件 |
| `bench` | 固定计划三轴 `read_slices_batch()`，支持 cold/hot/both 缓存阶段，不写结果 |
| `verify` | `BlockedFileReader::verify()` |
| `extract` | 单切片或单列写无头 float32 |

所有子命令异常在 `main()` 统一转换为错误消息和退出码 2。

### `run_test.cpp` -> `run_test`

面向 `test18`、`test50` 及自定义数据集的综合性能工具：

- 定位数据文件和复用有效 `.b3d`（含 block_size 校验）；
- 可选转换、自动 block_size 探测和随机点校验；
- 支持 `--cache-mode cold|hot|both`、`--cold-method scrub|none`、`--cold-isolation suite|case`、`--warmup-scope dataset|workload`；默认 warmup scope 为 `workload`；
- 生成一次固定随机/连续切片请求计划并输出 `PLAN_HASH`，cold/hot 复用同一计划；
- cold 阶段可逐页读取 scrub 文件生成 `cold_scrubbed`，hot 阶段显式 warm-up 后生成 `hot_prefetched`；
- 输出 read/write/total 拆分时间，主指标仍是含写盘的 total；`BENCHMARK_RESULT` 额外记录 reader pool 的 workers/jobs/serial fallback 诊断字段，`READ_DISPATCH` 记录 round-robin/contiguous A/B 策略；
- 读取并把每张切片写入独立 phase 目录，支持可选 `fflush` + `_commit/fsync`；
- 默认启用 `--pipeline on` 的有界 producer-consumer 写盘流水线：按 256 MiB payload 软预算推导 read window 与队列容量，单 writer 保持逐文件 write/sync/close，并输出 `TIMING_MODEL`、`PIPELINE_RESULT`；`--pipeline off` 仅用于诊断或回滚；
- `both` 模式输出 `BENCHMARK_COMPARE` 并逐文件校验 cold/hot 结果一致；
- 默认保存控制台日志到当前工作目录 `logs/`，包括 stdout/stderr；Phase 5 正式日志另有报告归档副本位于 `../experiments/批量融合读取与读写流水线优化开发方案实验数据归档/`，Phase 6 micro8 A/B 日志归档副本位于 `../experiments/phase6_20260717/`；
- 支持自定义数据集：`--datasets <name> --dim-x N --dim-y N --dim-z N`，文件自动映射为 `<name>.dat` / `<name>.b3d`；也可用 `--b3d-file FILE` 在单数据集运行时显式指定待测 `.b3d`，此时维度从 `.b3d` 头读取、不会按同名 `.dat` 派生/转换，只有 `--verify` 才需要 raw 文件。

其计时包含切片结果写盘，不包含 reader 构造和 mmap。