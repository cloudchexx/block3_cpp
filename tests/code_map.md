# Test Code Map

## `test_block3d.cpp`

自包含的库级测试入口，覆盖：

| 测试 | 覆盖内容 |
|---|---|
| `test_morton` | Morton encode/decode 往返 |
| `test_block_layout` | 块数、块大小和总块数 |
| `test_block_order` | Morton 排序 |
| `test_roundtrip` | `.dat -> .b3d -> full volume` |
| `test_slice_reads` | X/Y/Z 切片及输出顺序 |
| `test_column_reads` | X/Y/Z 单列 |
| `test_subvolume` | 半开区间子体积 |
| `test_storage_ratio` | 小数据格式开销 `< 1.5x` |
| `test_verify` | 随机点原始文件校验 |
| `test_batch_read` | 批量切片 |
| `test_concurrent_adjacent_same_block_key` | 相邻切片共享缓存键、混合轴并发回归 |
| `test_batch_column_read` | 三轴批量列 |
| `test_memory_budget` | 转换预算参数和 reader 元数据 |

测试自行生成无头 X-Y-Z `float32` 数据，在系统临时目录创建 `.dat/.b3d` 后清理。

## `test_cli.cpp`

进程级集成测试，不链接 `block3d` 库，而是查找并启动真实 `block3d_cli`：

```text
create small raw file
 -> block3d_cli convert
 -> block3d_cli verify <b3d> <raw>
 -> old/wrong argument order must fail cleanly
 -> missing raw must fail
 -> missing b3d must fail
 -> cleanup
```

Windows 使用 `CreateProcessW`；POSIX 使用 `std::system`。测试重点是参数顺序、退出码和异常不能演变为 Windows `0xC0000409` fast-fail。

## CTest 映射

```text
test_block3d executable -> CTest test_block3d
test_cli executable     -> CTest test_cli
```

`run_test` 不在本目录，也不是 CTest；它是大数据性能工具。