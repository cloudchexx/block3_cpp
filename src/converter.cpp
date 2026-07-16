#include "block3d/converter.hpp"
#include "block3d/reader.hpp"
#include <cstring>
#include <fstream>
#include <vector>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>

#ifdef OMP_ENABLED
#include <omp.h>
#endif

namespace block3d {

struct BlockData {
    uint64_t linear_idx;
    std::vector<float> data;
};

static void extract_block(uint32_t bx, uint32_t by_, uint32_t bz,
                          const float* raw_3d,
                          uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
                          uint64_t bs, float* out) {
    uint64_t x0 = static_cast<uint64_t>(bx) * bs;
    uint64_t y0 = static_cast<uint64_t>(by_) * bs;
    uint64_t z0 = static_cast<uint64_t>(bz) * bs;
    uint64_t nx = std::min(bs, dim_x - x0);
    uint64_t ny = std::min(bs, dim_y - y0);
    uint64_t nz = std::min(bs, dim_z - z0);

    if (nx == bs && ny == bs && nz == bs) {
        for (uint64_t lx = 0; lx < bs; lx++) {
            uint64_t raw_base = (x0 + lx) * dim_y * dim_z + y0 * dim_z + z0;
            for (uint64_t ly = 0; ly < bs; ly++) {
                std::memcpy(&out[lx * bs * bs + ly * bs],
                            &raw_3d[raw_base + ly * dim_z],
                            bs * sizeof(float));
            }
        }
    } else {
        std::memset(out, 0, bs * bs * bs * sizeof(float));
        for (uint64_t lx = 0; lx < nx; lx++) {
            uint64_t raw_base = (x0 + lx) * dim_y * dim_z + y0 * dim_z + z0;
            for (uint64_t ly = 0; ly < ny; ly++) {
                std::memcpy(&out[lx * bs * bs + ly * bs],
                            &raw_3d[raw_base + ly * dim_z],
                            nz * sizeof(float));
            }
        }
    }
}

void convert_raw_to_blocked(
    const std::string& raw_path,
    const std::string& output_path,
    uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
    uint64_t block_size,
    int num_threads,
    bool progress,
    uint64_t max_memory_mb)
{
    if (num_threads <= 0)
        num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (num_threads < 1) num_threads = 1;

    BlockLayout3D layout(dim_x, dim_y, dim_z, block_size);
    uint64_t bs = block_size;

    MappedFile raw_file(raw_path);
    const float* raw_3d = raw_file.data();

    auto ordered_blocks = layout.block_order();

    std::ofstream out(output_path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot create output file: " + output_path);

    FileHeader header;
    std::memcpy(header.magic, MAGIC, 4);
    header.version      = VERSION;
    header.dim_x        = dim_x;
    header.dim_y        = dim_y;
    header.dim_z        = dim_z;
    header.block_size   = block_size;
    header.total_blocks = layout.total_blocks;
    header.data_offset  = aligned_data_offset(layout.total_blocks);
    header.reserved     = 0;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    uint64_t offset_placeholder = 0;
    for (uint64_t i = 0; i < layout.total_blocks; i++)
        out.write(reinterpret_cast<const char*>(&offset_placeholder),
                  sizeof(uint64_t));

    uint64_t current_pos = static_cast<uint64_t>(out.tellp());
    if (current_pos < header.data_offset) {
        std::vector<char> pad(header.data_offset - current_pos, 0);
        out.write(pad.data(), pad.size());
    }

    size_t total = ordered_blocks.size();

    size_t batch_size;
    if (max_memory_mb > 0) {
        uint64_t bytes_per_block = layout.block_bytes;
        uint64_t max_bytes = max_memory_mb * 1024 * 1024;
        uint64_t max_blocks_in_mem = max_bytes / bytes_per_block;
        if (max_blocks_in_mem < 1) max_blocks_in_mem = 1;
        batch_size = std::max<size_t>(1,
            std::min<size_t>(total, static_cast<size_t>(max_blocks_in_mem)));
    } else {
        batch_size = total;
    }

    if (num_threads > 1 && batch_size > static_cast<size_t>(num_threads) * 4) {
        size_t min_per_thread = 16;
        batch_size = std::max(static_cast<size_t>(num_threads) * min_per_thread,
                              batch_size / 4);
    }
    if (batch_size > total) batch_size = total;
    if (batch_size < 1) batch_size = 1;

    std::vector<uint64_t> block_offsets(layout.total_blocks);
    std::atomic<size_t> done{0};

    auto print_progress = [&](const char* prefix) {
        if (!progress) return;
        size_t d = done.load();
        if (d % 100 == 0 || d == total) {
            std::cout << "\r" << prefix << " " << d << "/" << total
                      << " blocks" << std::flush;
        }
    };

    std::vector<float> block_pool(batch_size * layout.block_floats);

    for (size_t batch_start = 0; batch_start < total;
         batch_start += batch_size) {
        size_t batch_end = std::min(batch_start + batch_size, total);
        size_t batch_len = batch_end - batch_start;

        std::vector<BlockData> batch_data(batch_len);

#ifdef OMP_ENABLED
        #pragma omp parallel for num_threads(num_threads)
#endif
        for (int64_t i = 0; i < static_cast<int64_t>(batch_len); i++) {
            auto [bx, by_, bz] = ordered_blocks[batch_start + i];
            batch_data[i].linear_idx =
                layout.linear_index(bx, by_, bz);
            float* dst = block_pool.data() + i * layout.block_floats;
            extract_block(bx, by_, bz, raw_3d,
                          dim_x, dim_y, dim_z, bs, dst);
        }

        for (size_t i = 0; i < batch_len; i++) {
            uint64_t off = static_cast<uint64_t>(out.tellp());
            block_offsets[batch_data[i].linear_idx] = off;
            const float* src = block_pool.data() + i * layout.block_floats;
            out.write(
                reinterpret_cast<const char*>(src),
                static_cast<std::streamsize>(layout.block_bytes));
            done++;
            print_progress("Converting");
        }
    }

    if (progress) std::cout << "\n";

    out.seekp(HEADER_SIZE);
    out.write(reinterpret_cast<const char*>(block_offsets.data()),
              static_cast<std::streamsize>(layout.total_blocks * sizeof(uint64_t)));

    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    out.close();

    if (progress) {
        std::cout << "Conversion complete: " << output_path << std::endl;
    }
}

} // namespace block3d
