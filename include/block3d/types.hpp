#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace block3d {

#pragma pack(push, 1)
struct FileHeader {
    char     magic[4];       // "3DBK"
    uint32_t version;        // 1
    uint64_t dim_x;
    uint64_t dim_y;
    uint64_t dim_z;
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t data_offset;    // byte offset of first block data
    uint64_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 4 + 4 + 7 * 8,
              "FileHeader must be 64 bytes");

constexpr const char* MAGIC = "3DBK";
constexpr uint32_t VERSION = 1;
constexpr uint64_t HEADER_SIZE = sizeof(FileHeader);
constexpr uint64_t PAGE_ALIGN = 4096;

inline uint64_t aligned_data_offset(uint64_t total_blocks) {
    uint64_t off = HEADER_SIZE + total_blocks * sizeof(uint64_t);
    return (off + PAGE_ALIGN - 1) & ~(PAGE_ALIGN - 1);
}

inline std::string str_axis(char axis) {
    return std::string(1, axis);
}

} // namespace block3d
