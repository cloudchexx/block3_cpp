# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 测试结构

这里没有第三方测试框架：每个文件编译成一个自包含可执行文件，并由 CTest 注册。详细覆盖见 `code_map.md`。

- `test_block3d.cpp`：库功能、格式、批量读取和并发缓存回归。
- `test_cli.cpp`：启动同一构建输出目录中的真实 `block3d_cli`，测试参数解析、退出码和异常边界。

## 运行方式

全部测试：

```bash
ctest --test-dir build -C Release --output-on-failure
```

单个 CTest：

```bash
ctest --test-dir build -C Release -R '^test_block3d$' --output-on-failure
ctest --test-dir build -C Release -R '^test_cli$' --output-on-failure
```

当前 `test_block3d` 内部没有测试名称过滤器；要运行其中一个函数，需要临时改入口或新增正式过滤能力，不能把 CTest `-R` 当成内部函数筛选。

## 修改测试时

- 测试数据保持足够小，使用系统临时目录并清理输出，不依赖根目录的大型 `.dat/.b3d`。
- 格式或布局变更必须覆盖完整 roundtrip、三轴切片、列、子体积和存储比例。
- reader 缓存或批量并发变更必须保留并扩展 `test_concurrent_adjacent_same_block_key`；该测试防止缓存内部引用失效造成 use-after-free。
- CLI 参数、文件缺失、坏 magic 或异常处理变更应放到 `test_cli.cpp`，通过真实进程和退出码验证。
- `test_cli` 期望 `block3d_cli` 位于测试可执行文件旁。若调整 CMake 输出目录或目标依赖，同步修正定位逻辑。
- Windows 进程测试使用 `CreateProcessW`，不要退回易受多层引号影响的 `cmd.exe` 拼接。

修改测试文件后至少重新构建对应测试目标；CLI 测试还要确保 `block3d_cli` 已构建。