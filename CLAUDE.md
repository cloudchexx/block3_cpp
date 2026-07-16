# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 注意：

1.使用jujutsu进行项目管理，你拥有对应的skill

2.注意windows powershell命令格式，你拥有对应的skill

## 文档分层

本文件只记录跨模块约束和常用命令。进入具体目录后，优先阅读该目录自己的 `CLAUDE.md` 和 `code_map.md`：

- `include/block3d/`：公共 API、文件格式和布局类型
- `src/`：转换、读取、CLI 和大数据基准实现
- `tests/`：库测试及 CLI 进程集成测试
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

## 修改后的验证范围

- 修改格式、布局、转换或 reader：运行 `test_block3d` 和 `test_cli`。
- 修改 CLI 参数或错误处理：至少运行 `test_cli`。
- 修改并发缓存逻辑：必须覆盖 `test_concurrent_adjacent_same_block_key` 所在的完整 `test_block3d`。
- 修改性能路径时，不要用 `run_test` 替代正确性测试；大数据测试需要本地数据集和大量磁盘空间。

不要编辑或把上下文建立在 `build/`、`logs/`、`generator_py/__pycache__/`、生成的 `.dat/.b3d` 或编译产物上。