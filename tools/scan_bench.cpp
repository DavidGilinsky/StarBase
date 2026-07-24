// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tools/scan_bench.cpp
// Purpose:       Measure FITS header-read throughput against a real archive at
//                a range of concurrency levels, so the scanner's worker count is
//                chosen from data rather than guessed. Reads headers only (the
//                scanner's unit of work); never touches pixels.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fits_reader.hpp"

namespace {

using Clock = std::chrono::steady_clock;
namespace stdfs = std::filesystem;

// Read headers of files[begin..) cooperatively: each thread claims the next
// index atomically, so uneven per-file latency self-balances.
struct Result {
    long ok = 0;
    long err = 0;
};

Result run(const std::vector<std::string>& files, int threads) {
    std::atomic<size_t> next{0};
    std::atomic<long> ok{0}, err{0};
    auto worker = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= files.size()) break;
            try {
                auto h = starbase::fits::read_header(files[i]);
                (void)h;
                ok.fetch_add(1);
            } catch (const std::exception&) {
                err.fetch_add(1);
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    return {ok.load(), err.load()};
}

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: scan_bench <root-dir> [max-files] [threads]\n"
                  << "  With <threads>, does ONE run at that concurrency (drive from a\n"
                  << "  shell loop that drops caches between runs for cold numbers).\n"
                  << "  Without it, sweeps 1..32 threads on a warm cache.\n";
        return 2;
    }
    const std::string root = argv[1];
    const size_t max_files = (argc > 2) ? std::stoul(argv[2]) : 800;
    const int single_threads = (argc > 3) ? std::stoi(argv[3]) : 0;

    std::vector<std::string> files;
    std::error_code ec;
    for (stdfs::recursive_directory_iterator it(
             root, stdfs::directory_options::skip_permission_denied, ec), end;
         it != end && files.size() < max_files; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension().string();
        if (ext == ".fits" || ext == ".fit" || ext == ".fts")
            files.push_back(it->path().string());
    }
    if (files.empty()) { std::cerr << "no FITS files found\n"; return 1; }

    // Single-run mode: report just this concurrency level, so an external loop
    // can drop the page cache before each invocation and measure cold.
    if (single_threads > 0) {
        const auto t0 = Clock::now();
        const auto r = run(files, single_threads);
        const double s = seconds_since(t0);
        std::printf("threads=%-3d frames=%zu  %7.2fs  %8.1f frames/s  (err %ld)\n",
                    single_threads, files.size(), s, r.ok / s, r.err);
        return 0;
    }

    // Warm sweep (no cache control): shows the cache-hit ceiling, not the sweep.
    std::printf("sampled %zu frames; WARM sweep (cached):\n", files.size());
    std::printf("  %-8s %10s %14s %14s\n", "threads", "seconds", "frames/s", "speedup");
    double base = 0;
    for (int th : {1, 2, 4, 8, 16, 24, 32}) {
        const auto t0 = Clock::now();
        const auto r = run(files, th);
        const double s = seconds_since(t0);
        const double fps = r.ok / s;
        if (th == 1) base = fps;
        std::printf("  %-8d %10.2f %14.1f %13.2fx\n", th, s, fps, fps / base);
    }
    return 0;
}
