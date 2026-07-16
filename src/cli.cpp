#include "block3d/reader.hpp"
#include "block3d/converter.hpp"
#include "block3d/rng.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdlib>

using namespace block3d;

static void print_usage(const char* prog) {
    std::cerr << "Usage:\n";
    std::cerr << "  " << prog << " convert <input> <output> --dim-x N --dim-y N --dim-z N\n";
    std::cerr << "         [--block-size N] [--threads N] [--memory-limit N] [--no-progress]\n";
    std::cerr << "  " << prog << " info <file>\n";
    std::cerr << "  " << prog << " bench <file> [--num-reads N] [--random] [--threads N] [--memory-limit N] [--warm-up]\n";
    std::cerr << "  " << prog << " verify <b3d> <raw> [--samples N]\n";
    std::cerr << "  " << prog << " extract <file> --axis x|y|z --index N [-o FILE]\n";
    std::cerr << "  " << prog << " extract <file> --column-axis x|y|z --c1 N --c2 N [-o FILE]\n";
}

static bool has_arg(int argc, char* argv[], const char* name) {
    for (int i = 0; i < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

static const char* get_arg(int argc, char* argv[], const char* name,
                            const char* def = nullptr) {
    for (int i = 1; i < argc - 1; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

static int get_arg_int(int argc, char* argv[], const char* name, int def) {
    auto* s = get_arg(argc, argv, name);
    return s ? std::atoi(s) : def;
}

static uint64_t get_arg_uint64(int argc, char* argv[], const char* name,
                                uint64_t def) {
    auto* s = get_arg(argc, argv, name);
    return s ? std::strtoull(s, nullptr, 10) : def;
}

// ── Command: convert ─────────────────────────────────────────────────

static int cmd_convert(int argc, char* argv[]) {
    if (argc < 4) { std::cerr << "Missing arguments for convert\n"; return 1; }
    const char* input  = argv[2];
    const char* output = argv[3];
    uint64_t dx = get_arg_uint64(argc, argv, "--dim-x", 0);
    uint64_t dy = get_arg_uint64(argc, argv, "--dim-y", 0);
    uint64_t dz = get_arg_uint64(argc, argv, "--dim-z", 0);
    if (dx == 0 || dy == 0 || dz == 0) {
        std::cerr << "Must specify --dim-x, --dim-y, --dim-z\n"; return 1;
    }
    uint64_t bs = get_arg_uint64(argc, argv, "--block-size", 32);
    int nt = get_arg_int(argc, argv, "--threads", 0);
    uint64_t mem_mb = get_arg_uint64(argc, argv, "--memory-limit", 0);
    bool prog = !has_arg(argc, argv, "--no-progress");

    std::cout << "Converting: " << input << " -> " << output << "\n";
    std::cout << "  dims=" << dx << "x" << dy << "x" << dz
              << " block_size=" << bs << " threads="
              << (nt ? std::to_string(nt) : "auto");
    if (mem_mb) std::cout << " mem_limit=" << mem_mb << "MB";
    std::cout << "\n";

    auto t0 = std::chrono::high_resolution_clock::now();
    convert_raw_to_blocked(input, output, dx, dy, dz, bs, nt, prog, mem_mb);
    auto t1 = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();

    std::ifstream out_f(output, std::ios::binary | std::ios::ate);
    auto size_bytes = out_f.tellg();
    double size_gb = size_bytes / (1024.0 * 1024.0 * 1024.0);
    std::cout << "Done in " << dt << "s. Output: " << size_gb << " GB\n";
    return 0;
}

// ── Command: info ────────────────────────────────────────────────────

static int cmd_info(int argc, char* argv[]) {
    if (argc < 3) { std::cerr << "Missing file argument\n"; return 1; }
    BlockedFileReader reader(argv[2]);
    const auto& lay = reader.layout();
    double raw_size = static_cast<double>(reader.dim_x() * reader.dim_y()
                                          * reader.dim_z() * 4);

    std::ifstream f(argv[2], std::ios::binary | std::ios::ate);
    double file_size = static_cast<double>(f.tellg());

    std::cout << "File:          " << argv[2] << "\n";
    std::cout << "Dimensions:    X=" << reader.dim_x()
              << " Y=" << reader.dim_y()
              << " Z=" << reader.dim_z() << "\n";
    std::cout << "Block size:    " << reader.block_size() << "\n";
    std::cout << "Total blocks:  " << reader.total_blocks() << "\n";
    std::cout << "Data offset:   " << reader.data_offset() << "\n";
    std::cout << "Blocks layout: " << lay.blocks_x << "x" << lay.blocks_y
              << "x" << lay.blocks_z << "\n";
    std::cout << "Storage ratio: " << (file_size / raw_size) << "x\n";
    return 0;
}

// ── Command: bench ───────────────────────────────────────────────────

static int cmd_bench(int argc, char* argv[]) {
    if (argc < 3) { std::cerr << "Missing file argument\n"; return 1; }
    const char* path = argv[2];
    int num_reads = get_arg_int(argc, argv, "--num-reads", 100);
    bool random_mode = has_arg(argc, argv, "--random");
    int nt = get_arg_int(argc, argv, "--threads", 0);
    uint64_t mem_mb = get_arg_uint64(argc, argv, "--memory-limit", 0);
    bool warm_up_flag = has_arg(argc, argv, "--warm-up");

    BlockedFileReader reader(path, nt, mem_mb);
    XorShift32 rng(42);

    std::cout << "Benchmarking: " << path << "\n";
    std::cout << "Dimensions: X=" << reader.dim_x() << " Y=" << reader.dim_y()
              << " Z=" << reader.dim_z() << "\n";
    std::cout << "Threads: " << reader.num_threads() << "\n\n";

    if (warm_up_flag) {
        std::cout << "Warming up OS page cache ..." << std::flush;
        auto tw0 = std::chrono::high_resolution_clock::now();
        reader.warm_up(false);
        auto tw1 = std::chrono::high_resolution_clock::now();
        double wdt = std::chrono::duration<double>(tw1 - tw0).count();
        std::cout << " done in " << wdt << "s\n\n";
    }

    struct AxisInfo { char name; uint64_t dim; };
    AxisInfo axes[] = {{'x', reader.dim_x()},
                       {'y', reader.dim_y()},
                       {'z', reader.dim_z()}};

    double results[3] = {0};

    for (int a = 0; a < 3; a++) {
        char axis = axes[a].name;
        uint64_t dim = axes[a].dim;

        std::vector<uint64_t> indices;
        if (random_mode) {
            for (int i = 0; i < num_reads; i++)
                indices.push_back(rng.rand_u64_mod(dim));
        } else {
            uint64_t step = std::max<uint64_t>(1, dim / num_reads);
            for (uint64_t i = 0; i < dim && indices.size() <
                 static_cast<size_t>(num_reads); i += step)
                indices.push_back(i);
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        auto slices = reader.read_slices_batch(axis, indices, nt);
        auto t1 = std::chrono::high_resolution_clock::now();

        double dt = std::chrono::duration<double>(t1 - t0).count();
        double avg_ms = dt / indices.size() * 1000.0;
        double tput = indices.size() / dt;
        results[a] = avg_ms;

        std::string desc = random_mode ? "Random" : "Sequential";
        std::cout << "  " << desc << " " << static_cast<char>(std::toupper(axis))
                  << "-slice (" << indices.size() << "x): "
                  << dt << "s total, " << avg_ms << " ms avg, "
                  << tput << " slices/s\n";
    }

    double avg_all = (results[0] + results[1] + results[2]) / 3.0;
    double max_r = std::max(results[0], std::max(results[1], results[2]));
    double min_r = std::min(results[0], std::min(results[1], results[2]));
    std::cout << "\n  Average across axes: " << avg_all << " ms\n";
    std::cout << "  Balance (max/min): " << (max_r / min_r) << "x\n";

    return 0;
}

// ── Command: verify ──────────────────────────────────────────────────

static int cmd_verify(int argc, char* argv[]) {
    // Argument order is <b3d> <raw>: the blocked file is the first positional
    // argument, consistent with every other subcommand (info/bench/extract all
    // take the .b3d first). The raw file is the second positional argument and
    // is only needed for cross-checking. Earlier the order was <raw> <b3d>,
    // which was inconsistent and caused users to swap the files; opening the
    // raw file as a .b3d then threw an uncaught exception that aborted the
    // process via Windows fast-fail (0xC0000409) with no output.
    if (argc < 4) { std::cerr << "Usage: verify <b3d> <raw> [--samples N]\n"; return 1; }
    const char* b3d_path = argv[2];
    const char* raw_path = argv[3];
    int samples = get_arg_int(argc, argv, "--samples", 10000);

    BlockedFileReader reader(b3d_path);
    std::cout << "Verifying " << samples << " random points...\n";
    auto t0 = std::chrono::high_resolution_clock::now();
    bool ok = reader.verify(raw_path, samples);
    auto t1 = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();

    if (ok) {
        std::cout << "PASSED in " << dt << "s\n";
        return 0;
    } else {
        std::cout << "FAILED! Data mismatch detected.\n";
        return 1;
    }
}

// ── Command: extract ─────────────────────────────────────────────────

static int cmd_extract(int argc, char* argv[]) {
    if (argc < 3) { std::cerr << "Missing file argument\n"; return 1; }
    const char* path = argv[2];
    const char* output = get_arg(argc, argv, "-o", nullptr);
    const char* axis = get_arg(argc, argv, "--axis", nullptr);
    const char* col_axis = get_arg(argc, argv, "--column-axis", nullptr);

    BlockedFileReader reader(path);

    auto write_raw = [&](const std::vector<float>& data,
                         const std::string& def_name) {
        std::string out_path = output ? output : def_name;
        std::ofstream f(out_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                data.size() * sizeof(float));
        std::cout << "Saved to: " << out_path << " ("
                  << data.size() << " floats)\n";
    };

    if (axis) {
        int idx = get_arg_int(argc, argv, "--index", -1);
        if (idx < 0) { std::cerr << "Missing --index\n"; return 1; }
        auto data = reader.read_slice(axis[0], static_cast<uint64_t>(idx));
        write_raw(data, "slice_" + str_axis(axis[0]) + std::to_string(idx) + ".raw");
    } else if (col_axis) {
        int c1 = get_arg_int(argc, argv, "--c1", -1);
        int c2 = get_arg_int(argc, argv, "--c2", -1);
        if (c1 < 0 || c2 < 0) { std::cerr << "Missing --c1 and --c2\n"; return 1; }
        std::vector<float> data;
        if (col_axis[0] == 'x')
            data = reader.read_x_column(c1, c2);
        else if (col_axis[0] == 'y')
            data = reader.read_y_column(c1, c2);
        else
            data = reader.read_z_column(c1, c2);
        write_raw(data, "col_" + str_axis(col_axis[0])
                  + "_" + std::to_string(c1) + "_" + std::to_string(c2) + ".raw");
    } else {
        std::cerr << "Specify --axis or --column-axis\n"; return 1;
    }
    return 0;
}

// ── Main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    std::string cmd = argv[1];

    // All subcommands can throw std::runtime_error / std::out_of_range
    // (file not found, invalid magic, dimension out of range, mmap failure,
    // ...). Without a catch, such exceptions are uncaught -> std::terminate
    // -> on Windows a fast-fail (0xC0000409 "stack buffer overrun") with no
    // output, which looks like a crash rather than a clean error. Catch here
    // and report the message with a non-zero exit code so bad input (e.g.
    // wrong argument order / missing file / corrupt file) fails gracefully.
    try {
        if (cmd == "convert")  return cmd_convert(argc, argv);
        if (cmd == "info")     return cmd_info(argc, argv);
        if (cmd == "bench")    return cmd_bench(argc, argv);
        if (cmd == "verify")   return cmd_verify(argc, argv);
        if (cmd == "extract")  return cmd_extract(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage(argv[0]);
    return 1;
}
