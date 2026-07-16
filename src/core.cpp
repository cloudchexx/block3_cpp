#include "block3d/core.hpp"
#include <algorithm>

namespace block3d {

std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>
BlockLayout3D::block_order() const {
    struct Entry { uint32_t code, bx, by_, bz; };
    std::vector<Entry> entries;
    entries.reserve(total_blocks);

    for (uint32_t bx = 0; bx < static_cast<uint32_t>(blocks_x); bx++)
        for (uint32_t by_ = 0; by_ < static_cast<uint32_t>(blocks_y); by_++)
            for (uint32_t bz = 0; bz < static_cast<uint32_t>(blocks_z); bz++)
                entries.push_back({morton_encode(bx, by_, bz), bx, by_, bz});

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.code < b.code; });

    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> result;
    result.reserve(entries.size());
    for (auto& e : entries)
        result.emplace_back(e.bx, e.by_, e.bz);

    return result;
}

} // namespace block3d
