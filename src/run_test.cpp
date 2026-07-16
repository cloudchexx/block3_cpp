#include "block3d/reader.hpp"
#include "block3d/converter.hpp"
#include "block3d/rng.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <streambuf>
#include <thread>
#include <cstdio>
#include <limits>

#ifdef _WIN32
#include <io.h>      // _commit
#include <process.h> // _getpid
#else
#include <unistd.h>  // fsync, getpid
#endif

namespace fs = std::filesystem;
using namespace block3d;

// ── Tee streambuf (duplicate cout to log file) ───────────────────────

static std::streambuf* g_console_rdbuf = nullptr;
static std::ofstream   g_log_file;
static std::streambuf* g_tee_buf = nullptr;
static std::string     g_run_id;  // shared, unique: log name + per-run output dir

class TeeBuf : public std::streambuf {
    std::streambuf* out1_;
    std::streambuf* out2_;
public:
    TeeBuf(std::streambuf* o1, std::streambuf* o2) : out1_(o1), out2_(o2) {}
protected:
    int overflow(int c) override {
        if (c == EOF) return EOF;
        int r1 = out1_->sputc(static_cast<char>(c));
        out2_->sputc(static_cast<char>(c));
        return r1;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize r = out1_->sputn(s, n);
        out2_->sputn(s, n);
        return r;
    }
    int sync() override {
        int r = out1_->pubsync();
        out2_->pubsync();
        return r;
    }
};

// ── Cross-platform process id ────────────────────────────────────────
static unsigned long get_pid() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// Generate a per-run id with at least millisecond resolution + process id,
// so collisions across rapid re-invocations are avoided.
static std::string make_run_id() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tm, &tt);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::ostringstream id;
    id << std::put_time(&tm, "%Y%m%d_%H%M%S")
       << "_" << std::setfill('0') << std::setw(3) << ms.count()
       << "_pid" << get_pid();
    return id.str();
}

static void setup_logging(const std::vector<std::string>& datasets) {
    fs::create_directories("logs");

    std::ostringstream oss;
    oss << "logs/block3d_";
    for (size_t i = 0; i < datasets.size(); i++) {
        if (i > 0) oss << "_";
        oss << datasets[i];
    }
    oss << "_" << g_run_id << ".log";
    std::string log_path = oss.str();

    g_log_file.open(log_path);
    if (!g_log_file) {
        std::cerr << "Warning: cannot create log file: " << log_path << "\n";
        return;
    }

    g_console_rdbuf = std::cout.rdbuf();
    g_tee_buf = new TeeBuf(g_console_rdbuf, g_log_file.rdbuf());
    std::cout.rdbuf(g_tee_buf);
    std::cout << "Log saved to: " << log_path << "\n\n";
}

static void teardown_logging() {
    if (g_tee_buf) {
        std::cout.rdbuf(g_console_rdbuf);
        delete g_tee_buf;
        g_tee_buf = nullptr;
    }
    if (g_log_file.is_open()) g_log_file.close();
}

// ── Output persistence: flush + push to device ──────────────────────
// After fflush, request the OS to push buffered data to the storage stack.
// NOTE: this does NOT guarantee bypassing the device firmware write cache;
// a true durable barrier would require an explicit ATA/SCSI flush command
// at the device level, which this tool does not issue.
static bool sync_output(FILE* f) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

// ── Dataset definitions ──────────────────────────────────────────────

struct DatasetInfo {
    std::string raw;
    std::string b3d;
    uint64_t dim_x, dim_y, dim_z;
};

static std::map<std::string, DatasetInfo> DATASETS = {
    {"test18", {"test18.dat", "test18.b3d", 801, 2405, 2501}},
    {"test50", {"test50.dat", "test50.b3d", 2002, 2202, 3001}},
};

// ── Helpers ──────────────────────────────────────────────────────────

static bool is_valid_b3d(const std::string& path,
                          uint64_t dx, uint64_t dy, uint64_t dz,
                          uint64_t bs = 0) {
    if (!fs::exists(path)) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    FileHeader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (f.gcount() != sizeof(FileHeader)) return false;
    return std::memcmp(hdr.magic, MAGIC, 4) == 0 &&
           hdr.version == VERSION &&
           hdr.dim_x == dx && hdr.dim_y == dy && hdr.dim_z == dz &&
           (bs == 0 || hdr.block_size == bs);
}

static std::string size_str(uint64_t bytes) {
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gb << " GB";
    return oss.str();
}

// Resolve data file path, searching multiple directories.
// Search order: CWD, CWD/.., exe_dir, exe_dir/.., exe_dir/../..
static std::string resolve_path(const std::string& name,
                                  const fs::path& cwd,
                                  const fs::path& exe_dir) {
    auto check = [&](const fs::path& base) -> std::string {
        auto p = base / name;
        try {
            if (fs::exists(p)) return fs::absolute(p).string();
        } catch (...) {}
        return "";
    };

    // Search in order
    for (auto& d : {cwd, cwd.parent_path(), exe_dir,
                    exe_dir.parent_path(),
                    exe_dir.parent_path().parent_path()}) {
        auto r = check(d);
        if (!r.empty()) return r;
    }
    return name; // fallback: let caller handle
}

// ── Index generation ─────────────────────────────────────────────────

// Fixed-seed random indices (deterministic across runs).
static std::vector<uint64_t> make_rand_indices(uint32_t seed,
                                                int count,
                                                uint64_t dim) {
    XorShift32 rng(seed);
    std::vector<uint64_t> indices;
    indices.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++)
        indices.push_back(rng.rand_u64_mod(dim));
    return indices;
}

// Truly contiguous indices: start, start+1, ... (clamped to dim).
static std::vector<uint64_t> make_seq_indices(uint64_t start,
                                               int count,
                                               uint64_t dim) {
    std::vector<uint64_t> indices;
    if (start >= dim || count <= 0) return indices;
    uint64_t avail = dim - start;
    uint64_t n = std::min<uint64_t>(static_cast<uint64_t>(count), avail);
    indices.reserve(n);
    for (uint64_t i = 0; i < n; i++) indices.push_back(start + i);
    return indices;
}

// ── Steady-state batch benchmark ──────────────────────────────────────
// Reader construction (open + mmap + header parse) is OUTSIDE the timing
// window. The timed region is the steady-state batch: slice reads via
// read_slices_batch + per-slice raw float32 file create/write/close[/sync].
// This is NOT an end-to-end measurement including process/reader setup.

struct BenchResult {
    double total_sec = 0;
    double avg_ms = 0;
    double throughput = 0;
    size_t   n_read = 0;       // slices returned by read_slices_batch
    size_t   n_written = 0;    // slices fully written (open/write/sync/close ok)
    uint64_t total_bytes = 0;
    std::vector<uint64_t> indices;
    std::vector<uint64_t> slice_bytes;
    bool io_error = false;
};

static BenchResult run_bench(const std::string& b3d_path,
                              char axis,
                              const std::string& mode,
                              const std::vector<uint64_t>& indices,
                              const fs::path& out_dir,
                              const std::string& ds_name,
                              bool output_sync) {
    // Reader construction (mmap + header parse) is intentionally OUTSIDE the
    // timing window: this is a steady-state batch measurement, not an
    // end-to-end measurement that includes reader/process setup.
    BlockedFileReader reader(b3d_path);
    BenchResult r;
    r.indices = indices;

    auto t0 = std::chrono::high_resolution_clock::now();
    auto slices = reader.read_slices_batch(axis, indices);
    r.n_read = slices.size();
    r.slice_bytes.assign(slices.size(), 0);

    // A read-count mismatch is itself an I/O failure: do not write partial
    // output and do not emit success metrics for this case.
    if (slices.size() != indices.size()) {
        std::cerr << "[ERROR] read_slices_batch returned " << slices.size()
                  << " slices for " << indices.size()
                  << " requested (axis=" << axis << ", mode=" << mode << ")\n";
        r.io_error = true;
        auto t1 = std::chrono::high_resolution_clock::now();
        r.total_sec = std::chrono::duration<double>(t1 - t0).count();
        return r;
    }

    for (size_t i = 0; i < slices.size(); i++) {
        std::ostringstream fn;
        fn << ds_name << "_" << axis << "_" << mode << "_"
           << std::setw(4) << std::setfill('0') << i
           << "_idx" << indices[i] << ".raw";
        fs::path fp = out_dir / fn.str();
        std::string fp_str = fp.string();

        FILE* f = std::fopen(fp_str.c_str(), "wb");
        if (!f) {
            std::cerr << "[ERROR] cannot open output file: " << fp << "\n";
            r.io_error = true;
            continue;
        }
        const auto& sl = slices[i];
        uint64_t bytes = sl.size() * sizeof(float);
        // Per-slice success requires: full write, optional sync, clean close.
        // Only on full success do we count this slice as written.
        bool ok = std::fwrite(sl.data(), 1,
                              static_cast<size_t>(bytes), f)
                  == static_cast<size_t>(bytes);
        if (ok && output_sync) ok = sync_output(f);
        if (std::fclose(f) != 0) ok = false;
        if (!ok) {
            std::cerr << "[ERROR] write/sync/close failed for: " << fp << "\n";
            r.io_error = true;
            continue;
        }
        r.slice_bytes[i] = bytes;
        r.total_bytes += bytes;
        r.n_written++;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    r.total_sec = std::chrono::duration<double>(t1 - t0).count();
    // Success metrics exist ONLY when every requested slice was written.
    if (!r.io_error && r.n_written > 0 && r.n_written == r.n_read
        && r.total_sec > 0) {
        r.avg_ms     = r.total_sec / static_cast<double>(r.n_written) * 1000.0;
        r.throughput = static_cast<double>(r.n_written) / r.total_sec;
    }
    return r;
}

static void print_sep(const std::string& title) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(70, '=') << "\n";
}

static void log_per_slice_bytes(const BenchResult& r) {
    if (r.slice_bytes.empty()) {
        std::cout << "         per-slice bytes: (none)\n";
        return;
    }
    bool uniform = true;
    for (size_t i = 1; i < r.slice_bytes.size(); i++)
        if (r.slice_bytes[i] != r.slice_bytes[0]) { uniform = false; break; }
    std::cout << "         per-slice bytes: ";
    if (uniform) {
        std::cout << r.slice_bytes[0] << " x" << r.slice_bytes.size()
                  << " (uniform), total=" << r.total_bytes << "\n";
    } else {
        std::cout << "[";
        for (size_t i = 0; i < r.slice_bytes.size(); i++) {
            if (i) std::cout << ",";
            std::cout << r.slice_bytes[i];
        }
        std::cout << "], total=" << r.total_bytes << "\n";
    }
}

// Print a bench table only for fully-successful cases. The caller must NOT
// invoke this when any case in the group had an I/O error (we abort instead).
static void print_bench_table(const std::string& title,
                               const std::vector<char>& axes,
                               const std::vector<BenchResult>& results) {
    std::cout << "\n[" << title << "]\n";
    std::cout << "         " << std::left << std::setw(6) << "Axis"
              << std::right << std::setw(10) << "Total"
              << std::setw(12) << "Avg"
              << std::setw(14) << "Throughput" << "\n";
    std::cout << "         " << std::string(6, '-') << " "
              << std::string(8, '-') << " "
              << std::string(10, '-') << " "
              << std::string(12, '-') << "\n";

    double sum = 0, mn = std::numeric_limits<double>::max(), mx = 0;
    int cnt = 0;
    for (size_t i = 0; i < axes.size(); i++) {
        const auto& r = results[i];
        std::cout << "         "
                  << std::left << std::setw(6)
                  << static_cast<char>(std::toupper(axes[i]))
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(9) << r.total_sec << "s"
                  << std::setw(10) << std::setprecision(2) << r.avg_ms << "ms"
                  << std::setw(12) << std::setprecision(1) << r.throughput
                  << " slices/s  (read " << r.n_read
                  << ", written " << r.n_written << ", "
                  << size_str(r.total_bytes) << " written)\n";
        log_per_slice_bytes(r);
        if (r.n_written > 0) {
            sum += r.avg_ms;
            if (r.avg_ms < mn) mn = r.avg_ms;
            if (r.avg_ms > mx) mx = r.avg_ms;
            cnt++;
        }
    }

    if (cnt > 0) {
        double avg = sum / static_cast<double>(cnt);
        std::cout << "         " << std::string(42, '-') << "\n";
        std::cout << "         " << std::left << std::setw(6) << "Avg"
                  << std::right << std::setw(12)
                  << std::setprecision(2) << avg << "ms";
        if (cnt > 1 && mn > 0)
            std::cout << "  balance=" << std::setprecision(2) << (mx / mn) << "x";
        else
            std::cout << "  balance=n/a";
        std::cout << "  (over " << cnt
                  << (cnt > 1 ? " axes" : " axis") << ")\n";
    }
}

// ── Full test for one dataset ────────────────────────────────────────

// Tracks whether ANY b3d conversion has happened in this process. If so, the
// freshly-written b3d pages are very likely resident in the OS page cache, so
// benchmark numbers measured later in the SAME process are NOT cold-cache.
// This flag is process-wide (not per-dataset).
static bool g_converted_b3d_in_process = false;

static int run_full_test(const std::string& ds_name,
                           const DatasetInfo& cfg,
                           uint64_t block_size,
                           int random_count, int seq_count,
                           bool verify_enabled, int verify_samples,
                           bool warm_up,
                           size_t warmup_stride,
                           uint64_t warmup_memory_mb,
                           uint32_t seed,
                           uint64_t seq_start,
                           const fs::path& run_dir,
                           const std::vector<char>& sel_axes,
                           const std::vector<std::string>& sel_modes,
                           bool output_sync) {
    print_sep("Dataset: " + ds_name + "  (" +
              std::to_string(cfg.dim_x) + "x" +
              std::to_string(cfg.dim_y) + "x" +
              std::to_string(cfg.dim_z) + ")");

    std::string b3d_path = cfg.b3d;
    std::string raw_path = cfg.raw;

    // raw existence is validated by the caller, but be defensive.
    if (!fs::exists(raw_path)) {
        std::cout << "[ERROR] raw file '" << raw_path << "' not found for "
                  << ds_name << "\n";
        return 1;
    }

    // Auto-detect block_size when not explicitly set.
    if (block_size == 0) {
        auto slash = b3d_path.find_last_of("/\\");
        std::string parent =
            (slash != std::string::npos) ? b3d_path.substr(0, slash) : ".";
        auto sc = detect_storage_medium(parent);
        block_size = auto_block_size(cfg.dim_x, cfg.dim_y, cfg.dim_z, sc);
        const char* sc_names[] = {"HDD", "SSD", "NVMe", "Unknown"};
        std::cout << "[AUTO-BS] Detected "
                  << sc_names[static_cast<int>(sc)]
                  << " storage, auto block_size=" << block_size << "\n";
    }

    // 1. Convert (skip if valid)
    if (is_valid_b3d(b3d_path, cfg.dim_x, cfg.dim_y, cfg.dim_z, block_size)) {
        auto sz = fs::file_size(b3d_path);
        std::cout << "[SKIP] " << ds_name << ": '" << b3d_path
                  << "' already exists (" << size_str(sz)
                  << "), valid format\n";
    } else {
        std::cout << "[CONVERT] " << ds_name << ": " << raw_path
                  << " -> " << b3d_path << "\n";
        std::cout << "         dims=" << cfg.dim_x << "x" << cfg.dim_y
                  << "x" << cfg.dim_z << ", block_size=" << block_size
                  << ", threads=auto\n";
        // Record that this process converted a b3d. The freshly-written b3d
        // pages are very likely resident in the OS page cache, so any
        // benchmark measured later in THIS process is NOT cold-cache.
        g_converted_b3d_in_process = true;

        auto t0 = std::chrono::high_resolution_clock::now();
        convert_raw_to_blocked(raw_path, b3d_path,
                               cfg.dim_x, cfg.dim_y, cfg.dim_z,
                               block_size, 0, true);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        auto b3d_sz = fs::file_size(b3d_path);
        auto raw_sz = fs::file_size(raw_path);
        double ratio = static_cast<double>(b3d_sz) / static_cast<double>(raw_sz);
        std::cout << "         done in " << std::fixed
                  << std::setprecision(1) << dt << "s, "
                  << size_str(b3d_sz) << ", storage ratio "
                  << std::setprecision(3) << ratio << "x\n";
    }

    // 2. Verify (opt-in). Reads the raw input, so it pollutes the page cache
    //    for THIS process; subsequent benchmarks are NOT cold-cache results.
    if (verify_enabled) {
        if (verify_samples <= 0) {
            std::cout << "[ERROR] --verify requested but verify-samples="
                      << verify_samples << " is not positive\n";
            return 1;
        }
        std::cout << "\n[VERIFY] *** --verify ENABLED for this process ***\n"
                  << "         Verifying " << verify_samples
                  << " random points by reading the RAW input file.\n"
                  << "         This populates the OS page cache; benchmark "
                     "numbers in THIS process\n"
                  << "         are NOT cold-cache results. Use --verify for "
                     "correctness checks only.\n";
        BlockedFileReader reader(b3d_path);
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = reader.verify(raw_path, static_cast<uint64_t>(verify_samples));
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        if (ok) {
            std::cout << "         PASSED in " << std::fixed
                      << std::setprecision(2) << dt << "s\n";
        } else {
            std::cout << "         FAILED! Data mismatch detected.\n";
            return 1;
        }
    }

    unsigned hc = std::thread::hardware_concurrency();
    int thread_count = static_cast<int>(hc > 0 ? hc : 1);

    std::cout << "\n[CONFIG] seed=" << seed
              << " (fixed for random reads)\n";
    std::cout << "         threads=" << thread_count << " (auto)\n";
    std::cout << "         run_dir=" << run_dir.string()
              << " (shared across datasets; files prefixed by dataset name)\n";
    std::cout << "         output_pattern=" << ds_name
              << "_<axis>_<mode>_<seq>_<idx>.raw  (mode=rand|seq, seq=0000..)\n";
    std::cout << "         output_sync="
              << (output_sync ? "requested" : "disabled") << "\n";
    if (output_sync) {
        std::cout << "           requested: after fflush, "
#ifdef _WIN32
                  << "_commit"
#else
                  << "fsync"
#endif
                  << " is issued per output file. Timing covers slice read +\n"
                  << "           file create + write + close + sync. This does "
                     "NOT guarantee bypassing\n"
                  << "           the device firmware write cache.\n";
    } else {
        std::cout << "           disabled (--no-output-sync): data is committed "
                     "to the OS only; this is a\n"
                  << "           diagnostic mode and is NOT a durable/persisted "
                     "result. The device firmware\n"
                  << "           cache is NOT bypassed.\n";
    }
    std::cout << "         timing: steady-state batch = slice read "
                 "(read_slices_batch) + per-slice raw\n"
              << "                  float32 file create/write/close"
              << (output_sync ? "/sync" : "") << ".\n"
              << "                  Reader construction (open+mmap+header "
                 "parse) is NOT timed.\n"
              << "                  This is NOT an end-to-end measurement "
                 "including process/reader setup.\n";

    // Warm up OS page cache before benchmarks (hot-cache diagnostic only).
    if (warm_up) {
        std::cout << "\n[WARMUP] *** HOT-CACHE DIAGNOSTIC --warm-up ENABLED ***\n"
                  << "         Preloading data pages (stride=" << warmup_stride;
        if (warmup_memory_mb > 0)
            std::cout << ", max_mem=" << warmup_memory_mb << "MB";
        std::cout << ") ..." << std::flush;
        auto t0 = std::chrono::high_resolution_clock::now();
        {
            BlockedFileReader reader(b3d_path);
            reader.warm_up(false, warmup_stride, warmup_memory_mb);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        std::cout << " done in " << std::fixed
                  << std::setprecision(1) << dt << "s\n"
                  << "         NOTE: numbers below are WARM-cache, NOT cold-cache.\n";
    }

    // If THIS process converted a b3d (above), the freshly-written b3d pages
    // are very likely resident in the OS page cache. The benchmarks below in
    // THIS process are therefore NOT cold-cache results. A proper cold-cache
    // measurement requires: exit this process, have an external harness clear
    // the OS page cache, then restart with the now-existing .b3d using a
    // single --axis + single --mode. Do NOT interpret the numbers below as
    // cold-cache.
    if (g_converted_b3d_in_process) {
        std::cout << "\n[CONVERSION-CACHE-NOTE] A .b3d was CONVERTED in this "
                     "process. The newly-written\n"
                  << "            b3d pages are very likely resident in the OS "
                     "page cache, so the\n"
                  << "            benchmark(s) below measured in THIS process "
                     "are NOT cold-cache\n"
                  << "            results. To obtain a cold-cache measurement: "
                     "EXIT this process, have\n"
                  << "            an external harness clear the OS page cache, "
                     "then RESTART against the\n"
                  << "            now-existing .b3d using a single --axis and a "
                     "single --mode.\n";
    }

    uint64_t dims[3] = {cfg.dim_x, cfg.dim_y, cfg.dim_z};

    // 3. Per-mode benchmarks over the selected axes only.
    for (const auto& mode : sel_modes) {
        bool is_rand = (mode == "random");
        const std::string& mlabel = is_rand ? std::string("rand")
                                            : std::string("seq");
        const char* title = is_rand ? "RANDOM" : "SEQUENTIAL";

        // Validate parameters for the cases we are about to generate.
        int count = is_rand ? random_count : seq_count;
        if (count <= 0) {
            std::cout << "\n[ERROR] " << mode << " count=" << count
                      << " must be > 0\n";
            return 1;
        }

        if (!is_rand) {
            // seq_start must be < the dimension of every selected axis.
            for (char a : sel_axes) {
                uint64_t dim = (a == 'x') ? dims[0]
                              : (a == 'y') ? dims[1] : dims[2];
                if (seq_start >= dim) {
                    std::cout << "\n[ERROR] seq_start=" << seq_start
                              << " must be < dim_" << a << "=" << dim
                              << " for axis '" << a << "'\n";
                    return 1;
                }
            }
        }

        std::cout << "\n[" << title << "] "
                  << (is_rand
                        ? ("seed=" + std::to_string(seed) + ", "
                           + std::to_string(random_count)
                           + " random slices per selected axis")
                        : ("contiguous from start=" + std::to_string(seq_start)
                           + ", requested=" + std::to_string(seq_count)
                           + " (clamped to dim-start per axis)"))
                  << "\n";

        std::vector<BenchResult> results;
        results.reserve(sel_axes.size());
        for (char a : sel_axes) {
            uint64_t dim = (a == 'x') ? dims[0]
                          : (a == 'y') ? dims[1] : dims[2];
            std::vector<uint64_t> idx = is_rand
                ? make_rand_indices(seed, random_count, dim)
                : make_seq_indices(seq_start, seq_count, dim);

            if (!is_rand) {
                std::cout << "         axis=" << a
                          << " start_index=" << seq_start
                          << " contiguous_length=" << idx.size()
                          << " (dim=" << dim << ")\n";
            }

            // Each generated case must have at least 1 request.
            if (idx.empty()) {
                std::cout << "[ERROR] generated " << mode
                          << " case for axis '" << a
                          << "' produced 0 requests (dim=" << dim
                          << ", count=" << count << ")\n";
                return 1;
            }

            results.push_back(run_bench(b3d_path, a, mlabel,
                                         idx, run_dir, ds_name,
                                         output_sync));
        }

        // On ANY I/O error in this mode group, do NOT print a timing table
        // (no avg/balance). Report and fail.
        bool any_err = false;
        for (size_t i = 0; i < sel_axes.size(); i++) {
            if (results[i].io_error) {
                std::cout << "[ERROR] I/O error during " << mode
                          << " bench axis " << sel_axes[i]
                          << " (read=" << results[i].n_read
                          << ", written=" << results[i].n_written << ")\n";
                any_err = true;
            }
        }
        if (any_err) {
            std::cout << "[ERROR] Skipping timing table for " << mode
                      << " due to I/O errors above.\n";
            return 1;
        }

        print_bench_table(title, sel_axes, results);
    }

    // 4. Summary
    {
        auto raw_sz = fs::file_size(raw_path);
        auto b3d_sz = fs::file_size(b3d_path);
        double ratio = static_cast<double>(b3d_sz) / static_cast<double>(raw_sz);
        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "  SUMMARY -- " << ds_name << "\n";
        std::cout << std::string(70, '=') << "\n";
        std::cout << "  Storage:   " << size_str(b3d_sz)
                  << " / " << size_str(raw_sz)
                  << " = " << std::fixed << std::setprecision(3) << ratio << "x\n";
        std::cout << "  Outputs:   " << run_dir.string() << "\n";
        std::cout << std::string(70, '=') << "\n\n";
    }
    return 0;
}

// ── Main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::vector<std::string> datasets;
    uint64_t block_size = 0;   // 0 = auto-detect via storage probe
    int random_count = 100;
    int seq_count = 10;
    int verify_samples = 20000;
    bool verify_enabled = false;   // default: do NOT pollute cache
    bool no_log = false;
    bool warm_up = false;
    int warmup_stride_val = 4096;
    int warmup_memory_mb = 0;
    uint32_t seed = 42;
    uint64_t seq_start = 0;
    std::string output_dir;  // empty -> derive (cwd/benchmark_output)
    bool output_sync = true; // default: request persistence
    std::string axis_arg = "all";
    std::string mode_arg = "all";
    uint64_t custom_dx = 0, custom_dy = 0, custom_dz = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--datasets" && i + 1 < argc) {
            while (++i < argc && argv[i][0] != '-')
                datasets.push_back(argv[i]);
            i--;
        } else if (arg == "--block-size" && i + 1 < argc) {
            block_size = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--random-count" && i + 1 < argc) {
            random_count = std::atoi(argv[++i]);
        } else if (arg == "--seq-count" && i + 1 < argc) {
            seq_count = std::atoi(argv[++i]);
        } else if (arg == "--seq-start" && i + 1 < argc) {
            seq_start = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint32_t>(
                        std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--verify-samples" && i + 1 < argc) {
            verify_samples = std::atoi(argv[++i]);
        } else if (arg == "--verify") {
            verify_enabled = true;
        } else if (arg == "--skip-verify") {
            verify_enabled = false;  // compat; also the default
        } else if (arg == "--no-output-sync") {
            output_sync = false;
        } else if (arg == "--axis" && i + 1 < argc) {
            axis_arg = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            mode_arg = argv[++i];
        } else if (arg == "--no-log") {
            no_log = true;
        } else if (arg == "--warm-up") {
            warm_up = true;
        } else if (arg == "--warmup-stride" && i + 1 < argc) {
            warmup_stride_val = std::atoi(argv[++i]);
        } else if (arg == "--warmup-memory" && i + 1 < argc) {
            warmup_memory_mb = std::atoi(argv[++i]);
        } else if (arg == "--dim-x" && i + 1 < argc) {
            custom_dx = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dim-y" && i + 1 < argc) {
            custom_dy = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dim-z" && i + 1 < argc) {
            custom_dz = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    if (datasets.empty()) datasets = {"test18", "test50"};

    // ── Strict parameter validation (fail fast, non-zero) ────────────
    // block_size == 0 means auto-detect; resolved per-dataset in run_full_test.
    if (block_size > 256) {
        std::cerr << "[ERROR] --block-size must be 0–256 (0=auto)\n";
        return 2;
    }
    if (random_count <= 0) {
        std::cerr << "[ERROR] --random-count must be > 0 (got "
                  << random_count << ")\n";
        return 2;
    }
    if (seq_count <= 0) {
        std::cerr << "[ERROR] --seq-count must be > 0 (got "
                  << seq_count << ")\n";
        return 2;
    }
    if (warmup_memory_mb < 0) {
        std::cerr << "[ERROR] --warmup-memory must be >= 0 (got "
                  << warmup_memory_mb << ")\n";
        return 2;
    }
    if (warm_up && warmup_stride_val < 1) {
        std::cerr << "[ERROR] --warmup-stride must be >= 1 when warming up (got "
                  << warmup_stride_val << ")\n";
        return 2;
    }

    std::vector<char> sel_axes;
    if (axis_arg == "all")      sel_axes = {'x', 'y', 'z'};
    else if (axis_arg == "x")   sel_axes = {'x'};
    else if (axis_arg == "y")   sel_axes = {'y'};
    else if (axis_arg == "z")   sel_axes = {'z'};
    else {
        std::cerr << "[ERROR] invalid --axis '" << axis_arg
                  << "': must be x|y|z|all\n";
        return 2;
    }

    std::vector<std::string> sel_modes;
    if (mode_arg == "all")           sel_modes = {"random", "sequential"};
    else if (mode_arg == "random")  sel_modes = {"random"};
    else if (mode_arg == "sequential") sel_modes = {"sequential"};
    else {
        std::cerr << "[ERROR] invalid --mode '" << mode_arg
                  << "': must be random|sequential|all\n";
        return 2;
    }

    if (verify_enabled && verify_samples <= 0) {
        std::cerr << "[ERROR] --verify requested but --verify-samples="
                  << verify_samples << " is not positive\n";
        return 2;
    }

    auto cwd     = fs::current_path();
    auto exe_dir = fs::path(argv[0]).parent_path();

    // Resolve file paths
    auto resolve = [&](DatasetInfo& cfg) {
        cfg.raw = resolve_path(cfg.raw, cwd, exe_dir);
        cfg.b3d = resolve_path(cfg.b3d, cwd, exe_dir);
    };

    // ── Unique run id (ms timestamp + pid) and unique shared run dir ──
    // Bump suffix until BOTH the output dir and (if logging) the log file
    // path do not already exist; never reuse/truncate a prior run's files.
    std::string base_id = make_run_id();
    fs::path out_root = output_dir.empty()
        ? (cwd / "benchmark_output")
        : fs::path(output_dir);
    auto log_candidate = [&](const std::string& id) -> fs::path {
        std::ostringstream oss;
        oss << "logs/block3d_";
        for (size_t i = 0; i < datasets.size(); i++) {
            if (i > 0) oss << "_";
            oss << datasets[i];
        }
        oss << "_" << id << ".log";
        return fs::path(oss.str());
    };
    std::string run_id = base_id;
    int suffix = 2;
    for (;;) {
        fs::path cand_dir = out_root / run_id;
        bool exists_dir = fs::exists(cand_dir);
        bool exists_log = (!no_log) && fs::exists(log_candidate(run_id));
        if (!exists_dir && !exists_log) break;
        run_id = base_id + "_" + std::to_string(suffix++);
        if (suffix > 9999) { // sanity guard
            std::cerr << "[ERROR] could not allocate a unique run id\n";
            return 1;
        }
    }
    g_run_id = run_id;
    fs::path run_dir = out_root / run_id;

    // Logging
    if (!no_log) setup_logging(datasets);

    // ── Run-wide notices ─────────────────────────────────────────────
    std::cout << "[CACHE-NOTE] This tool does NOT clear the OS page cache or "
                 "device caches.\n"
              << "            Cold-cache measurement is an EXTERNAL precondition: "
                 "an administrator\n"
              << "            or test harness must clear the OS page cache (and verify "
                 "physical disk\n"
              << "            reads) BEFORE running. Without that, results reflect "
                 "whatever cached\n"
              << "            state the OS/device currently holds.\n";

    std::cout << "\n[OUTPUT-SYNC] output_sync="
              << (output_sync ? "requested" : "disabled") << "\n";
    if (output_sync) {
        std::cout << "   requested (default): after fflush, "
#ifdef _WIN32
                  << "_commit"
#else
                  << "fsync"
#endif
                  << " is issued per output file. Timing covers slice read +\n"
                  << "   file create + write + close + sync. This does NOT guarantee "
                     "bypassing the\n"
                  << "   device firmware write cache.\n";
    } else {
        std::cout << "   disabled (--no-output-sync): data is committed to the OS only; "
                     "this is a\n"
                  << "   diagnostic mode and is NOT a durable/persisted result. The "
                     "device firmware\n"
                  << "   cache is NOT bypassed.\n";
    }

    // Case plan + caching implications.
    {
        std::cout << "\n[CASE-PLAN] axis=" << axis_arg << " mode=" << mode_arg << "\n";
        bool all_axes = (sel_axes.size() == 3);
        bool all_modes = (sel_modes.size() == 2);
        if (all_axes || all_modes) {
            std::cout << "   Cases run sequentially in this single process. The OS "
                         "page cache state\n"
                      << "   evolves across cases, so these are NOT independent "
                         "cold-cache results.\n"
                      << "   For cold-cache reference numbers, externally clear the "
                         "cache and run a\n"
                      << "   single --axis + single --mode in a separate process.\n";
        } else {
            std::cout << "   A reduced case set runs in this process (one axis, one "
                         "mode). Cache state\n"
                      << "   still depends on prior activity in this process (e.g. "
                         "--verify/--warm-up)\n"
                      << "   and is NOT cleared by this tool.\n";
        }
    }

    if (verify_enabled) {
        std::cout << "\n[VERIFY-MODE] *** --verify ENABLED ***\n"
                  << "   This process will read the RAW input file for verification, "
                     "populating the OS\n"
                  << "   page cache. Benchmark numbers in THIS process are NOT "
                     "cold-cache results.\n"
                  << "   Use --verify for correctness checking only; the default is to "
                     "skip verify.\n";
    } else {
        std::cout << "\n[VERIFY-MODE] verify=skipped (default). Benchmark cases do not "
                     "read the raw input\n"
                  << "   in this process; cold-cache remains an external precondition.\n";
    }

    if (warm_up) {
        std::cout << "\n!!! ================ WARM-CACHE DIAGNOSTIC ================ !!!\n"
                  << "!!! --warm-up is ENABLED for this run. Benchmark numbers  !!!\n"
                  << "!!! below reflect a WARM OS page cache and are NOT        !!!\n"
                  << "!!! cold-cache performance numbers. Use only for hot-cache !!!\n"
                  << "!!! diagnostics.                                          !!!\n"
                  << "!!! ============== END WARM-CACHE DIAGNOSTIC ============= !!!\n";
    }
    std::cout << "\n";

    // Create the shared run directory now (unique id guarantees no reuse).
    {
        std::error_code ec;
        fs::create_directories(run_dir, ec);
        if (ec) {
            std::cerr << "[ERROR] cannot create run directory '" << run_dir
                      << "': " << ec.message() << "\n";
            teardown_logging();
            return 1;
        }
    }

    auto t_start = std::chrono::high_resolution_clock::now();

    int ret = 0;
    for (const auto& ds_name : datasets) {
        DatasetInfo cfg;
        auto it = DATASETS.find(ds_name);
        if (it != DATASETS.end()) {
            cfg = it->second;
        } else {
            // Custom dataset: requires --dim-x/--dim-y/--dim-z.
            if (custom_dx == 0 || custom_dy == 0 || custom_dz == 0) {
                std::cout << "[ERROR] Unknown dataset: " << ds_name
                          << ". Available:";
                for (const auto& [k, v] : DATASETS) std::cout << " " << k;
                std::cout << "\n"
                          << "        For custom datasets, specify"
                          << " --dim-x N --dim-y N --dim-z N\n";
                ret = 1;
                break;
            }
            cfg.raw   = ds_name + ".dat";
            cfg.b3d   = ds_name + ".b3d";
            cfg.dim_x = custom_dx;
            cfg.dim_y = custom_dy;
            cfg.dim_z = custom_dz;
        }
        resolve(cfg);

        if (!fs::exists(cfg.raw)) {
            std::cout << "[ERROR] raw file not found for dataset '" << ds_name
                      << "': " << cfg.raw << "\n";
            ret = 1;
            break;
        }

        ret = run_full_test(ds_name, cfg, block_size,
                            random_count, seq_count,
                            verify_enabled, verify_samples,
                            warm_up,
                            static_cast<size_t>(warmup_stride_val),
                            static_cast<uint64_t>(warmup_memory_mb),
                            seed,
                            seq_start,
                            run_dir,
                            sel_axes,
                            sel_modes,
                            output_sync);
        if (ret != 0) break;
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double total_dt = std::chrono::duration<double>(t_end - t_start).count();
    std::cout << "Total elapsed: " << std::fixed
              << std::setprecision(1) << total_dt << "s\n";

    teardown_logging();
    return ret;
}
