// Throughput and latency benchmarks for the LSM-tree key-value store.
//
// Each benchmark runs N operations and reports:
//   ops/s   total throughput
//   p50     median per-op latency (µs)
//   p95     95th-percentile latency
//   p99     99th-percentile latency
//
// Run from the project root after building:
//   build\Release\bench.exe

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>
#include "db.h"

namespace fs = std::filesystem;
using Clk = std::chrono::high_resolution_clock;
using Us  = std::chrono::duration<double, std::micro>;
using Ms  = std::chrono::duration<double, std::milli>;

// ── key / value generators ───────────────────────────────────────────────────

static std::string fmtkey(int i) {
    char b[24]; snprintf(b, sizeof(b), "k%015d", i); return b;
}

static std::string fmtval(int i) {
    // ~100-byte value — realistic for a small row
    char b[104]; snprintf(b, sizeof(b), "v%015d_%080d", i, i); return b;
}

// ── per-benchmark result accumulator ─────────────────────────────────────────

struct Bench {
    std::vector<double> us;

    void record(Clk::time_point t0, Clk::time_point t1) {
        us.push_back(Us(t1 - t0).count());
    }

    double pct(double p) const {
        auto s = us;
        std::sort(s.begin(), s.end());
        return s.at(static_cast<size_t>(p / 100.0 * (s.size() - 1)));
    }

    double ops_per_sec(double wall_ms) const {
        return us.size() / (wall_ms / 1000.0);
    }
};

// ── output helpers ────────────────────────────────────────────────────────────

static constexpr int W = 72;

static void print_header(int n) {
    std::cout << "\n  kvstore benchmark  (N=" << n
              << "  key≈16 B  val≈100 B)\n";
    std::cout << "  " << std::string(W, '-') << "\n";
    std::cout << "  " << std::left  << std::setw(32) << "benchmark"
              << std::right
              << std::setw(12) << "ops/s"
              << std::setw(10) << "p50 µs"
              << std::setw(10) << "p95 µs"
              << std::setw(10) << "p99 µs"
              << "\n";
    std::cout << "  " << std::string(W, '-') << "\n";
}

static void print_row(const std::string& name, Bench& b, double wall_ms) {
    std::cout << "  " << std::left  << std::setw(32) << name
              << std::right << std::fixed
              << std::setw(12) << std::setprecision(0) << b.ops_per_sec(wall_ms)
              << std::setw(10) << std::setprecision(1) << b.pct(50)
              << std::setw(10) << b.pct(95)
              << std::setw(10) << b.pct(99)
              << "\n";
}

static void print_note(const std::string& note) {
    std::cout << "  " << std::string(W, ' ').replace(0, note.size() + 2, "  " + note)
              << "\n";
}

// ── benchmarks ────────────────────────────────────────────────────────────────

// 1. Sequential writes — WAL append + MemTable insert, no disk flush
static void bench_seq_write(const fs::path& root, int n) {
    DB::Options o;
    o.memtable_flush_bytes = 256 << 20; // never auto-flush
    o.compaction_threshold = 999;
    DB db(root / "seqw", o);

    Bench b;
    auto wall = Clk::now();
    for (int i = 0; i < n; ++i) {
        auto t0 = Clk::now();
        db.put(fmtkey(i), fmtval(i));
        b.record(t0, Clk::now());
    }
    print_row("seq write", b, Ms(Clk::now() - wall).count());
}

// 2. Writes with automatic flush — triggers WAL seal + SSTable build + MANIFEST
static void bench_flush_write(const fs::path& root, int n) {
    DB::Options o;
    o.memtable_flush_bytes = 4 << 10; // 4 KB threshold → many flushes
    o.compaction_threshold = 999;
    DB db(root / "flushw", o);

    Bench b;
    auto wall = Clk::now();
    for (int i = 0; i < n; ++i) {
        auto t0 = Clk::now();
        db.put(fmtkey(i), fmtval(i));
        b.record(t0, Clk::now());
    }
    print_row("write + flush (4 KB)", b, Ms(Clk::now() - wall).count());
    print_note("↑ includes WAL fsync, SSTable build, MANIFEST rename");
}

// 3. Hot reads — all keys live in MemTable (pure in-memory std::map lookup)
static void bench_hot_read(const fs::path& root, int n) {
    DB::Options o;
    o.memtable_flush_bytes = 256 << 20;
    DB db(root / "hotr", o);
    for (int i = 0; i < n; ++i) db.put(fmtkey(i), fmtval(i));

    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, n - 1);

    Bench b;
    auto wall = Clk::now();
    for (int i = 0; i < n; ++i) {
        auto t0 = Clk::now();
        db.get(fmtkey(dist(rng)));
        b.record(t0, Clk::now());
    }
    print_row("read  (MemTable hit)", b, Ms(Clk::now() - wall).count());
}

// 4. Cold reads — MemTable empty, data in one compacted SSTable on disk
static void bench_cold_read(const fs::path& root, int n) {
    DB::Options o;
    o.memtable_flush_bytes = 4 << 10;
    o.compaction_threshold = 4;
    DB db(root / "coldr", o);
    for (int i = 0; i < n; ++i) db.put(fmtkey(i), fmtval(i));
    db.flush_memtable();
    db.compact(); // single SSTable, MemTable empty

    std::mt19937 rng(2);
    std::uniform_int_distribution<int> dist(0, n - 1);

    Bench b;
    auto wall = Clk::now();
    for (int i = 0; i < n; ++i) {
        auto t0 = Clk::now();
        db.get(fmtkey(dist(rng)));
        b.record(t0, Clk::now());
    }
    print_row("read  (SSTable, Bloom hit)", b, Ms(Clk::now() - wall).count());
    print_note("↑ Bloom passes → sparse-index seek → single disk read");
}

// 5. Miss reads — key definitely absent; Bloom filter returns false → no disk I/O
static void bench_bloom_miss(const fs::path& root, int n) {
    DB::Options o;
    o.memtable_flush_bytes = 4 << 10;
    o.compaction_threshold = 4;
    DB db(root / "bloomm", o);
    for (int i = 0; i < n; ++i) db.put(fmtkey(i), fmtval(i));
    db.flush_memtable();
    db.compact();

    Bench b;
    auto wall = Clk::now();
    for (int i = 0; i < n; ++i) {
        // "zz_" prefix guarantees these keys are absent (lexicographically after all stored keys)
        auto t0 = Clk::now();
        db.get("zz_miss_" + std::to_string(i));
        b.record(t0, Clk::now());
    }
    print_row("read  (Bloom miss, 0 disk I/O)", b, Ms(Clk::now() - wall).count());
    print_note("↑ Bloom returns false → SSTable file skipped entirely");
}

// 6. Delete — tombstone write, same codepath as put
static void bench_delete(const fs::path& root, int n) {
    DB::Options o;
    o.memtable_flush_bytes = 256 << 20;
    DB db(root / "del", o);
    for (int i = 0; i < n; ++i) db.put(fmtkey(i), fmtval(i));

    Bench b;
    auto wall = Clk::now();
    for (int i = 0; i < n; ++i) {
        auto t0 = Clk::now();
        db.del(fmtkey(i));
        b.record(t0, Clk::now());
    }
    print_row("delete (tombstone)", b, Ms(Clk::now() - wall).count());
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    const int N = 10'000;
    fs::path root = "bench_tmp";
    fs::remove_all(root);

    print_header(N);
    bench_seq_write(root, N);
    bench_flush_write(root, N);
    std::cout << "\n";
    bench_hot_read(root, N);
    bench_cold_read(root, N);
    bench_bloom_miss(root, N);
    std::cout << "\n";
    bench_delete(root, N);

    std::cout << "  " << std::string(W, '-') << "\n\n";
    fs::remove_all(root);
    return 0;
}
