# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 本目录职责

这里实现 C++ 库、普通 CLI 和大数据性能工具。公共契约在 `../include/block3d/`；文件和调用链导航见 `code_map.md`。

## 实现不变量

### 转换

- `converter.cpp` 必须把逻辑块偏移写入按 X-Y-Z 排列的索引表，同时按 Morton 顺序写块数据。
- 每个物理块长度固定为 `block_size^3 * sizeof(float)`；边界块先清零再复制有效区域。
- OpenMP 只并行块提取，输出流保持顺序单线程写入。
- `max_memory_mb` 目前只约束块池批次；不要把它描述成完整进程硬限制。

### 读取

- `reader.cpp` 的索引偏移以整个文件起点为基准，单位是字节；读取前转换成 float 索引。
- 相交块按物理偏移排序，以减少离散文件访问。
- 同一块层的切片共享缓存键 `(axis, index / block_size)`。
- 缓存内容必须复制后在锁外使用；这是并发回归测试覆盖的安全边界。
- 批量接口按请求并行，不是单切片内部块级并行。
- 维护 Windows/POSIX mmap 分支时，平台专属成员访问必须处于相应 `#ifdef` 中。

### CLI 与 benchmark

- `cli.cpp` 的 `verify` 参数顺序固定为 `<b3d> <raw>`。
- 顶层 `main()` 必须捕获库异常并以非零退出码报告，避免 Windows fast-fail。
- `block3d_cli bench` 只测读取；`run_test.cpp` 测读取、输出写盘和可选同步。修改输出时保持两种口径的区别清晰。
- `run_test` 的性能数据受 OS 页缓存影响，不要把同进程多个轴/模式称为独立冷缓存测试。

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