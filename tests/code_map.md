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
| `test_batch_read` | 批量切片逐元素校验：同 layer、跨 layer、重复乱序、边界、空请求、错误输入、stream API、window 和重复 buffer 独立性 |
| `test_concurrent_adjacent_same_block_key` | 相邻切片共享缓存键、混合轴并发回归 |
| `test_reader_thread_pool_lifecycle` | 持久 reader 线程池 1/2/4/16 worker 与串行结果一致性、round-robin/contiguous dispatch 正确性、共享 reader 并发单片和 fused batch |
| `test_batch_column_read` | 三轴批量列 |
| `test_memory_budget` | 转换预算参数和 reader 元数据 |
| `test_micro_tiled_roundtrip` | v2 micro-tiled header/version/layout、点读、三轴切片、batch/stream、三轴列、子体积、完整读取、verify 和非法 micro size |

测试自行生成无头 X-Y-Z `float32` 数据，在系统临时目录创建 `.dat/.b3d` 后清理。

## `fixtures/dispatch_*.log`

小型静态 benchmark 日志夹具，用于 `tools/compare_dispatch_ab.py` 的 CTest smoke。夹具只验证解析、`PLAN_HASH` 校验和中位数决策逻辑；正式 Phase 4 结论仍必须来自真实 test18/test50 多轮日志。

## `test_cli.cpp`

进程级集成测试，不链接 `block3d` 库，而是查找并启动真实 `block3d_cli`：

```text
create small raw file
 -> block3d_cli convert legacy and micro-tiled v2
 -> block3d_cli info shows version/layout/micro size
 -> block3d_cli verify <b3d> <raw>
 -> invalid micro-tiled CLI options fail cleanly
 -> old/wrong argument order must fail cleanly
 -> missing raw must fail
 -> missing b3d must fail
 -> cache-prepare small scrub file
 -> bench both/hot-only cache mode smoke tests
 -> invalid/conflicting cache args fail cleanly
 -> cleanup
```

Windows 使用 `CreateProcessW`；POSIX 使用 `std::system`。测试重点是参数顺序、退出码和异常不能演变为 Windows `0xC0000409` fast-fail。

## CTest 映射

```text
test_block3d executable        -> CTest test_block3d
test_cli executable            -> CTest test_cli
tools/compare_dispatch_ab.py   -> CTest test_dispatch_compare_tool (when Python3 is available)
```

`run_test` 不在本目录，也不是 CTest；它是大数据性能工具。修改 `run_test` 的 cold/hot、输出目录或日志行为后，应单独运行 smoke benchmark；需要性能确认时再使用 `block3d-data/test18` 与 `block3d-cache/scrub.bin` 跑完整流程。Phase 5 正式 benchmark 的报告归档位于 `../experiments/批量融合读取与读写流水线优化开发方案实验数据归档/`，Phase 6 micro8 A/B 归档位于 `../experiments/phase6_20260717/`，均与本目录小型测试夹具分开维护。