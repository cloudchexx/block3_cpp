#include "block3d/reader.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <fstream>

#ifdef OMP_ENABLED
#include <omp.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace block3d {

// ── MappedFile ────────────────────────────────────────────────────────

MappedFile::MappedFile(const std::string& path) { do_mmap(path); }
MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : mapped_data_(other.mapped_data_), size_(other.size_),
      file_handle_(other.file_handle_), map_handle_(other.map_handle_)
{
    other.mapped_data_ = nullptr;
    other.file_handle_ = nullptr;
    other.map_handle_  = nullptr;
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        mapped_data_ = other.mapped_data_;
        size_        = other.size_;
        file_handle_ = other.file_handle_;
        map_handle_  = other.map_handle_;
        other.mapped_data_ = nullptr;
        other.file_handle_ = nullptr;
        other.map_handle_  = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MappedFile::close() {
#ifdef _WIN32
    if (mapped_data_) { UnmapViewOfFile(mapped_data_); mapped_data_ = nullptr; }
    if (map_handle_)  { CloseHandle(map_handle_);       map_handle_  = nullptr; }
    if (file_handle_ && file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle_); file_handle_ = nullptr;
    }
#else
    if (mapped_data_ && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, size_);
        mapped_data_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    size_ = 0;
}

void MappedFile::do_mmap(const std::string& path) {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Failed to open file: " + path);
    file_handle_ = hFile;

    LARGE_INTEGER li;
    GetFileSizeEx(hFile, &li);
    size_ = static_cast<size_t>(li.QuadPart);

    HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile); file_handle_ = nullptr;
        throw std::runtime_error("Failed to create file mapping: " + path);
    }
    map_handle_ = hMap;

    void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!ptr) {
        CloseHandle(hMap);   map_handle_ = nullptr;
        CloseHandle(hFile);  file_handle_ = nullptr;
        throw std::runtime_error("Failed to map view: " + path);
    }
    mapped_data_ = ptr;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("Failed to open file: " + path);
    fd_ = fd;

    struct stat st;
    fstat(fd, &st);
    size_ = static_cast<size_t>(st.st_size);

    void* ptr = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd); fd_ = -1;
        throw std::runtime_error("Failed to mmap file: " + path);
    }
    mapped_data_ = ptr;
#endif
}

void MappedFile::prefault(size_t offset, size_t length, size_t stride) {
    if (!mapped_data_ || offset >= size_) return;
    if (offset + length > size_) length = size_ - offset;
    if (length == 0 || stride == 0) return;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(mapped_data_) + offset;

#ifdef _WIN32
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = const_cast<void*>(static_cast<const void*>(base));
    entry.NumberOfBytes  = length;
    if (PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0)) return;
#else
    if (::madvise(static_cast<char*>(mapped_data_) + offset,
                  length, MADV_WILLNEED) == 0) return;
#endif

    volatile uint8_t sink;
    for (size_t pos = 0; pos < length; pos += stride) {
        sink = base[pos];
    }
    (void)sink;
}

// ── Intra-slice block-processing helper ───────────────────────────────
// Blocks within a single slice write to non-overlapping regions of the
// output buffer, so they can safely run in parallel without locks.
// Falls back to serial when block_count ≤ num_threads × 4.

namespace {

template<typename F>
void for_each_block_parallel(int num_threads,
                              const std::vector<BlockCoord>& blocks,
                              F&& process) {
    size_t nb = blocks.size();
    int nt = num_threads;
    if (nt <= 1 || nb <= static_cast<size_t>(nt) * 4) {
        for (size_t i = 0; i < nb; i++) process(blocks[i]);
        return;
    }
    size_t nw = static_cast<size_t>(nt);
    if (nw > nb) nw = nb;
    std::vector<std::thread> threads;
    threads.reserve(nw);
    for (size_t t = 0; t < nw; t++) {
        threads.emplace_back([&, t]() {
            for (size_t i = t; i < nb; i += nw) process(blocks[i]);
        });
    }
    for (auto& th : threads) th.join();
}

} // anonymous namespace

// ── BlockedFileReader ─────────────────────────────────────────────────

BlockedFileReader::BlockedFileReader(const std::string& file_path,
                                     int num_threads,
                                     uint64_t max_memory_mb)
    : num_threads_(num_threads > 0 ? num_threads
                                   : static_cast<int>(
                                         std::thread::hardware_concurrency())),
      max_memory_mb_(max_memory_mb)
{
    if (num_threads_ < 1) num_threads_ = 1;

    std::ifstream f(file_path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open file: " + file_path);

    FileHeader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (f.gcount() != sizeof(hdr))
        throw std::runtime_error("Failed to read header");

    if (std::memcmp(hdr.magic, MAGIC, 4) != 0)
        throw std::runtime_error("Invalid file magic");

    dim_x_ = hdr.dim_x;
    dim_y_ = hdr.dim_y;
    dim_z_ = hdr.dim_z;
    block_size_ = hdr.block_size;
    total_blocks_ = hdr.total_blocks;
    data_offset_ = hdr.data_offset;
    layout_ = BlockLayout3D(dim_x_, dim_y_, dim_z_, block_size_);

    block_offsets_.resize(total_blocks_);
    f.seekg(HEADER_SIZE);
    f.read(reinterpret_cast<char*>(block_offsets_.data()),
           total_blocks_ * sizeof(uint64_t));

    mapped_file_ = MappedFile(file_path);
}

void BlockedFileReader::warm_up(bool async, size_t stride,
                                 uint64_t max_memory_mb) {
    if (warm_up_future_.valid() &&
        !warm_up_done_.load(std::memory_order_acquire)) return;

    uint64_t bs = block_size_;
    uint64_t data_size = total_blocks_ * bs * bs * bs * sizeof(float);
    uint64_t prefault_size = data_size;
    if (max_memory_mb > 0) {
        uint64_t max_bytes = max_memory_mb * 1024ULL * 1024ULL;
        if (prefault_size > max_bytes) prefault_size = max_bytes;
    }

    auto prefault_fn = [this, stride, prefault_size]() {
        mapped_file_.prefault(data_offset_, prefault_size, stride);
        warm_up_done_.store(true, std::memory_order_release);
    };

    if (async) {
        warm_up_done_.store(false, std::memory_order_release);
        warm_up_future_ = std::async(std::launch::async, prefault_fn);
    } else {
        warm_up_done_.store(false, std::memory_order_release);
        prefault_fn();
    }
}

bool BlockedFileReader::is_warm_up_done() const {
    return warm_up_done_.load(std::memory_order_acquire);
}

void BlockedFileReader::wait_warm_up() {
    if (warm_up_future_.valid()) warm_up_future_.wait();
}

uint64_t
BlockedFileReader::block_offset_float(uint32_t bx, uint32_t by_,
                                       uint32_t bz) const {
    uint64_t lin = layout_.linear_index(bx, by_, bz);
    if (lin >= block_offsets_.size()) {
        throw std::out_of_range("block_offset_float: linear index out of range");
    }
    return block_offsets_[lin] / 4;
}

void BlockedFileReader::cache_prune() {
    while (cache_.size() >= CACHE_MAX_ENTRIES && !cache_order_.empty()) {
        CacheKey oldest = cache_order_.front();
        cache_order_.pop_front();
        cache_.erase(oldest);
    }
}

std::vector<BlockCoord>
BlockedFileReader::sorted_block_list(char axis, uint64_t index) {
    uint64_t bs = block_size_;
    uint32_t block_key = static_cast<uint32_t>(index / bs);

    CacheKey key{axis, block_key};

    // Fast path: cache hit. Copy the cached vector *out while holding the
    // lock*, then return that independent copy. The caller may iterate it
    // after the lock is released, so we must never hand back a reference into
    // cache_ (a rehash, prune, or concurrent overwrite of the same key would
    // dangle it).
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            cache_order_.remove(key);
            cache_order_.push_back(key);
            return it->second;  // copy under lock
        }
    }

    // Slow path: build the sorted block list as pure local work (no shared
    // state touched). The check-and-insert is intentionally NOT atomic across
    // the build, so multiple threads may build the same key concurrently;
    // that is harmless because each returns its own local copy and the
    // insert path below is guarded and idempotent.
    std::vector<BlockCoord> coords;

    if (axis == 'x') {
        for (uint32_t by_ = 0; by_ < layout_.blocks_y; by_++)
            for (uint32_t bz = 0; bz < layout_.blocks_z; bz++)
                coords.push_back({block_key, by_, bz});
    } else if (axis == 'y') {
        for (uint32_t bx = 0; bx < layout_.blocks_x; bx++)
            for (uint32_t bz = 0; bz < layout_.blocks_z; bz++)
                coords.push_back({bx, block_key, bz});
    } else {
        for (uint32_t bx = 0; bx < layout_.blocks_x; bx++)
            for (uint32_t by_ = 0; by_ < layout_.blocks_y; by_++)
                coords.push_back({bx, by_, block_key});
    }

    std::sort(coords.begin(), coords.end(),
              [this](const BlockCoord& a, const BlockCoord& b) {
                  return block_offset_float(a.bx, a.by_, a.bz) <
                         block_offset_float(b.bx, b.by_, b.bz);
              });

    // Publish under the lock. Re-check first: another thread may have
    // inserted the same key while we were building. If so, refresh its LRU
    // position and return the existing (deterministic) content instead of
    // clobbering it -- this avoids a redundant overwrite of a vector that a
    // third thread may currently be copying out.
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            cache_order_.remove(key);
            cache_order_.push_back(key);
            return it->second;  // copy under lock
        }
        cache_prune();
        // Store a copy in the cache, keep our local for the return value so
        // we don't need a second copy out of the map.
        cache_[key] = coords;
        cache_order_.push_back(key);
        return coords;  // move/copy of our local; fully independent of cache_
    }
}

// ── X-Slice ──────────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_x_slice(uint64_t x) {
    if (x >= dim_x_)
        throw std::out_of_range("X out of range");

    uint64_t ny = dim_y_, nz = dim_z_;
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    std::vector<float> result(ny * nz);
    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);

    auto blocks = sorted_block_list('x', x);

    for_each_block_parallel(num_threads_, blocks,
        [&](const BlockCoord& c) {
            if (c.bx != bx) return;

            uint64_t y0 = static_cast<uint64_t>(c.by_) * bs;
            uint64_t z0 = static_cast<uint64_t>(c.bz) * bs;
            uint64_t ny_part = std::min(bs, dim_y_ - y0);
            uint64_t nz_part = std::min(bs, dim_z_ - z0);

            uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);
            const float* src = dv + boff + static_cast<uint64_t>(lx) * bs * bs;

            for (uint64_t ly = 0; ly < ny_part; ly++) {
                float* dst = &result[(y0 + ly) * nz + z0];
                const float* srow = src + ly * bs;
                for (uint64_t lz = 0; lz < nz_part; lz++) {
                    dst[lz] = srow[lz];
                }
            }
        });

    return result;
}

// ── Y-Slice ──────────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_y_slice(uint64_t y) {
    if (y >= dim_y_)
        throw std::out_of_range("Y out of range");

    uint64_t nx = dim_x_, nz = dim_z_;
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    std::vector<float> result(nx * nz);
    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t ly  = static_cast<uint32_t>(y % bs);

    auto blocks = sorted_block_list('y', y);

    for_each_block_parallel(num_threads_, blocks,
        [&](const BlockCoord& c) {
            if (c.by_ != by_) return;

            uint64_t x0 = static_cast<uint64_t>(c.bx) * bs;
            uint64_t z0 = static_cast<uint64_t>(c.bz) * bs;
            uint64_t nx_part = std::min(bs, dim_x_ - x0);
            uint64_t nz_part = std::min(bs, dim_z_ - z0);

            uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);

            for (uint32_t lx = 0; lx < nx_part; lx++) {
                const float* src = dv + boff
                    + static_cast<uint64_t>(lx) * bs * bs
                    + static_cast<uint64_t>(ly) * bs;
                float* dst = &result[(x0 + lx) * nz + z0];
                for (uint64_t lz = 0; lz < nz_part; lz++) {
                    dst[lz] = src[lz];
                }
            }
        });

    return result;
}

// ── Z-Slice ──────────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_z_slice(uint64_t z) {
    if (z >= dim_z_)
        throw std::out_of_range("Z out of range");

    uint64_t nx = dim_x_, ny = dim_y_;
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    std::vector<float> result(nx * ny);
    uint32_t bz = static_cast<uint32_t>(z / bs);
    uint32_t lz = static_cast<uint32_t>(z % bs);

    uint64_t bs2 = bs * bs;

    auto blocks = sorted_block_list('z', z);

    for_each_block_parallel(num_threads_, blocks,
        [&](const BlockCoord& c) {
            if (c.bz != bz) return;

            uint64_t x0 = static_cast<uint64_t>(c.bx) * bs;
            uint64_t y0 = static_cast<uint64_t>(c.by_) * bs;
            uint64_t nx_part = std::min(bs, dim_x_ - x0);
            uint64_t ny_part = std::min(bs, dim_y_ - y0);

            uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);

            for (uint32_t lx = 0; lx < nx_part; lx++) {
                const float* src_plane = dv + boff + static_cast<uint64_t>(lx) * bs2;
                uint64_t x_row = (x0 + lx) * ny;
                for (uint32_t ly = 0; ly < ny_part; ly++) {
                    result[x_row + (y0 + ly)] =
                        src_plane[static_cast<uint64_t>(ly) * bs + lz];
                }
            }
        });

    return result;
}

// ── Generic slice ────────────────────────────────────────────────────

std::vector<float>
BlockedFileReader::read_slice(char axis, uint64_t index) {
    switch (axis) {
    case 'x': return read_x_slice(index);
    case 'y': return read_y_slice(index);
    case 'z': return read_z_slice(index);
    default:
        throw std::invalid_argument("Unknown axis: " + str_axis(axis));
    }
}

std::vector<std::vector<float>>
BlockedFileReader::read_slices_batch(char axis,
                                     const std::vector<uint64_t>& indices,
                                     int /*num_threads*/) {
    // Each read_slice call parallelises block processing internally via
    // num_threads_.  Process slices sequentially to avoid oversubscription.
    size_t n = indices.size();
    std::vector<std::vector<float>> results(n);
    for (size_t i = 0; i < n; i++) {
        results[i] = read_slice(axis, indices[i]);
    }
    return results;
}

// ── Column reads ─────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_x_column(uint64_t y, uint64_t z) {
    if (y >= dim_y_ || z >= dim_z_)
        throw std::out_of_range("Y or Z out of range");

    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();
    std::vector<float> result(dim_x_);

    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t bz  = static_cast<uint32_t>(z / bs);
    uint32_t ly  = static_cast<uint32_t>(y % bs);
    uint32_t lz  = static_cast<uint32_t>(z % bs);

    for (uint32_t bx = 0; bx < layout_.blocks_x; bx++) {
        uint64_t x0 = static_cast<uint64_t>(bx) * bs;
        uint64_t nx_part = std::min(bs, dim_x_ - x0);
        uint64_t boff = block_offset_float(bx, by_, bz);

        for (uint32_t lx = 0; lx < nx_part; lx++) {
            result[x0 + lx] = dv[boff
                + static_cast<uint64_t>(lx) * bs * bs
                + static_cast<uint64_t>(ly) * bs
                + lz];
        }
    }
    return result;
}

std::vector<float> BlockedFileReader::read_y_column(uint64_t x, uint64_t z) {
    if (x >= dim_x_ || z >= dim_z_)
        throw std::out_of_range("X or Z out of range");

    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();
    std::vector<float> result(dim_y_);

    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t bz = static_cast<uint32_t>(z / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);
    uint32_t lz = static_cast<uint32_t>(z % bs);

    for (uint32_t by_ = 0; by_ < layout_.blocks_y; by_++) {
        uint64_t y0 = static_cast<uint64_t>(by_) * bs;
        uint64_t ny_part = std::min(bs, dim_y_ - y0);
        uint64_t boff = block_offset_float(bx, by_, bz);

        for (uint32_t ly = 0; ly < ny_part; ly++) {
            result[y0 + ly] = dv[boff
                + static_cast<uint64_t>(lx) * bs * bs
                + static_cast<uint64_t>(ly) * bs
                + lz];
        }
    }
    return result;
}

std::vector<float> BlockedFileReader::read_z_column(uint64_t x, uint64_t y) {
    if (x >= dim_x_ || y >= dim_y_)
        throw std::out_of_range("X or Y out of range");

    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();
    std::vector<float> result(dim_z_);

    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);
    uint32_t ly = static_cast<uint32_t>(y % bs);

    for (uint32_t bz = 0; bz < layout_.blocks_z; bz++) {
        uint64_t z0 = static_cast<uint64_t>(bz) * bs;
        uint64_t nz_part = std::min(bs, dim_z_ - z0);
        uint64_t boff = block_offset_float(bx, by_, bz);

        const float* src = dv + boff
            + static_cast<uint64_t>(lx) * bs * bs
            + static_cast<uint64_t>(ly) * bs;
        std::memcpy(&result[z0], src, nz_part * sizeof(float));
    }
    return result;
}

// ── Batch column reads ───────────────────────────────────────────────

namespace {

template<typename F>
std::vector<std::vector<float>>
column_batch(const std::vector<std::pair<uint64_t, uint64_t>>& coords,
             int num_threads,
             F&& read_fn) {
    size_t n = coords.size();
    std::vector<std::vector<float>> results(n);

    int nt = num_threads;
    if (nt <= 0) nt = static_cast<int>(std::thread::hardware_concurrency());
    if (nt < 1) nt = 1;

    if (nt <= 1 || n <= 1) {
        for (size_t i = 0; i < n; i++)
            results[i] = read_fn(coords[i].first, coords[i].second);
        return results;
    }

    size_t nw = static_cast<size_t>(nt);
    if (nw > n) nw = n;
    std::vector<std::thread> threads;
    threads.reserve(nw);

    for (size_t w = 0; w < nw; w++) {
        threads.emplace_back([&, w, nw]() {
            for (size_t i = w; i < n; i += nw)
                results[i] = read_fn(coords[i].first, coords[i].second);
        });
    }
    for (auto& t : threads) t.join();
    return results;
}

} // anonymous namespace

std::vector<std::vector<float>>
BlockedFileReader::read_x_columns_batch(
    const std::vector<std::pair<uint64_t, uint64_t>>& coords,
    int num_threads) {
    return column_batch(coords, num_threads > 0 ? num_threads : num_threads_,
        [this](uint64_t a, uint64_t b) { return read_x_column(a, b); });
}

std::vector<std::vector<float>>
BlockedFileReader::read_y_columns_batch(
    const std::vector<std::pair<uint64_t, uint64_t>>& coords,
    int num_threads) {
    return column_batch(coords, num_threads > 0 ? num_threads : num_threads_,
        [this](uint64_t a, uint64_t b) { return read_y_column(a, b); });
}

std::vector<std::vector<float>>
BlockedFileReader::read_z_columns_batch(
    const std::vector<std::pair<uint64_t, uint64_t>>& coords,
    int num_threads) {
    return column_batch(coords, num_threads > 0 ? num_threads : num_threads_,
        [this](uint64_t a, uint64_t b) { return read_z_column(a, b); });
}

// ── Subvolume ────────────────────────────────────────────────────────

std::vector<float>
BlockedFileReader::read_subvolume(uint64_t xs, uint64_t xe,
                                   uint64_t ys, uint64_t ye,
                                   uint64_t zs, uint64_t ze) {
    if (xs >= xe || ys >= ye || zs >= ze)
        throw std::invalid_argument("Invalid subvolume range");
    if (xe > dim_x_ || ye > dim_y_ || ze > dim_z_)
        throw std::out_of_range("Subvolume exceeds dimensions");

    uint64_t nx = xe - xs;
    uint64_t ny = ye - ys;
    uint64_t nz = ze - zs;
    std::vector<float> result(nx * ny * nz);
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    uint32_t bxs = static_cast<uint32_t>(xs / bs);
    uint32_t bxe = static_cast<uint32_t>((xe - 1) / bs) + 1;
    uint32_t bys = static_cast<uint32_t>(ys / bs);
    uint32_t bye = static_cast<uint32_t>((ye - 1) / bs) + 1;
    uint32_t bzs = static_cast<uint32_t>(zs / bs);
    uint32_t bze = static_cast<uint32_t>((ze - 1) / bs) + 1;

    for (uint32_t bx = bxs; bx < bxe; bx++) {
        for (uint32_t by_ = bys; by_ < bye; by_++) {
            for (uint32_t bz = bzs; bz < bze; bz++) {
                uint64_t x0 = static_cast<uint64_t>(bx) * bs;
                uint64_t y0 = static_cast<uint64_t>(by_) * bs;
                uint64_t z0 = static_cast<uint64_t>(bz) * bs;

                uint32_t lx_start = static_cast<uint32_t>(
                    (xs > x0) ? (xs - x0) : 0);
                uint32_t lx_end   = static_cast<uint32_t>(
                    std::min(xe - x0, bs));
                uint32_t ly_start = static_cast<uint32_t>(
                    (ys > y0) ? (ys - y0) : 0);
                uint32_t ly_end   = static_cast<uint32_t>(
                    std::min(ye - y0, bs));
                uint32_t lz_start = static_cast<uint32_t>(
                    (zs > z0) ? (zs - z0) : 0);
                uint32_t lz_end   = static_cast<uint32_t>(
                    std::min(ze - z0, bs));
                uint32_t ln = lz_end - lz_start;
                if (ln == 0) continue;

                uint64_t boff = block_offset_float(bx, by_, bz);

                for (uint32_t lx = lx_start; lx < lx_end; lx++) {
                    uint64_t dx = x0 + lx - xs;
                    for (uint32_t ly = ly_start; ly < ly_end; ly++) {
                        uint64_t dy = y0 + ly - ys;
                        uint64_t src_start = boff
                            + static_cast<uint64_t>(lx) * bs * bs
                            + static_cast<uint64_t>(ly) * bs
                            + lz_start;
                        uint64_t dst_idx = dx * ny * nz + dy * nz;
                        std::memcpy(&result[dst_idx + (z0 + lz_start - zs)],
                                    dv + src_start,
                                    ln * sizeof(float));
                    }
                }
            }
        }
    }
    return result;
}

std::vector<float> BlockedFileReader::read_full_volume() {
    return read_subvolume(0, dim_x_, 0, dim_y_, 0, dim_z_);
}

// ── Verify ───────────────────────────────────────────────────────────

float BlockedFileReader::read_point(uint64_t x, uint64_t y, uint64_t z) {
    uint64_t bs = block_size_;
    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t bz  = static_cast<uint32_t>(z / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);
    uint32_t ly = static_cast<uint32_t>(y % bs);
    uint32_t lz = static_cast<uint32_t>(z % bs);
    uint64_t boff = block_offset_float(bx, by_, bz);
    return mapped_file_.data()[boff
        + static_cast<uint64_t>(lx) * bs * bs
        + static_cast<uint64_t>(ly) * bs
        + lz];
}

bool BlockedFileReader::verify(const std::string& raw_path,
                                uint64_t num_samples, float tol) {
    MappedFile raw(raw_path);
    const float* raw_data = raw.data();

    XorShift32 rng(42);

    std::vector<uint64_t> xs(num_samples), ys(num_samples), zs(num_samples);
    for (uint64_t s = 0; s < num_samples; s++) {
        xs[s] = rng.rand_u64_mod(dim_x_);
        ys[s] = rng.rand_u64_mod(dim_y_);
        zs[s] = rng.rand_u64_mod(dim_z_);
    }

    int nt = num_threads_;
    if (nt <= 1 || num_samples <= 1) nt = 1;

    size_t workers = static_cast<size_t>(nt);
    if (workers > num_samples) workers = num_samples;

    std::atomic<bool> all_passed{true};
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t w = 0; w < workers; w++) {
        threads.emplace_back([&, w, workers]() {
            for (size_t s = w; s < num_samples; s += workers) {
                uint64_t x = xs[s], y = ys[s], z = zs[s];
                float blocked_val = read_point(x, y, z);
                uint64_t raw_idx = x * dim_y_ * dim_z_ + y * dim_z_ + z;
                float raw_val = raw_data[raw_idx];
                float abs_raw = std::abs(raw_val);
                float threshold = tol * std::max(abs_raw, 1e-10f);
                if (std::abs(blocked_val - raw_val) > threshold) {
                    all_passed.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return all_passed.load(std::memory_order_relaxed);
}

} // namespace block3d
