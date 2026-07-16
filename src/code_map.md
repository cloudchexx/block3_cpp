# Source Code Map

## 库实现

### `core.cpp`

实现 `BlockLayout3D::block_order()`：枚举所有逻辑块，计算 Morton code，排序后返回 `(bx, by, bz)`。转换器使用该结果决定物理写入顺序。

### `converter.cpp`

- `extract_block()`：从无头 X-Y-Z 原始 mmap 中提取固定块；完整块逐行 `memcpy`，边界块补零。
- `convert_raw_to_blocked()`：

```text
MappedFile(raw)
 -> BlockLayout3D + Morton order
 -> write FileHeader
 -> reserve logical offset table
 -> align data area
 -> batch extract blocks (OpenMP when available)
 -> sequentially write Morton-ordered blocks
 -> backfill logical offset table
```

块提取可并行，文件写入串行。`max_memory_mb` 只参与批次大小计算。

### `reader.cpp`

#### `MappedFile`

- Windows：file handle + mapping handle + mapped view。
- POSIX：fd + `mmap`。
- `prefault()`：优先请求 OS 预取，失败时按 stride 触页。

#### `BlockedFileReader`

构造：读取头和索引表，再 mmap 整个 `.b3d`。

关键内部路径：

```text
read_*_slice()
 -> sorted_block_list(axis, index)
 -> logical block offset lookup
 -> sort by physical offset
 -> copy block fragments into contiguous result
```

其他路径：

- `read_slices_batch()`：round-robin 请求级多线程。
- `read_*_column()`：X/Y 跨步读取，Z 使用连续段复制。
- `read_subvolume()`：遍历相交块并复制连续 Z 段；区间为半开区间。
- `read_full_volume()`：完整范围的 `read_subvolume()`。
- `verify()`：固定随机坐标，多线程对照原始 `.dat`。
- `warm_up()`：同步或异步预热 OS 页缓存。

## 可执行文件

### `cli.cpp` -> `block3d_cli`

| 子命令 | 路径 |
|---|---|
| `convert` | `convert_raw_to_blocked()` |
| `info` | reader 元数据和存储比例 |
| `bench` | 三轴 `read_slices_batch()`，不写结果 |
| `verify` | `BlockedFileReader::verify()` |
| `extract` | 单切片或单列写无头 float32 |

所有子命令异常在 `main()` 统一转换为错误消息和退出码 2。

### `run_test.cpp` -> `run_test`

面向 `test18`、`test50` 的独立工具：

- 定位数据文件和复用有效 `.b3d`；
- 可选转换、随机点校验和 warm-up；
- 生成随机/连续切片请求；
- 读取并把每张切片写盘；
- 可选 `fflush` + `_commit/fsync`；
- 输出吞吐、平均时间、轴平衡、存储比例和日志。

其计时包含切片结果写盘，不包含 reader 构造和 mmap。