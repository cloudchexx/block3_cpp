# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 本目录职责

这里是 `block3d` 的公共接口和持久化格式契约。实现位于 `../../src/`，测试位于 `../../tests/`。符号导航见 `code_map.md`。

## 修改约束

- `types.hpp` 的 `FileHeader` 是磁盘格式，不可像普通内存结构体一样随意调整。修改字段、大小、版本或对齐时，必须同步转换器、reader、CLI 信息输出、格式测试和根目录文档。
- 保持 `FileHeader` 为 64 字节；索引表紧随其后，数据区由 `aligned_data_offset()` 对齐到 4096 字节。
- `BlockLayout3D::linear_index()` 定义逻辑块索引顺序；改变它会改变索引表语义，必须与 writer 和 reader 一起迁移。
- 块内布局固定为局部 X-Y-Z，Z 连续。切片和列 API 的返回顺序是公共契约。
- `read_subvolume()` 使用半开区间 `[start, end)`。
- `MappedFile` 的 Windows 和 POSIX 状态成员不同；移动、关闭和映射实现必须分别受平台条件保护。
- `sorted_block_list()` 必须返回副本。缓存 mutex 释放后不能暴露 `unordered_map` 内部 vector 的引用。
- `detect_storage_medium()` 的输出仅作启发参考，不可硬编码为唯一策略。阈值需同时适配 HDD、SSD 和 NVMe。
- `auto_block_size()` 返回值区间为 [16, 256] 且为 4 的倍数。新增介质类型时须同步更新目标块数，并考虑维度乘积约束。
- 新增或修改公共 API 后，更新本目录 `code_map.md`，并在 `tests/test_block3d.cpp` 增加对应覆盖。

## 验证

```bash
cmake --build build --config Release --target test_block3d test_cli
ctest --test-dir build -C Release -R '^(test_block3d|test_cli)$' --output-on-failure
```

若修改磁盘格式，旧 `.b3d` 的兼容性必须被明确处理；不要仅靠现有样本文件判断正确性。