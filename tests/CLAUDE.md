# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 测试结构

这里没有第三方测试框架：每个文件编译成一个自包含可执行文件，并由 CTest 注册。详细覆盖见 `code_map.md`。

- `test_block3d.cpp`：库功能、格式、批量读取和并发缓存回归。
- `test_cli.cpp`：启动同一构建输出目录中的真实 `block3d_cli`，测试参数解析、退出码和异常边界。
- `test_dispatch_compare_tool`：Python3 可用时注册，使用静态日志夹具 smoke 测试 Phase 4 dispatch A/B 比较脚本。

## 运行方式

全部测试：

```bash
ctest --test-dir build -C Release --output-on-failure
```

单个 CTest：

```bash
ctest --test-dir build -C Release -R '^test_block3d$' --output-on-failure
ctest --test-dir build -C Release -R '^test_cli$' --output-on-failure
ctest --test-dir build -C Release -R '^test_dispatch_compare_tool$' --output-on-failure
```

当前 `test_block3d` 内部没有测试名称过滤器；要运行其中一个函数，需要临时改入口或新增正式过滤能力，不能把 CTest `-R` 当成内部函数筛选。

## 修改测试时

- 测试数据保持足够小，使用系统临时目录并清理输出，不依赖根目录的大型 `.dat/.b3d`。
- 格式或布局变更必须覆盖完整 roundtrip、三轴切片、批量/stream 批量切片、列、子体积、点读、verify、存储比例和错误路径；v2 micro-tiled 还要断言 header version/layout/micro size。
- reader 缓存或批量并发变更必须保留并扩展 `test_concurrent_adjacent_same_block_key`；该测试防止缓存内部引用失效造成 use-after-free。
- CLI 参数、cache-prepare、bench 缓存阶段输出、文件缺失、坏 magic 或异常处理变更应放到 `test_cli.cpp`，通过真实进程和退出码验证。
- `run_test` 是大数据性能工具，不纳入默认 CTest；修改其真实大数据路径后，需要单独用 `block3d-data/test18` 和 `block3d-cache/scrub.bin` 跑 smoke 或完整 benchmark。
- Phase 5 正式 benchmark 的报告归档在 `../experiments/批量融合读取与读写流水线优化开发方案实验数据归档/`，Phase 6 micro8 A/B 归档在 `../experiments/phase6_20260717/`；它们不是测试夹具，不要把其中大日志当成 CTest 输入，CTest 仍使用 `fixtures/dispatch_*.log` 小夹具。
- `test_cli` 期望 `block3d_cli` 位于测试可执行文件旁。若调整 CMake 输出目录或目标依赖，同步修正定位逻辑。
- Windows 进程测试使用 `CreateProcessW`，不要退回易受多层引号影响的 `cmd.exe` 拼接。

修改测试文件后至少重新构建对应测试目标；CLI 测试还要确保 `block3d_cli` 已构建。