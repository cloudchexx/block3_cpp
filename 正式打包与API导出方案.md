# 正式打包与 API 导出方案（审核修订版）

> 本文只规划“正式打包 + 公共 API + API 测试脚本”。本阶段先不考虑比赛现场调参和性能冲榜，也不重写核心算法。后续实现时应只在现有转换器、`BlockedFileReader` 和 CMake 外层增加稳定封装。

---

## 0. 审核结论

原方案的大方向是正确的：项目应该交付 C、C++、Python 三类可复用接口，并提供安装、示例和 smoke test。但原方案在“真实项目接口”上不够贴合当前代码，主要问题如下：

1. **`query_box` 不是当前项目的核心接口。**
   - 当前核心能力是 `.dat -> .b3d` 转换，以及从 `.b3d` 读取三轴切片、批量切片、三轴列、子体积、单点、全体积和随机点校验。
   - `read_subvolume()` 才接近“盒查询”，但项目需求最核心是 X/Y/Z 任意切片和 X 主维单列，不应把第一版 API 设计成只有 `query_box`。

2. **`block3d_result { void* data; size_t size; int value_type; }` 太模糊。**
   - 本项目数据类型固定为 `float32`。
   - 切片、列、子体积结果都有明确 shape 和输出布局。
   - C API 必须显式表达：维度、元素个数、结果类型、所有权、释放函数。

3. **打开对象应是 `.b3d` 文件，不是数据集目录。**
   - 当前 `BlockedFileReader` 构造函数打开的是已转换的 `.b3d`。
   - 原始 `.dat` 只在转换和 `verify(raw)` 时使用。

4. **第一版公共 API 不能只暴露读取，还必须暴露转换。**
   - `项目需求文档.md` 要求实现原始浮点数组与优化存储格式数据的转换。
   - 第一版至少要公开 `convert_raw_to_b3d()`，否则打包后的算法库无法独立完成“原始数据 -> 优化格式”。

5. **Python API 不应照搬旧方案的 `QueryEngine.query_box()`。**
   - Python 第一版应围绕 `Reader` / `BlockedFile` 暴露：`read_slice()`、`read_slices()`、`read_column()`、`read_subvolume()`、`read_point()`、`verify()`、`convert_raw_to_b3d()`。
   - 切片建议返回 NumPy `ndarray(float32)`，否则大切片若返回 Python list 会产生巨大对象开销。

6. **CMake 当前没有 install/export/find_package。**
   - 当前只构建 `block3d`、`block3d_benchmark_cache`、`block3d_cli`、`test_block3d`、`test_cli`、`run_test`。
   - 正式打包必须补齐安装 public headers、库目标、`block3dConfig.cmake`、`block3dTargets.cmake`、示例和包外消费测试。

7. **API 测试应借鉴 `run_test` 的请求计划，但不要混同 benchmark。**
   - `run_test` 是大数据性能工具，计时口径是 `read_write_total`，会写出切片文件，支持 cold/hot 缓存和 `PLAN_HASH`。
   - API smoke test 应快速、确定性、验证接口可用和结果正确；可另写“run_test-like packaged API 脚本”用于 test18/test50 级别功能验收。

---

## 1. 当前项目真实接口盘点

### 1.1 现有 C++ 低层公共头

当前 `include/block3d/` 已有以下头文件：

```text
core.hpp              块几何、Morton 顺序、存储介质检测、自适应 block_size
converter.hpp         raw float32 .dat -> .b3d 转换
reader.hpp            .b3d mmap reader，切片/列/子体积/点/批量/verify/warm_up
types.hpp             .b3d 磁盘格式、版本、layout metadata、块内 offset helper
benchmark_cache.hpp   cold/hot benchmark 私有支持，供 CLI bench 和 run_test 使用
rng.hpp               测试和 benchmark 用确定性随机数
```

这些头目前已经可被 C++ 项目包含，但它们偏“项目内部公共头”，还不是面向下游用户的稳定 facade。正式打包建议保留它们供高级用户使用，同时新增更稳定的：

```text
include/block3d/block3d.h      C ABI
include/block3d/block3d.hpp    C++ facade
include/block3d/version.h      版本宏
```

### 1.2 当前转换能力

已有入口：

```cpp
block3d::ConvertOptions options;
options.block_size = 32;                // 或后续 facade 中允许 0 表示自动
options.num_threads = 0;                // 0 = auto
options.progress = true;
options.max_memory_mb = 0;              // 当前是转换批次软预算，不是进程硬上限
options.inner_layout = block3d::BlockInnerLayout::LegacyXYZ;
options.micro_size = 0;

block3d::convert_raw_to_blocked(
    raw_path,
    b3d_path,
    dim_x,
    dim_y,
    dim_z,
    options
);
```

重要语义：

- 原始 `.dat` 是无头 `float32`，X-Y-Z 顺序，Z 连续。
- `.b3d` 写入固定 64 字节 header、逻辑块 offset table、4096 对齐数据区。
- 当前支持 v1 legacy XYZ 块内布局和 v2 micro-tiled XYZ 布局。
- CLI 默认转换仍是 legacy；micro-tiled 必须显式选择。
- `max_memory_mb` 当前是局部软预算，不是完整进程内存硬限制。

### 1.3 当前读取能力

已有核心类：

```cpp
block3d::BlockedFileReader reader(
    b3d_path,
    num_threads,
    max_memory_mb,
    block3d::ReadDispatchStrategy::RoundRobin
);
```

主要能力：

```cpp
// 单切片
std::vector<float> read_x_slice(uint64_t x); // shape = [dim_y, dim_z]
std::vector<float> read_y_slice(uint64_t y); // shape = [dim_x, dim_z]
std::vector<float> read_z_slice(uint64_t z); // shape = [dim_x, dim_y]
std::vector<float> read_slice(char axis, uint64_t index);

// 批量切片
std::vector<std::vector<float>> read_slices_batch(
    char axis,
    const std::vector<uint64_t>& indices,
    int num_threads = 0
);

void read_slices_batch_stream(
    char axis,
    const std::vector<uint64_t>& indices,
    const block3d::SliceBatchOptions& options,
    const block3d::SliceConsumer& consumer
);

// 三轴列
std::vector<float> read_x_column(uint64_t y, uint64_t z); // length = dim_x
std::vector<float> read_y_column(uint64_t x, uint64_t z); // length = dim_y
std::vector<float> read_z_column(uint64_t x, uint64_t y); // length = dim_z

// 子体积，半开区间 [start, end)
std::vector<float> read_subvolume(
    uint64_t xs, uint64_t xe,
    uint64_t ys, uint64_t ye,
    uint64_t zs, uint64_t ze
); // shape = [xe-xs, ye-ys, ze-zs]，局部 X-Y-Z 顺序

// 其它
std::vector<float> read_full_volume();
float read_point(uint64_t x, uint64_t y, uint64_t z);
bool verify(const std::string& raw_path, uint64_t samples, float tol = 1e-3f);
void warm_up(bool async = true, size_t stride = 4096, uint64_t max_memory_mb = 0);
```

输出布局是公共契约：

```text
X slice -> result[y * dim_z + z]
Y slice -> result[x * dim_z + z]
Z slice -> result[x * dim_y + y]
subvolume -> local X-Y-Z order: ((x-xs) * ny * nz + (y-ys) * nz + (z-zs))
```

### 1.4 当前 CMake 状态

当前根 `CMakeLists.txt` 主要目标：

```text
block3d                    src/core.cpp + src/reader.cpp + src/converter.cpp
block3d_benchmark_cache    src/benchmark_cache.cpp
block3d_cli                CLI: convert/info/cache-prepare/bench/verify/extract
test_block3d               库级 CTest
test_cli                   CLI 进程级 CTest
run_test                   大数据性能工具，不属于 CTest
```

当前缺口：

```text
没有 C API 目标/头
没有 C++ facade 头
没有 install(TARGETS ...)
没有 install(EXPORT ...)
没有 block3dConfig.cmake / block3dConfigVersion.cmake
没有 find_package(block3d CONFIG REQUIRED) 包外验证
没有 Python/pybind11 模块
没有 Python 包构建配置
```

---

## 2. `run_test` 对 API 规划的启发

`run_test` 不应被改造成公共 API，也不应作为默认 CTest。它的价值是告诉我们正式接口和脚本应该覆盖哪些真实使用场景。

### 2.1 `run_test` 的关键行为

默认配置近似为：

```text
数据集：test18、test50
请求轴：x/y/z/all
请求模式：random/sequential/all
random-count：100
seq-count：10
seed：42
seq-start：0
batch-read：fused
pipeline：on
pipeline-memory：256 MiB payload
output-sync：requested
read-dispatch：round-robin
cache-mode：both
```

它做的事情：

1. 定位 raw `.dat` 和 `.b3d`。
2. 如果 `.b3d` 不存在或不匹配，则转换。
3. 可选 `--verify` 随机点校验 raw 与 `.b3d`。
4. 构造确定性的切片请求计划：
   - 三轴随机切片。
   - 三轴连续切片。
5. 输出 `PLAN_HASH`，确保 cold/hot 使用同一请求计划。
6. 用 `read_slices_batch_stream()` 读取切片。
7. 将每个切片写成无头 float32 `.raw`。
8. 记录 `BENCHMARK_RESULT`：`total_sec`、`read_sec`、`write_sec`、吞吐、输出字节数等。
9. both 模式下比较 cold/hot 输出文件一致性。

### 2.2 对公共 API 的要求

为了让下游用户不用依赖 `run_test.cpp` 也能复现核心功能，正式 API 至少要支持：

```text
1. raw -> b3d 转换
2. 打开 b3d 并读取元数据
3. 三轴单切片读取
4. 三轴批量切片读取（run_test 的核心读取路径）
5. 三轴列读取，尤其 X 主维单列
6. 子体积读取（盒形查询）
7. 单点读取和 raw 随机点校验
8. 明确的结果 shape 和输出布局
9. 线程数、软内存预算、read_dispatch 等选项
10. 错误码 / 异常边界清晰
```

### 2.3 API 测试脚本与 `run_test` 的边界

后续新增的 C++ / Python 测试脚本应“借鉴 `run_test` 的请求计划”，但不承担冷/热缓存正式 benchmark：

```text
API smoke test:
  目标：接口可用、结果正确、可安装后调用
  数据：小型临时数据为主，可选 test18/test50
  计时：不是主要目标
  输出：可写少量 .raw 检查文件格式

run_test:
  目标：大数据读写性能、冷/热缓存、输出同步、日志、PLAN_HASH
  数据：test18/test50
  计时：read_write_total
  输出：大量 .raw 和 benchmark 日志
```

---

## 3. 正式 API 设计决策

### 3.1 第一版 API 范围

第一版不暴露内部优化细节，只暴露当前需求和测试脚本必需能力：

```text
convert_raw_to_b3d
open_b3d / close
info
read_slice
read_slices_batch
read_column
read_subvolume
read_point
verify_points
free_result
```

暂不作为第一版公共 API 的内容：

```text
cold/hot cache scrub
run_test pipeline writer
benchmark 日志格式
自动正式性能 A/B
内部块 offset table
内部 Morton block list
内部线程池诊断计数（可在 C++ 低层 reader 保留）
```

### 3.2 坐标和范围约定

公共文档必须固定以下语义：

```text
坐标从 0 开始。
单切片 index 必须满足 0 <= index < dim_axis。
子体积范围使用半开区间 [start, end)。
子体积必须满足 start < end 且 end <= dim_axis。
越界返回错误 / 抛出异常，不做静默裁剪。
所有数值为 float32。
```

### 3.3 结果布局约定

```text
Axis X slice:
  shape = (dim_y, dim_z)
  flat offset = y * dim_z + z

Axis Y slice:
  shape = (dim_x, dim_z)
  flat offset = x * dim_z + z

Axis Z slice:
  shape = (dim_x, dim_y)
  flat offset = x * dim_y + y

X column:
  input fixed (y, z)
  shape = (dim_x)
  offset = x

Y column:
  input fixed (x, z)
  shape = (dim_y)
  offset = y

Z column:
  input fixed (x, y)
  shape = (dim_z)
  offset = z

Subvolume:
  range = [xs, xe) × [ys, ye) × [zs, ze)
  shape = (xe-xs, ye-ys, ze-zs)
  flat offset = (x-xs) * ny * nz + (y-ys) * nz + (z-zs)
```

---

## 4. C API 规划

### 4.1 目标

C API 是 ABI 边界，面向 C 项目、其它语言绑定和最稳定的库调用方式。要求：

1. 不暴露 C++ 类。
2. 不让异常穿过 C ABI。
3. 所有函数返回明确 `block3d_status`。
4. 所有输出参数失败时可安全释放。
5. 所有返回数组都由库分配，由 `block3d_free_array()` / `block3d_free_batch()` 释放。
6. API 明确 shape、元素数、结果类型和布局。

### 4.2 建议头文件：`include/block3d/block3d.h`

```c
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct block3d_context block3d_context;

typedef enum block3d_status {
    BLOCK3D_OK = 0,
    BLOCK3D_ERROR_INVALID_ARGUMENT = 1,
    BLOCK3D_ERROR_IO = 2,
    BLOCK3D_ERROR_OUT_OF_MEMORY = 3,
    BLOCK3D_ERROR_OUT_OF_RANGE = 4,
    BLOCK3D_ERROR_FORMAT = 5,
    BLOCK3D_ERROR_INTERNAL = 6
} block3d_status;

typedef enum block3d_axis {
    BLOCK3D_AXIS_X = 0,
    BLOCK3D_AXIS_Y = 1,
    BLOCK3D_AXIS_Z = 2
} block3d_axis;

typedef enum block3d_layout {
    BLOCK3D_LAYOUT_LEGACY_XYZ = 0,
    BLOCK3D_LAYOUT_MICRO_TILED_XYZ = 1
} block3d_layout;

typedef enum block3d_read_dispatch {
    BLOCK3D_READ_DISPATCH_ROUND_ROBIN = 0,
    BLOCK3D_READ_DISPATCH_CONTIGUOUS = 1
} block3d_read_dispatch;

typedef struct block3d_version {
    int major;
    int minor;
    int patch;
} block3d_version;

typedef struct block3d_convert_options {
    uint64_t block_size;      /* 0 = auto; otherwise 16..256 */
    int num_threads;          /* 0 = auto */
    uint64_t max_memory_mb;   /* soft batch budget */
    block3d_layout layout;    /* first version defaults to legacy */
    uint32_t micro_size;      /* 0 for legacy, 8 for micro-tiled */
    int progress;             /* non-zero enables progress output */
} block3d_convert_options;

typedef struct block3d_reader_options {
    int num_threads;          /* 0 = auto */
    uint64_t max_memory_mb;   /* soft budget */
    block3d_read_dispatch read_dispatch;
} block3d_reader_options;

typedef struct block3d_file_info {
    uint64_t dim_x;
    uint64_t dim_y;
    uint64_t dim_z;
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t data_offset;
    uint32_t format_version;
    block3d_layout layout;
    uint32_t micro_size;
} block3d_file_info;

typedef struct block3d_array {
    float* data;
    uint64_t dim0;
    uint64_t dim1;
    uint64_t dim2;
    size_t ndim;
    uint64_t count;
} block3d_array;

typedef struct block3d_slice_batch {
    float* data;              /* contiguous: slice_count * slice_elems */
    uint64_t* indices;        /* copy of requested indices */
    uint64_t slice_count;
    uint64_t slice_elems;
    uint64_t dim0;
    uint64_t dim1;
    block3d_axis axis;
} block3d_slice_batch;

block3d_version block3d_get_version(void);
const char* block3d_status_message(block3d_status status);

block3d_convert_options block3d_default_convert_options(void);
block3d_reader_options block3d_default_reader_options(void);

block3d_status block3d_convert_raw_to_b3d(
    const char* raw_path,
    const char* b3d_path,
    uint64_t dim_x,
    uint64_t dim_y,
    uint64_t dim_z,
    const block3d_convert_options* options
);

block3d_status block3d_open_b3d(
    const char* b3d_path,
    const block3d_reader_options* options,
    block3d_context** out_context
);

void block3d_close(block3d_context* context);

block3d_status block3d_get_info(
    const block3d_context* context,
    block3d_file_info* out_info
);

block3d_status block3d_read_slice(
    block3d_context* context,
    block3d_axis axis,
    uint64_t index,
    block3d_array* out_array
);

block3d_status block3d_read_slices_batch(
    block3d_context* context,
    block3d_axis axis,
    const uint64_t* indices,
    uint64_t index_count,
    block3d_slice_batch* out_batch
);

block3d_status block3d_read_column(
    block3d_context* context,
    block3d_axis axis,
    uint64_t coord1,
    uint64_t coord2,
    block3d_array* out_array
);

block3d_status block3d_read_subvolume(
    block3d_context* context,
    uint64_t xs,
    uint64_t xe,
    uint64_t ys,
    uint64_t ye,
    uint64_t zs,
    uint64_t ze,
    block3d_array* out_array
);

block3d_status block3d_read_point(
    block3d_context* context,
    uint64_t x,
    uint64_t y,
    uint64_t z,
    float* out_value
);

block3d_status block3d_verify_points(
    block3d_context* context,
    const char* raw_path,
    uint64_t samples,
    float tolerance
);

void block3d_free_array(block3d_array* array);
void block3d_free_slice_batch(block3d_slice_batch* batch);

#ifdef __cplusplus
}
#endif
```

### 4.3 C API 到当前实现的映射

```text
block3d_convert_raw_to_b3d
  -> block3d::convert_raw_to_blocked()

block3d_open_b3d
  -> new block3d::BlockedFileReader(...)

block3d_get_info
  -> reader.dim_x(), dim_y(), dim_z(), block_size(), total_blocks(), version(), inner_layout(), micro_size()

block3d_read_slice
  -> reader.read_slice(axis, index)

block3d_read_slices_batch
  -> reader.read_slices_batch(axis, indices, options.num_threads)
     然后复制/移动成连续 batch buffer

block3d_read_column
  -> axis=x: reader.read_x_column(coord1=y, coord2=z)
     axis=y: reader.read_y_column(coord1=x, coord2=z)
     axis=z: reader.read_z_column(coord1=x, coord2=y)

block3d_read_subvolume
  -> reader.read_subvolume(xs, xe, ys, ye, zs, ze)

block3d_read_point
  -> reader.read_point(x, y, z)

block3d_verify_points
  -> reader.verify(raw_path, samples, tolerance)
```

---

## 5. C++ facade API 规划

### 5.1 目标

C++ facade 面向下游 C++ 用户，提供更自然的 RAII、异常和 `std::vector<float>` 结果。它可以直接包现有 `BlockedFileReader`，不必绕一层 C API。

建议文件：

```text
include/block3d/block3d.hpp
src/block3d_cpp.cpp        可选；若全部 inline/PImpl 可不单独增加
```

### 5.2 建议接口

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace block3d {

struct Version {
    int major;
    int minor;
    int patch;
};

enum class Axis { X, Y, Z };
enum class Layout { LegacyXYZ, MicroTiledXYZ };
enum class ReadDispatch { RoundRobin, Contiguous };

struct ConvertOptionsFacade {
    std::uint64_t block_size = 0;       // 0 = auto
    int num_threads = 0;                // 0 = auto
    std::uint64_t max_memory_mb = 0;    // soft budget
    Layout layout = Layout::LegacyXYZ;
    std::uint32_t micro_size = 0;
    bool progress = true;
};

struct ReaderOptionsFacade {
    int num_threads = 0;
    std::uint64_t max_memory_mb = 0;
    ReadDispatch read_dispatch = ReadDispatch::RoundRobin;
};

struct FileInfo {
    std::uint64_t dim_x;
    std::uint64_t dim_y;
    std::uint64_t dim_z;
    std::uint64_t block_size;
    std::uint64_t total_blocks;
    std::uint32_t format_version;
    Layout layout;
    std::uint32_t micro_size;
};

struct Array {
    std::vector<float> data;
    std::vector<std::uint64_t> shape;
};

struct SliceBatch {
    std::vector<float> data;            // contiguous batch
    std::vector<std::uint64_t> indices;
    Axis axis;
    std::uint64_t slice_count;
    std::uint64_t slice_elems;
    std::vector<std::uint64_t> slice_shape;
};

Version version();

void convert_raw_to_b3d(
    const std::string& raw_path,
    const std::string& b3d_path,
    std::uint64_t dim_x,
    std::uint64_t dim_y,
    std::uint64_t dim_z,
    const ConvertOptionsFacade& options = {}
);

class Reader {
public:
    explicit Reader(const std::string& b3d_path,
                    const ReaderOptionsFacade& options = {});
    ~Reader();

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;

    FileInfo info() const;

    Array read_slice(Axis axis, std::uint64_t index) const;
    SliceBatch read_slices(Axis axis, const std::vector<std::uint64_t>& indices) const;
    Array read_column(Axis axis, std::uint64_t coord1, std::uint64_t coord2) const;
    Array read_subvolume(std::uint64_t xs, std::uint64_t xe,
                         std::uint64_t ys, std::uint64_t ye,
                         std::uint64_t zs, std::uint64_t ze) const;
    float read_point(std::uint64_t x, std::uint64_t y, std::uint64_t z) const;
    bool verify(const std::string& raw_path,
                std::uint64_t samples = 1000,
                float tolerance = 1e-3f) const;

private:
    class Impl;
    Impl* impl_;
};

} // namespace block3d
```

### 5.3 为什么不继续使用 `Dataset + QueryEngine + query_box`

正式第一版更适合用 `Reader`，原因：

1. 当前实际打开的是 `.b3d` 文件，不是数据集目录。
2. 需求核心是切片和列，不是抽象 query box。
3. `run_test` 的核心路径是批量切片读取，不是单个 box 查询。
4. `read_subvolume()` 已可作为 box 查询能力暴露，不需要把它命名为唯一主接口。

---

## 6. Python API 规划

### 6.1 技术选择

建议使用 `pybind11`。Python 第一版建议直接依赖 NumPy：

```text
pybind11 + numpy
```

理由：

1. 切片结果可能几十 MB，返回 Python list 不合适。
2. NumPy `ndarray(float32)` 能自然表达 shape、dtype 和 C-contiguous 布局。
3. 下游验证、写 raw、分析都更方便。

### 6.2 建议 Python 接口

```python
import block3d

block3d.__version__
block3d.convert_raw_to_b3d(...)
block3d.Reader
block3d.Reader.info()
block3d.Reader.read_slice(axis, index)
block3d.Reader.read_slices(axis, indices)
block3d.Reader.read_column(axis, coord1, coord2)
block3d.Reader.read_subvolume(xs, xe, ys, ye, zs, ze)
block3d.Reader.read_point(x, y, z)
block3d.Reader.verify(raw_path, samples=1000, tolerance=1e-3)
```

示例：

```python
import block3d

block3d.convert_raw_to_b3d(
    raw_path="Z:/wutan/block3d-data/test18/test18.dat",
    b3d_path="Z:/wutan/block3d-data/test18/test18.b3d",
    dim_x=801,
    dim_y=2405,
    dim_z=2501,
    block_size=0,
    layout="legacy",
)

reader = block3d.Reader(
    "Z:/wutan/block3d-data/test18/test18.b3d",
    threads=0,
    memory_limit_mb=0,
    read_dispatch="round-robin",
)

info = reader.info()
print(info)

x0 = reader.read_slice("x", 0)          # numpy.ndarray, shape=(dim_y, dim_z), dtype=float32
y0 = reader.read_slice("y", 0)          # shape=(dim_x, dim_z)
z0 = reader.read_slice("z", 0)          # shape=(dim_x, dim_y)
xs = reader.read_slices("x", [0, 1, 2]) # shape=(3, dim_y, dim_z)
col = reader.read_column("x", 0, 0)     # shape=(dim_x,)
sub = reader.read_subvolume(0, 4, 0, 5, 0, 6) # shape=(4, 5, 6)
value = reader.read_point(0, 0, 0)
```

### 6.3 Python 返回值 shape

```text
read_slice("x", i) -> ndarray shape (dim_y, dim_z)
read_slice("y", i) -> ndarray shape (dim_x, dim_z)
read_slice("z", i) -> ndarray shape (dim_x, dim_y)

read_slices("x", [..]) -> ndarray shape (n, dim_y, dim_z)
read_slices("y", [..]) -> ndarray shape (n, dim_x, dim_z)
read_slices("z", [..]) -> ndarray shape (n, dim_x, dim_y)

read_column("x", y, z) -> ndarray shape (dim_x,)
read_column("y", x, z) -> ndarray shape (dim_y,)
read_column("z", x, y) -> ndarray shape (dim_z,)

read_subvolume(xs, xe, ys, ye, zs, ze) -> ndarray shape (xe-xs, ye-ys, ze-zs)
```

### 6.4 Python 构建方式

建议先做 CMake 内 pybind module，再补 `scikit-build-core` wheel：

```text
python/
  pybind_module.cpp
  pyproject.toml
  README.md
```

CMake 选项：

```cmake
option(BLOCK3D_BUILD_PYTHON "Build block3d Python module" OFF)
```

第一阶段验收：

```bash
cmake -S Z:/wutan/block3d-cpp -B Z:/wutan/block3d-cpp/build -DBLOCK3D_BUILD_PYTHON=ON
cmake --build Z:/wutan/block3d-cpp/build --config Release
python -c "import block3d; print(block3d.__version__)"
```

第二阶段再做 editable install / wheel：

```bash
python -m pip install -e Z:/wutan/block3d-cpp/python
python -m pip wheel Z:/wutan/block3d-cpp/python -w Z:/wutan/block3d-cpp/dist
```

---

## 7. CMake 打包规划

### 7.1 构建选项

建议新增：

```cmake
option(BLOCK3D_BUILD_SHARED "Build block3d as a shared library" ON)
option(BLOCK3D_BUILD_CLI "Build block3d_cli" ON)
option(BLOCK3D_BUILD_RUN_TEST "Build run_test benchmark tool" ON)
option(BLOCK3D_BUILD_TESTS "Build C/C++ tests" ON)
option(BLOCK3D_BUILD_EXAMPLES "Build examples" ON)
option(BLOCK3D_BUILD_PYTHON "Build Python module" OFF)
option(BLOCK3D_INSTALL "Enable install rules" ON)
```

说明：

- `block3d` 是正式库目标。
- `block3d_benchmark_cache` 只服务 `block3d_cli bench` 和 `run_test`，不应成为普通用户必须链接的核心依赖。
- `run_test` 可随源码构建，但不属于安装后最小运行时必需项。

### 7.2 安装布局

```text
install/
  include/
    block3d/
      block3d.h
      block3d.hpp
      version.h
      core.hpp              可选：高级 C++ 低层 API
      converter.hpp         可选
      reader.hpp            可选
      types.hpp             可选
  bin/
    block3d_cli.exe
    run_test.exe            可选
    block3d.dll             Windows shared build
  lib/
    block3d.lib / libblock3d.a / libblock3d.so
  lib/cmake/block3d/
    block3dConfig.cmake
    block3dConfigVersion.cmake
    block3dTargets.cmake
```

### 7.3 包外 CMake 使用方式

```cmake
cmake_minimum_required(VERSION 3.14)
project(block3d_consumer LANGUAGES CXX)

find_package(block3d CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE block3d::block3d)
```

验收命令：

```bash
cmake -S Z:/wutan/block3d-cpp -B Z:/wutan/block3d-cpp/build -DCMAKE_BUILD_TYPE=Release -DBLOCK3D_BUILD_TESTS=ON -DBLOCK3D_BUILD_EXAMPLES=ON
cmake --build Z:/wutan/block3d-cpp/build --config Release
cmake --install Z:/wutan/block3d-cpp/build --prefix Z:/wutan/block3d-cpp/install

cmake -S Z:/wutan/block3d-cpp/examples/consumer_cmake -B Z:/wutan/block3d-cpp/examples/consumer_cmake/build -DCMAKE_PREFIX_PATH=Z:/wutan/block3d-cpp/install
cmake --build Z:/wutan/block3d-cpp/examples/consumer_cmake/build --config Release
```

---

## 8. 示例程序规划

### 8.1 C 示例

文件：

```text
examples/c/basic_read_slice.c
```

内容：

1. 打开 `.b3d`。
2. 打印元数据。
3. 读取 X/Y/Z 各一张切片。
4. 读取 X 主维单列。
5. 读取一个小子体积和单点。
6. 释放所有结果。

### 8.2 C++ 示例

文件：

```text
examples/cpp/basic_read_slice.cpp
```

内容：

1. `block3d::Reader reader(path)`。
2. `reader.info()`。
3. `reader.read_slice(Axis::X, 0)`。
4. `reader.read_slices(Axis::X, {0, 1, 2})`。
5. `reader.read_column(Axis::X, 0, 0)`。
6. `reader.read_subvolume(0, 4, 0, 4, 0, 4)`。

### 8.3 Python 示例

文件：

```text
examples/python/basic_read_slice.py
```

内容：

```python
import sys
import block3d

path = sys.argv[1]
reader = block3d.Reader(path)
print(reader.info())

x0 = reader.read_slice("x", 0)
print("x0", x0.shape, x0.dtype, float(x0[0, 0]))

col = reader.read_column("x", 0, 0)
print("x column", col.shape, float(col[0]))
```

---

## 9. API 测试规划

测试分三层：

```text
1. 小数据 API smoke test
   - 快速，纳入 CTest / pytest
   - 自行生成无头 float32 小数据
   - 验证转换、读取、shape、值、错误路径

2. 安装后消费测试
   - 独立 C/C++ consumer project
   - find_package(block3d CONFIG REQUIRED)
   - 链接安装后的库，不使用源码树 include 路径

3. run_test-like packaged API 功能脚本
   - 使用正式 C++ / Python API
   - 借鉴 run_test 的固定随机/连续切片计划
   - 可跑小数据，也可指向 test18/test50
   - 重点验证“打包接口能完成需求文档里的功能输出”
```

---

## 10. 基于 `run_test` 的 C++ API 测试脚本规划

建议文件：

```text
tests/api/test_packaged_cpp_api.cpp
```

### 10.1 命令行

```bash
test_packaged_cpp_api \
  --raw Z:/wutan/block3d-data/test18/test18.dat \
  --b3d Z:/wutan/block3d-data/test18/test18.b3d \
  --dim-x 801 --dim-y 2405 --dim-z 2501 \
  --output-dir Z:/wutan/block3d-cpp/api_test_output/test18 \
  --random-count 100 \
  --seq-count 10 \
  --seed 42
```

小数据 CI 版本可不传大型路径，而是在临时目录生成小 raw。

### 10.2 脚本逻辑

```text
1. 解析参数。
2. 若 b3d 不存在，则调用 block3d::convert_raw_to_b3d(raw, b3d, dims, options)。
3. 打开 block3d::Reader。
4. info() 校验 dim_x/dim_y/dim_z、block_size、layout、version。
5. verify(raw, samples, 1e-3) 做随机点正确性校验。
6. 构造与 run_test 一致的请求计划：
   - seed = 42
   - random-count = 100
   - seq-count = 10
   - seq-start = 0
   - axes = x/y/z
   - modes = random/sequential
7. 对每个 axis/mode：
   - reader.read_slices(axis, indices)
   - 校验 batch shape 和 slice_count
   - 写出无头 float32 .raw：<axis>_<mode>_<seq>_<idx>.raw
   - 抽样对照原始 raw 文件，确认相对误差 <= 1e-3
8. 读取 X 主维单列：reader.read_column(Axis::X, y, z)
   - 长度必须是 dim_x
   - 抽样对照 raw[x, y, z]
9. 读取 Y/Z 列，确认三轴列 API 可用。
10. 读取小子体积：read_subvolume(xs, xe, ys, ye, zs, ze)
    - 校验 shape 和抽样值。
11. 读取单点：read_point(x, y, z)
12. 检查存储比例：b3d_size / raw_size < 1.5。
13. 打印 API_TEST_RESULT ok=1。
```

### 10.3 C++ 脚本核心伪代码

```cpp
block3d::ConvertOptionsFacade conv;
conv.block_size = 0;
conv.num_threads = 0;
conv.layout = block3d::Layout::LegacyXYZ;

if (!exists(b3d_path)) {
    block3d::convert_raw_to_b3d(raw_path, b3d_path, dx, dy, dz, conv);
}

block3d::Reader reader(b3d_path);
auto info = reader.info();
assert(info.dim_x == dx && info.dim_y == dy && info.dim_z == dz);
assert(reader.verify(raw_path, 1000, 1e-3f));

for (auto mode : {"random", "sequential"}) {
    for (auto axis : {block3d::Axis::X, block3d::Axis::Y, block3d::Axis::Z}) {
        auto indices = make_indices_like_run_test(axis, mode, seed, random_count, seq_count, seq_start);
        auto batch = reader.read_slices(axis, indices);
        assert(batch.slice_count == indices.size());
        write_batch_as_raw_files(batch, output_dir);
        sample_check_against_raw(raw_path, dx, dy, dz, axis, indices, batch, 1e-3f);
    }
}

auto xcol = reader.read_column(block3d::Axis::X, 0, 0);
assert(xcol.shape == std::vector<uint64_t>{dx});
check_x_column_against_raw(xcol, raw_path, dx, dy, dz, 0, 0);

auto sub = reader.read_subvolume(0, std::min<uint64_t>(4, dx),
                                 0, std::min<uint64_t>(5, dy),
                                 0, std::min<uint64_t>(6, dz));
check_subvolume_against_raw(sub, raw_path, dx, dy, dz);
```

### 10.4 与 `run_test` 的一致点

```text
一致：
- seed 默认 42
- random-count 默认 100
- seq-count 默认 10
- seq-start 默认 0
- 覆盖 x/y/z 三轴
- 覆盖 random/sequential 两种模式
- 批量切片读取
- 写出无头 float32 切片结果
- 输出可记录 plan hash

不同：
- 不做 cold/hot cache scrub
- 不把性能结果作为正式 benchmark
- 不启用复杂 pipeline writer 作为 API smoke 的必需路径
- 默认先用小数据，test18/test50 作为可选大数据验收
```

---

## 11. 基于 `run_test` 的 Python API 测试脚本规划

建议文件：

```text
tests/python/test_packaged_python_api.py
```

### 11.1 命令行

```bash
python tests/python/test_packaged_python_api.py \
  --raw Z:/wutan/block3d-data/test18/test18.dat \
  --b3d Z:/wutan/block3d-data/test18/test18.b3d \
  --dim-x 801 --dim-y 2405 --dim-z 2501 \
  --output-dir Z:/wutan/block3d-cpp/api_test_output_py/test18 \
  --random-count 100 \
  --seq-count 10 \
  --seed 42
```

### 11.2 脚本逻辑

```text
1. import block3d / numpy。
2. 如 b3d 不存在，调用 block3d.convert_raw_to_b3d()。
3. reader = block3d.Reader(b3d)。
4. reader.info() 校验维度和格式。
5. reader.verify(raw, samples=1000, tolerance=1e-3)。
6. 构造 run_test-like 请求计划。
7. reader.read_slices(axis, indices) 返回 ndarray：
   - x: (n, dim_y, dim_z)
   - y: (n, dim_x, dim_z)
   - z: (n, dim_x, dim_y)
8. 每张切片 `.tofile(output.raw)` 写出无头 float32。
9. 抽样 mmap 原始 raw，对照切片值。
10. reader.read_column("x", y, z) 验证 X 主维单列。
11. reader.read_subvolume(...) 验证子体积。
12. reader.read_point(...) 验证单点。
13. 检查 b3d/raw 存储比例 < 1.5。
```

### 11.3 Python 脚本核心伪代码

```python
import argparse
import os
from pathlib import Path
import numpy as np
import block3d


def raw_at(raw_mmap, dim_y, dim_z, x, y, z):
    return raw_mmap[x * dim_y * dim_z + y * dim_z + z]


if not Path(args.b3d).exists():
    block3d.convert_raw_to_b3d(
        raw_path=args.raw,
        b3d_path=args.b3d,
        dim_x=args.dim_x,
        dim_y=args.dim_y,
        dim_z=args.dim_z,
        block_size=0,
        layout="legacy",
        threads=0,
    )

reader = block3d.Reader(args.b3d, threads=0, read_dispatch="round-robin")
info = reader.info()
assert info["dim_x"] == args.dim_x
assert info["dim_y"] == args.dim_y
assert info["dim_z"] == args.dim_z
assert reader.verify(args.raw, samples=1000, tolerance=1e-3)

raw = np.memmap(args.raw, dtype=np.float32, mode="r")

for mode in ("random", "sequential"):
    for axis in ("x", "y", "z"):
        indices = make_indices_like_run_test(axis, mode, args)
        batch = reader.read_slices(axis, indices)
        assert batch.shape[0] == len(indices)
        for request_pos, index in enumerate(indices):
            out = output_path(args.output_dir, axis, mode, request_pos, index)
            batch[request_pos].astype(np.float32, copy=False).tofile(out)
            sample_check_slice(raw, batch[request_pos], axis, index, args, tolerance=1e-3)

xcol = reader.read_column("x", 0, 0)
assert xcol.shape == (args.dim_x,)
for x in sample_positions(args.dim_x):
    assert abs(float(xcol[x]) - float(raw_at(raw, args.dim_y, args.dim_z, x, 0, 0))) <= 1e-3

sub = reader.read_subvolume(0, min(4, args.dim_x), 0, min(5, args.dim_y), 0, min(6, args.dim_z))
assert sub.ndim == 3

print("API_TEST_RESULT ok=1")
```

---

## 12. 与《项目需求文档》的符合性检查

| 需求 | API/测试覆盖 | 结论 |
|---|---|---|
| 原始浮点数组与优化存储格式双向转换 | `convert_raw_to_b3d()` 覆盖 raw -> b3d；`read_full_volume()` / 切片输出可重建 raw，测试中小数据可做 full roundtrip，大数据不默认整卷写回 | 基本符合；如需严格“双向转换文件接口”，可后续增加 `export_b3d_to_raw()` facade |
| 支持 X/Y/Z 三维任意切片访问 | `read_slice(axis, index)`、`read_slices(axis, indices)`；C++/Python 脚本按 run_test 覆盖三轴 random/sequential | 符合 |
| 三个维度访问效率均衡 | API 暴露三轴同等接口；性能均衡仍由 `run_test`/benchmark 验证，不由 smoke test 判分 | 功能符合；性能另测 |
| 支持按主维度（如 X）单列高效读取 | `read_column(Axis::X, y, z)` / `read_column("x", y, z)`；测试脚本必须检查 X column 长度和值 | 符合 |
| 单机单进程多线程 | convert/read options 暴露 `num_threads`；内部 reader/converter 已支持多线程 | 符合 |
| 可使用 CUDA/GPU | 当前项目未实现 GPU；需求是可使用，不是必须使用 | 不纳入本阶段 |
| 内存使用量可配置、可管控 | options 暴露 `max_memory_mb` / `memory_limit_mb`，但文档必须声明当前是软预算，不是进程硬上限 | 部分符合，需如实说明 |
| 正确性：单点相对误差 < 1e-3 | `verify(raw, samples, tolerance)` + 脚本抽样对照 raw；小数据可全量校验 | 符合 |
| 存储空间 < 原始 1.5 倍 | 测试脚本检查 `b3d_size / raw_size < 1.5` | 符合 |
| 输出为标准格式作为读取结束标志 | run_test-like 脚本将切片写为无头 float32 `.raw`；API smoke 可少量写出 | 符合功能验收 |

### 12.1 需要补充的一个可选 API

如果评审或用户明确要求“优化格式 -> 完整原始 `.dat` 文件”的库函数，而不是由调用方 `read_full_volume().write()` 自行完成，建议第二步增加：

```text
C API:
block3d_export_b3d_to_raw(context, raw_output_path)

C++ facade:
reader.export_raw(raw_output_path)

Python:
reader.export_raw(raw_output_path)
```

第一版可以先不做，因为大数据整卷导出成本高，而且需求测试通常围绕切片输出；但文档中必须说明 `read_full_volume()` 只适合小数据或内存足够场景。

---

## 13. 分阶段实施计划

### 阶段 1：API facade 设计落地

目标：

1. 新增 `include/block3d/version.h`。
2. 新增 `include/block3d/block3d.hpp`。
3. 新增 `include/block3d/block3d.h`。
4. 新增 C API 实现文件，例如 `src/block3d_c_api.cpp`。
5. 必要时新增 C++ facade 实现文件，例如 `src/block3d_cpp_api.cpp`。

验收：

```bash
cmake --build Z:/wutan/block3d-cpp/build --config Release --target block3d
ctest --test-dir Z:/wutan/block3d-cpp/build -C Release -R '^test_block3d$' --output-on-failure
```

### 阶段 2：C/C++ API smoke test 和示例

目标：

1. 新增 C API smoke test。
2. 新增 C++ facade smoke test。
3. 新增 C/C++ examples。
4. 测试小数据生成、转换、三轴切片、批量切片、列、子体积、点、verify、错误参数。

验收：

```bash
cmake --build Z:/wutan/block3d-cpp/build --config Release --target test_block3d test_cli
ctest --test-dir Z:/wutan/block3d-cpp/build -C Release --output-on-failure
```

### 阶段 3：CMake install/export/find_package

目标：

1. 增加构建选项。
2. 安装 headers 和库。
3. 生成 CMake package config。
4. 新增独立 consumer example。

验收：

```bash
cmake --install Z:/wutan/block3d-cpp/build --prefix Z:/wutan/block3d-cpp/install
cmake -S Z:/wutan/block3d-cpp/examples/consumer_cmake -B Z:/wutan/block3d-cpp/examples/consumer_cmake/build -DCMAKE_PREFIX_PATH=Z:/wutan/block3d-cpp/install
cmake --build Z:/wutan/block3d-cpp/examples/consumer_cmake/build --config Release
```

### 阶段 4：Python API

目标：

1. 新增 `python/pybind_module.cpp`。
2. 新增 Python 包构建配置。
3. 暴露 `convert_raw_to_b3d()` 和 `Reader`。
4. Python 返回 NumPy `float32 ndarray`。
5. 新增 Python example 和 pytest。

验收：

```bash
python -c "import block3d; print(block3d.__version__)"
python Z:/wutan/block3d-cpp/examples/python/basic_read_slice.py Z:/wutan/block3d-data/test18/test18.b3d
python -m pytest Z:/wutan/block3d-cpp/tests/python
```

### 阶段 5：run_test-like packaged API 脚本

目标：

1. 新增 `tests/api/test_packaged_cpp_api.cpp`。
2. 新增 `tests/python/test_packaged_python_api.py`。
3. 两者使用正式 API，而不是直接调用内部 reader 头。
4. 两者按 `run_test` 的 seed、random-count、seq-count、axis、mode 生成请求计划。
5. 两者写出无头 float32 切片，并抽样对照 raw。
6. 两者检查 X 主维单列、子体积、单点和存储比例。

验收小数据：

```bash
ctest --test-dir Z:/wutan/block3d-cpp/build -C Release -R 'api|packaged' --output-on-failure
python -m pytest Z:/wutan/block3d-cpp/tests/python
```

验收 test18/test50（可选，大数据）：

```bash
Z:/wutan/block3d-cpp/build/Release/test_packaged_cpp_api.exe --raw ... --b3d ... --dim-x ... --dim-y ... --dim-z ...
python Z:/wutan/block3d-cpp/tests/python/test_packaged_python_api.py --raw ... --b3d ... --dim-x ... --dim-y ... --dim-z ...
```

### 阶段 6：最终文档和发布包

目标：

1. 更新 README。
2. 记录 C/C++/Python API。
3. 记录 CMake install 和 find_package。
4. 记录 examples 和 tests 运行方式。
5. 生成 Windows x64 Release zip。
6. 可选生成 Python wheel。

建议产物：

```text
dist/
  block3d-windows-x64-release.zip
  block3d-0.1.0-cp312-cp312-win_amd64.whl
```

---

## 14. 最终验收清单

### 14.1 源码树构建

```bash
cmake -S Z:/wutan/block3d-cpp -B Z:/wutan/block3d-cpp/build -DCMAKE_BUILD_TYPE=Release -DBLOCK3D_BUILD_TESTS=ON -DBLOCK3D_BUILD_EXAMPLES=ON
cmake --build Z:/wutan/block3d-cpp/build --config Release
ctest --test-dir Z:/wutan/block3d-cpp/build -C Release --output-on-failure
```

### 14.2 安装和包外 C++ 消费

```bash
cmake --install Z:/wutan/block3d-cpp/build --prefix Z:/wutan/block3d-cpp/install
cmake -S Z:/wutan/block3d-cpp/examples/consumer_cmake -B Z:/wutan/block3d-cpp/examples/consumer_cmake/build -DCMAKE_PREFIX_PATH=Z:/wutan/block3d-cpp/install
cmake --build Z:/wutan/block3d-cpp/examples/consumer_cmake/build --config Release
```

### 14.3 C/C++ 示例

```bash
Z:/wutan/block3d-cpp/build/Release/basic_read_slice_c.exe Z:/wutan/block3d-data/test18/test18.b3d
Z:/wutan/block3d-cpp/build/Release/basic_read_slice_cpp.exe Z:/wutan/block3d-data/test18/test18.b3d
```

### 14.4 Python 导入和示例

```bash
python -c "import block3d; print(block3d.__version__)"
python Z:/wutan/block3d-cpp/examples/python/basic_read_slice.py Z:/wutan/block3d-data/test18/test18.b3d
python -m pytest Z:/wutan/block3d-cpp/tests/python
```

### 14.5 run_test-like API 脚本

```bash
Z:/wutan/block3d-cpp/build/Release/test_packaged_cpp_api.exe --raw Z:/wutan/block3d-data/test18/test18.dat --b3d Z:/wutan/block3d-data/test18/test18.b3d --dim-x 801 --dim-y 2405 --dim-z 2501 --output-dir Z:/wutan/block3d-cpp/api_test_output/test18

python Z:/wutan/block3d-cpp/tests/python/test_packaged_python_api.py --raw Z:/wutan/block3d-data/test18/test18.dat --b3d Z:/wutan/block3d-data/test18/test18.b3d --dim-x 801 --dim-y 2405 --dim-z 2501 --output-dir Z:/wutan/block3d-cpp/api_test_output_py/test18
```

### 14.6 性能 benchmark（与 API 测试分离）

```bash
# 仍按 Z:/wutan/基准操作手册.md 执行
# run_test 和 block3d_cli bench 的计时口径不得和 API smoke test 混用
```

---

## 15. 风险与注意事项

1. **不要把 benchmark_cache 暴露成普通用户必需依赖。**
   - 它服务 cold/hot benchmark，不是核心读取库。

2. **不要承诺 `max_memory_mb` 是进程级硬上限。**
   - 当前是转换/读取路径中的软预算。

3. **不要隐藏输出布局。**
   - C、C++、Python 文档都必须写清楚 X/Y/Z slice 的 shape 和 flat offset。

4. **不要把 `verify()` 放进正式 cold benchmark 前同进程执行。**
   - 它会读取 raw 并污染 OS page cache。

5. **Python 不要返回 list。**
   - 大切片返回 list 会造成巨大内存和对象开销。第一版建议依赖 NumPy。

6. **Windows 动态库路径需要文档说明。**
   - DLL 应放在 exe/Python module 同目录，或安装目录加入 `PATH`，或采用静态链接分发。

7. **`read_full_volume()` 不能作为 test18/test50 默认验证方式。**
   - 大数据整卷读取内存压力巨大；只在小数据 smoke test 中全量 roundtrip。

8. **v2 micro-tiled 不应默认切换。**
   - Phase 6 归档显示 micro8 总体通过守门，但 X-heavy 负载仍可能退化。正式打包第一版默认保持 legacy，micro-tiled 作为显式选项。

---

## 16. 推荐最终决策

本阶段正式采用以下决策：

1. 保持现有核心算法、格式和 `BlockedFileReader` 不重写。
2. 第一版公共 API 以 **转换 + 读取** 为核心，而不是 `query_box` 单一接口。
3. C API 作为稳定 ABI，显式返回 `float32` array/batch 和 shape。
4. C++ API 提供 RAII `Reader` facade，不暴露复杂内部缓存和线程池细节。
5. Python API 使用 pybind11 + NumPy，返回 shaped `ndarray(float32)`。
6. CMake 补齐 install/export/find_package。
7. examples 和 API tests 与 benchmark 分离。
8. 新增 C++/Python run_test-like API 脚本，用正式接口验证需求文档中的三轴切片、X 主维单列、正确性和存储比例。
9. `run_test` 继续作为大数据性能工具保留，不纳入普通 API smoke test。

最小可交付范围：

```text
C API
C++ Reader facade
convert_raw_to_b3d facade
CMake install/export/find_package
C/C++ examples
C/C++ API smoke test
Python Reader + convert_raw_to_b3d
Python example + pytest
run_test-like C++/Python API 功能脚本规划并实现
```
