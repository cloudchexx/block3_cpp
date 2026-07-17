# Public API Code Map

## `types.hpp` — 磁盘格式

- `FileHeader`：固定 64 字节的 `.b3d` 头。
- `MAGIC = "3DBK"`、`VERSION_LEGACY = 1`、`VERSION_MICROTILE = 2`、`VERSION = VERSION_LEGACY`。
- `LAYOUT_LEGACY_XYZ = 0`、`LAYOUT_MICRO_TILED_XYZ = 1`、`DEFAULT_MICRO_SIZE = 8`。
- `BlockInnerLayout`：块内布局枚举，支持 legacy XYZ 和 v2 micro-tiled XYZ。
- `encode_layout()` / `header_layout_kind()` / `header_micro_size()`：在 `FileHeader::reserved` 中编码/解析 v2 layout metadata。
- `legacy_local_offset()` / `micro_tiled_local_offset()` / `local_offset_for_layout()`：布局感知块内 float offset helper。
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

编码使用 64-bit Morton code，每轴保留低 21 位。

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
ConvertOptions{
  block_size, num_threads, progress, max_memory_mb,
  inner_layout, micro_size
}
convert_raw_to_blocked(raw, output, dx, dy, dz, options)
convert_raw_to_blocked(raw, output, dx, dy, dz,
                       block_size, num_threads,
                       progress, max_memory_mb)  // legacy-compatible overload
```

只提供 `.dat -> .b3d`。默认仍生成 v1 legacy；显式 `inner_layout=MicroTiledXYZ` 且 `micro_size=8` 生成 v2 micro-tiled。Phase 6 A/B 归档显示 micro8 在当前 test18/test50 综合 run_test 计划下总体通过，但 X-heavy 负载仍可能退化，因此调用方应显式选择布局。`max_memory_mb` 是转换批次预算，不是进程硬上限。

## `reader.hpp` — 读取接口

### `MappedFile`

只读 mmap RAII 封装，可移动、不可复制，提供字节/float 视图和 `prefault()`。

### `BlockedFileReader`

- 切片：`read_x/y/z_slice()`、`read_slice()`、`read_slices_batch()`、`read_slices_batch_stream()`。批量入口先校验 axis 和整批 index，再按 layer/window 融合读取；兼容 API 按 `request_pos` 收集，保持请求顺序和重复语义。
- 块分发策略：`ReadDispatchStrategy::RoundRobin` / `Contiguous`，由 `BlockedFileReader` 构造参数固定；CLI 的 `--read-dispatch round-robin|contiguous` 用于 A/B 和回滚，不改变 `.b3d` 格式或输出语义。2026-07-17 Phase 5 归档证据位于 `../../experiments/批量融合读取与读写流水线优化开发方案实验数据归档/`，正式默认策略保持 round-robin。
- 列：`read_x/y/z_column()` 及批量版本。
- 区域：`read_subvolume()`、`read_full_volume()`。
- 点与校验：`read_point()`、`verify()`。
- 缓存预热：`warm_up()`、`wait_warm_up()`。
- reader 生命周期持久线程池：切片和 fused batch 的块级并行复用构造时创建的 worker；可按 reader 的 dispatch strategy 使用 round-robin 或 contiguous physical chunks，`options.num_threads` 作为单次调用 worker 上限。
- 元数据：尺寸、块大小、块总数、数据偏移、格式 `version()`、块内 `inner_layout()`、`micro_size()`、布局 getter，以及 `thread_pool_workers()` / `thread_pool_jobs()` / `thread_pool_serial_fallbacks()` 诊断 getter。

## `benchmark_cache.hpp` — 冷/热缓存 benchmark 接口

定义 `block3d_benchmark_cache` 库的公共接口，供 `block3d_cli bench` 和 `run_test` 共用：

### 缓存模式枚举

- `CacheMode`：`Cold` / `Hot` / `Both`
- `ColdMethod`：`Scrub` / `None`
- `ColdIsolation`：`Suite`（计划级一次 scrub）/ `Case`（每 case 前重新 scrub）
- `WarmupScope`：`Dataset`（预热整个数据区）/ `Workload`（预热计划工作集）

### 数据结构

- `CacheOptions`：benchmark 缓存阶段完整参数（mode、method、isolation、scope、scrub 文件路径、ratio、passes、settle 时间）。
- `CachePrepareOptions` / `CachePrepareResult`：scrub 文件一次性准备输入/输出。
- `CacheScrubResult`：scrub 执行结果（字节数、校验和、耗时、状态消息）。

### 辅助函数

- `to_string()` / `parse_*()`：枚举与字符串双向转换。
- `cache_mode_has_cold()` / `cache_mode_has_hot()`：`Both` 同时满足两者。
- `query_system_page_size()`、`query_physical_memory_bytes()`、`required_scrub_bytes()`：平台内存查询。
- `parse_size_bytes()`：`"2G"` / `"512M"` 风格容量解析。
- `fnv1a64_update()` / `fnv1a64_u64()`：校验和工具函数。
- `prepare_cache_scrub_file()`：用 `splitmix64` 伪随机数据创建可复用 scrub 文件。
- `run_cache_scrub()`：用普通 buffered I/O 逐页读取 scrub 文件，生成 cold_scrubbed 效果。

```text
benchmark_cache.hpp  ← 公有接口
src/benchmark_cache.cpp  ← 实现
```

输出布局：

```text
X slice -> result[y * dim_z + z]
Y slice -> result[x * dim_z + z]
Z slice -> result[x * dim_y + y]
subvolume -> local X-Y-Z order
```

内部 `block_offsets_` 将逻辑块映射到绝对文件偏移；`sorted_block_list()` 为同一轴块层缓存按物理偏移排序的块坐标副本。