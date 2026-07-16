# Code Map

根目录只提供全局导航；模块细节已下沉到各目录的 `code_map.md`。

## 模块导航

| 区域 | 职责 | 详细地图 |
|---|---|---|
| `include/block3d/` | `.b3d` 格式、Morton/块布局、转换与读取公共 API | [`include/block3d/code_map.md`](include/block3d/code_map.md) |
| `src/` | 块转换、mmap reader、CLI、大数据测试实现 | [`src/code_map.md`](src/code_map.md) |
| `tests/` | 库功能、并发回归和真实 CLI 进程测试 | [`tests/code_map.md`](tests/code_map.md) |
| `generator_py/` | 随机三维数据生成器及 CLI/GUI/TUI | [`generator_py/code_map.md`](generator_py/code_map.md) |
| `CMakeLists.txt` | C++17、OpenMP 和五个构建目标 | 本文“构建目标” |
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
| `block3d_cli` | `convert/info/bench/verify/extract` CLI |
| `test_block3d` | 库级测试可执行文件 |
| `test_cli` | 启动真实 CLI 的集成测试 |
| `run_test` | 大数据转换、写盘和性能工具；不属于 CTest |

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 关键跨模块约束

- `block3d_cli bench` 只计读取；`run_test` 计读取与输出写盘/同步。
- `max_memory_mb` 是局部软预算，不是整个进程内存上限。
- reader 的共享块列表缓存必须在线程锁外使用副本。
- 性能结果必须结合 OS 页缓存状态解释。
- Morton 位扩展当前只保留每轴低 8 位；每轴块数超过 256 时编码可能碰撞。