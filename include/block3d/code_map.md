# Public API Code Map

## `types.hpp` — 磁盘格式

- `FileHeader`：固定 64 字节的 `.b3d` 头。
- `MAGIC = "3DBK"`、`VERSION = 1`。
- `HEADER_SIZE`、`PAGE_ALIGN = 4096`。
- `aligned_data_offset(total_blocks)`：计算头和 `uint64_t` 索引表之后的数据区对齐位置。
- `str_axis()`：CLI 输出文件名和消息使用的轴字符串。

```text
FileHeader
block_offsets[total_blocks]  # absolute byte offsets
padding
block data
```

## `core.hpp` — 块几何与 Morton 顺序

### Storage classification and adaptive block sizing

- `StorageClass` 枚举：`HDD`、`SSD`、`NVMe`、`Unknown`。
- `detect_storage_medium(output_dir)`：在目标目录执行 5 轮 1MiB 写入+fsync 测试，取中位延迟对照阈值分类。若无有效数据则返回 `Unknown`。
- `auto_block_size(dim_x, dim_y, dim_z, medium)`：将维度排序，在最差轴切片触及的目标块数约束下，扫描 16–256（步长 8）寻找最小可用块大小。HDD 目标为 ≤400 块以减少寻道，SSD/NVMe 目标为 ≤2000 块以平衡传输效率。

### Morton helpers

- `spread_bits()` / `compact_bits()`
- `morton_encode(bx, by, bz)`
- `morton_decode(code)`

当前编码只保留每轴低 8 位。

### `BlockLayout3D`

构造时派生：

```text
blocks_axis = ceil(dim_axis / block_size)
total_blocks = blocks_x * blocks_y * blocks_z
block_floats = block_size^3
block_bytes = block_floats * 4
```

主要方法：

- `linear_index()`：逻辑块 X-Y-Z 展平。
- `global_to_block()`：全局坐标转块坐标与局部坐标。
- `block_origin()` / `block_extent()`：块范围和边界裁剪。
- `block_order()`：声明；实现位于 `../../src/core.cpp`。

## `rng.hpp` — 可复现测试随机数

`XorShift32` 提供 `next()`、`next_float()`、`rand_u64_mod()`，供 C++ 测试、验证和 benchmark 使用；不是安全随机数生成器。

## `converter.hpp` — 转换入口

```cpp
convert_raw_to_blocked(raw, output, dx, dy, dz,
                       block_size, num_threads,
                       progress, max_memory_mb)
```

只提供 `.dat -> .b3d`。`max_memory_mb` 是转换批次预算，不是进程硬上限。

## `reader.hpp` — 读取接口

### `MappedFile`

只读 mmap RAII 封装，可移动、不可复制，提供字节/float 视图和 `prefault()`。

### `BlockedFileReader`

- 切片：`read_x/y/z_slice()`、`read_slice()`、`read_slices_batch()`。
- 列：`read_x/y/z_column()` 及批量版本。
- 区域：`read_subvolume()`、`read_full_volume()`。
- 点与校验：`read_point()`、`verify()`。
- 缓存预热：`warm_up()`、`wait_warm_up()`。
- 元数据：尺寸、块大小、块总数、数据偏移和布局 getter。

输出布局：

```text
X slice -> result[y * dim_z + z]
Y slice -> result[x * dim_z + z]
Z slice -> result[x * dim_y + y]
subvolume -> local X-Y-Z order
```

内部 `block_offsets_` 将逻辑块映射到绝对文件偏移；`sorted_block_list()` 为同一轴块层缓存按物理偏移排序的块坐标副本。