# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 本目录职责

这里实现 C++ 库、普通 CLI 和大数据性能工具。公共契约在 `../include/block3d/`；文件和调用链导航见 `code_map.md`。

## 实现不变量

### 转换

- `converter.cpp` 必须把逻辑块偏移写入按 X-Y-Z 排列的索引表，同时按 Morton 顺序写块数据。
- 每个物理块长度固定为 `block_size^3 * sizeof(float)`；边界块先清零再复制有效区域。
- OpenMP 并行块提取，输出流保持顺序单线程写入。转换采用双缓冲流水线：batch N 写入的同时 batch N+1 由 `std::async` + OpenMP 异步提取，重叠 I/O 与 CPU。
- `max_memory_mb` 目前只约束块池批次；不要把它描述成完整进程硬限制。
- block_size 未显式指定时默认自适应选择：`detect_storage_medium()` 探测输出目录介质类型，`auto_block_size()` 根据维度+介质计算最优值（16–256，步长 8）。探测临时文件写入后立即清理。

### 读取

- `reader.cpp` 的索引偏移以整个文件起点为基准，单位是字节；读取前转换成 float 索引。
- 相交块按物理偏移排序，以减少离散文件访问。
- 同一块层的切片共享缓存键 `(axis, index / block_size)`。
- 缓存内容必须复制后在锁外使用；这是并发回归测试覆盖的安全边界。
- 切片读取现已内置切片内多线程（`for_each_block_parallel`）：当块数 > `num_threads × 4` 时，已排序的块列表跨线程分片处理，各线程安全写入输出缓冲区的不重叠区域。
- `read_slices_batch` 已简化为顺序派发——每次 `read_slice` 调用已在内部用完所有线程，顺序处理可避免超额订阅。
- 批量接口按请求并行，不是单切片内部块级并行。
- 维护 Windows/POSIX mmap 分支时，平台专属成员访问必须处于相应 `#ifdef` 中。

### CLI 与 benchmark

- `cli.cpp` 的 `verify` 参数顺序固定为 `<b3d> <raw>`。
- 顶层 `main()` 必须捕获库异常并以非零退出码报告，避免 Windows fast-fail。
- `block3d_cli bench` 只测读取；`run_test.cpp` 测读取、输出写盘和可选同步。修改输出时保持两种口径的区别清晰。
- `benchmark_cache.cpp` 只服务 benchmark 私有逻辑：cache mode 参数、scrub 文件创建/读取、页面大小/物理内存查询、phase 日志和 plan hash。不要把自动 scrub 或自动完整预热塞进 reader 构造函数。公有接口位于 `include/block3d/benchmark_cache.hpp`。
- `block3d_cli bench` 和 `run_test` 默认 `--cache-mode both`，先 cold 再 hot；`--warm-up` 保持旧脚本兼容，但语义是 hot-only。`--warm-up` 与 `--cache-mode cold/both` 冲突时必须报错。
- cold scrub 成功条件包括 scrub 文件存在、容量满足本轮 ratio、完整读取、逐页触碰、访问尾字节、checksum 输出和 settle 等待；失败时不输出 cold 成绩、不降级。
- cold/hot 阶段必须复用同一不可变请求计划并输出 `PLAN_HASH`；reader 在各 phase 重新创建，避免把 reader 内部缓存收益混入 OS 页缓存对比。
- `block3d_cli bench` 计时范围为 `read_only`，不写结果；`run_test` 计时范围为 `read_write_total`，主指标 `total_sec` 含切片读取、输出文件写入/关闭和可选 `_commit`/`fsync`。
- `run_test both` 必须使用独立 phase 目录（`cold_scrubbed/`、`hot_prefetched/` 或 `cold_first_touch/`），运行前估算输出空间，完成后逐文件校验 cold/hot 输出一致；日志默认保存到当前工作目录 `logs/` 且包含 stderr。
- `--warm-up` 标志（`bench` 和 `run_test` 均支持）在计时前将数据区预加载到 OS 页缓存；预热后的测量结果是热缓存数据。
- `convert` 的 `--block-size` 默认值为 0（自动检测）；显式 N 可覆盖。`--block-size 0` 与省略等效。验证区间为 0 或 16–256。
- `run_test` 除内置 `test18`/`test50` 外，支持自定义数据集：未识别的名称通过 `--dim-x/--dim-y/--dim-z` 指定维度，文件自动推导为 `{name}.dat` / `{name}.b3d`。

## 构建与验证

```bash
cmake --build build --config Release --target block3d_cli test_block3d test_cli run_test
ctest --test-dir build -C Release --output-on-failure
```

CLI 快速烟雾测试可使用小数据，不要为了普通代码修改运行 `test18/test50`：

```bash
build/Release/block3d_cli info FILE.b3d
build/Release/block3d_cli verify FILE.b3d RAW.dat --samples 1000
```

修改 `reader.cpp` 或 `converter.cpp` 后至少运行两个 CTest；修改并发缓存时必须运行完整 `test_block3d`。