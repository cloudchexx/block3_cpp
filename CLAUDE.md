# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 注意：

1.你需要做好项目的版本管理，方便出错各种情况下的版本回滚，使用jujutsu进行项目管理，你拥有对应的skill

2.注意windows powershell命令格式，你拥有对应的skill

## 文档分层

本文件只记录跨模块约束和常用命令。进入具体目录后，优先阅读该目录自己的 `CLAUDE.md` 和 `code_map.md`：

- `include/block3d/`：公共 API、文件格式和布局类型
- `src/`：转换、读取、CLI 和大数据基准实现
- `tests/`：库测试及 CLI 进程集成测试
- `experiments/`：正式实验日志、派生数据和报告归档
- `generator_py/`：Python 测试数据生成器

全局导航见 `code_map.md`，不要把子模块实现细节重新堆回根目录文档。

## 构建与测试

项目使用 CMake 3.14+、C++17；OpenMP 可选。

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

运行单个 CTest 测试：

```bash
ctest --test-dir build -C Release -R '^test_block3d$' --output-on-failure
ctest --test-dir build -C Release -R '^test_cli$' --output-on-failure
```

准备本地冷缓存 scrub 文件与冷/热 benchmark：

```bash
build/Release/block3d_cli cache-prepare SCRUB.bin --size auto --cold-scrub-ratio 1.5
build/Release/block3d_cli bench DATA.b3d --cache-mode both --cold-method scrub --cold-scrub-file SCRUB.bin --warmup-scope workload
```

只构建某个目标：

```bash
cmake --build build --config Release --target block3d_cli
cmake --build build --config Release --target test_block3d
cmake --build build --config Release --target test_cli
cmake --build build --config Release --target run_test
```

Windows 多配置生成器的程序通常位于 `build/Release/`。项目未配置独立的 formatter、linter 或 Python 自动测试。

## 架构总览

项目把无头部、X-Y-Z 顺序的原始 `float32` `.dat` 转换成 `.b3d`：固定立方体块内部仍为 X-Y-Z 顺序，块数据按 Morton/Z-order 写入，逻辑块索引表保存每个块的绝对文件字节偏移。`BlockedFileReader` mmap 整个 `.b3d`，按索引定位块并提供 X/Y/Z 切片、列、子体积、单点和完整体积读取。

主要调用链：

```text
block3d_cli convert
  -> convert_raw_to_blocked
  -> BlockLayout3D::block_order
  -> .b3d

BlockedFileReader
  -> FileHeader + block offset table
  -> MappedFile
  -> slice / column / subvolume / point APIs
```

`run_test` 是包含结果写盘和同步的大数据性能工具；它不是 CTest。`block3d_cli bench` 只测读取，两者计时口径不能混用。

## 跨模块不变量

- 原始 `.dat` 无头部，索引为 `x * dim_y * dim_z + y * dim_z + z`。
- `.b3d` 头固定 64 字节，magic 为 `3DBK`，当前版本为 1，数据区按 4096 字节对齐。
- 索引表按逻辑块 X-Y-Z 顺序排列；块数据按 Morton 顺序排列；索引值是绝对字节偏移。
- 块内 Z 连续；边界块必须补零并在读取时按原始尺寸裁剪。
- 切片输出布局分别为 Y-Z、X-Z、X-Y；子体积区间为半开区间。
- `max_memory_mb` 当前只是局部软预算，不是进程级硬上限。
- 批量读取的线程安全依赖块列表缓存按值返回；不要改成锁外持有缓存内部引用。
- 性能结果依赖 OS 页缓存。描述或比较数据时必须说明冷缓存、转换后缓存、验证后缓存或显式 warm-up 状态。
- Python 生成器默认写 `3DDF` 自定义头；只有 CLI `--no-header` 生成的文件可直接交给 C++ 转换器。

## 冷/热缓存 benchmark

- `block3d_cli cache-prepare` 只负责一次性创建可复用 scrub 文件，不在 benchmark 开始前自动生成大文件。
- `block3d_cli bench` 和 `run_test` 都支持 `--cache-mode cold|hot|both`，默认 `both`；`--warm-up` 是兼容旧脚本的 `--cache-mode hot` 别名，不能与 `cold/both` 同时使用。
- cold 阶段默认 `--cold-method scrub`，必须显式提供 `--cold-scrub-file`；scrub 失败、文件不存在或容量不足时不能降级为 `cold_first_touch`。
- 临时/兼容测试可使用 `--cold-method none`，输出状态为 `cold_first_touch`，不得把它描述成本项目标准冷缓存成绩。
- `--cold-isolation suite` 表示 scrub 一次后跑完整套计划；`case` 表示每个 axis/mode case 前重新 scrub。
- `--warmup-scope dataset|workload` 分别表示预热整个数据区或固定请求计划会访问的工作集。
- `block3d_cli bench` 的计时范围是 `read_only`；`run_test` 的计时范围是 `read_write_total`，主指标 `total_sec` 包含读取、文件创建/写入/关闭及可选 `_commit`/`fsync`。
- 两个入口都输出固定 `PLAN_HASH`，确保 cold/hot 使用同一请求计划。`run_test both` 还会预估输出空间、创建独立的 `cold_scrubbed/` 与 `hot_prefetched/` 目录，并逐文件校验两阶段输出一致。
- `run_test` 默认正式路径为 `batch_read=fused`、`pipeline=on`、`pipeline_memory=256MiB payload`、`pipeline_window=auto`、单 writer、`output_sync=requested`、`read_dispatch=round-robin`；该 dispatch 默认已由 2026-07-17 test18/test50 各 3 轮 A/B 中位数固定，开发开关只用于诊断、A/B 或回滚。
- `run_test` 默认把控制台输出（包括错误）保存到当前工作目录的 `logs/`；`--no-log` 仅用于明确不需要日志的临时运行。
- Phase 5 正式实验证据已归档到 `experiments/phase5_20260717/`：只包含文本日志、派生 CSV/TXT 和报告说明，不包含大型 `.raw` 输出。后续写报告或复核默认策略时优先引用该归档。

## 自适应块大小与存储检测

- `convert` 默认不再固定 block_size=32。未指定 `--block-size` 时，程序会在输出目录执行 5 轮 1MiB 写入+fsync 探测，根据中位延迟将存储分类为 HDD / SSD / NVMe / Unknown。
- `auto_block_size()` 根据数据维度和介质类型选择块大小（范围 16–256，步长 8）：HDD 优先减少寻道（目标≤400 块/切片），SSD/NVMe 在减少传输浪费和块数之间取得平衡（目标≤2000 块/切片）。
- 若需固定块大小，仍然支持 `--block-size N` 显式覆盖。

## 切片内并行

- `read_x_slice` / `read_y_slice` / `read_z_slice` 现已支持切片内多线程块处理。当块数 > `num_threads × 4` 时，已排序的块列表会跨线程分片，各线程写入输出缓冲区中不重叠的 `(Y,Z)` / `(X,Z)` / `(X,Y)` 区域。
- `read_slices_batch` / `read_slices_batch_stream` 按 block layer/window 融合读取，与单片路径共享 reader 生命周期内的持久线程池。
- reader 的块分发策略可显式选择 `round-robin` 或 `contiguous` physical chunks；默认仍为 `round-robin`，`--read-dispatch` 仅用于  A/B 和回滚，不应在评审现场动态搜索。

## 预热

- 两个 benchmark 入口（`block3d_cli bench` 和 `run_test`）均支持 `--warm-up`，统一作为只执行 hot phase 的兼容别名；显式新命令优先使用 `--cache-mode hot`。
- 预热使用 `PrefetchVirtualMemory`（Windows）/ `madvise(MADV_WILLNEED)`（Linux）作为提示，然后实际触碰目标范围 stride 页面和最后一个字节；完成后才视为 warm-up done。
- 预热后的 benchmark 结果是热缓存数据——仅供诊断，并非冷缓存性能基准。

## 修改后的验证范围

- 修改格式、布局、转换或 reader：运行 `test_block3d` 和 `test_cli`。
- 修改 CLI 参数、cache-prepare、bench phase 输出或错误处理：至少运行 `test_cli`。
- 修改并发缓存逻辑：必须覆盖 `test_concurrent_adjacent_same_block_key` 所在的完整 `test_block3d`。
- 修改性能路径时，不要用 `run_test` 替代正确性测试；大数据测试需要本地数据集和大量磁盘空间。

不要编辑或把上下文建立在 `build/`、`logs/`、`generator_py/__pycache__/`、生成的 `.dat/.b3d` 或编译产物上。