# key-val

A from-scratch LSM-tree key-value store in C++17. Implements the core of engines like LevelDB/RocksDB: write-ahead log, sorted MemTable, immutable SSTables with Bloom filters, atomic MANIFEST, and crash-safe compaction.

---

## Features

- **Write-Ahead Log (WAL)** — every mutation is fsynced before the MemTable is touched; no acknowledged write is ever lost
- **MemTable** — in-memory `std::map`, sorted, with tombstone support for deletes
- **SSTables** — immutable sorted files with a sparse index and an embedded Bloom filter
- **Bloom filter** — double-hashing, k=7, ~1% FPR; eliminates disk I/O for absent keys
- **MANIFEST** — atomic SSTable registry (tmp → fsync → rename); deterministic recovery
- **Crash-safe flush** — SSTable durability committed via MANIFEST rename before WAL segment is sealed
- **Compaction** — merges all SSTables into one, dropping tombstones and reclaiming space
- **Concurrent reads** — `std::shared_mutex` lets multiple `get` calls run in parallel

---

## Build

Requires CMake 3.15+ and a C++17 compiler (MSVC, GCC, or Clang).

```bash
cmake -S . -B build
cmake --build build --config Release
```

This produces three executables under `build/Release/`:

| Executable | Description |
|---|---|
| `kvstore` | Interactive REPL |
| `bench` | Throughput and latency benchmarks |
| `test_crash` | Crash-recovery test suite |

---

## Usage

```bash
./build/Release/kvstore [data-dir]
```

`data-dir` defaults to `./kvdata`. The directory is created on first run.

```
kvstore  —  LSM-tree key-value store
data dir : ./kvdata

> put name Alice
OK
> get name
Alice
> del name
OK
> get name
(not found)
> stats
memtable : 0 entries, 0 bytes
sstables : 1 file(s)
manifest : generation 2
wal seg  : wal_000001.log
recovery : 0 WAL entries replayed
> flush
OK  (sstables now: 1)
> compact
OK  (sstables now: 1)
> help
  put <key> <value>   write a key
  get <key>           read a key
  del <key>           delete a key
  stats               show engine internals
  flush               force MemTable → SSTable
  compact             merge all SSTables into one
  help                this message
  exit / quit         close
```

---

## Configuration

`DB::Options` controls two thresholds:

```cpp
struct Options {
    size_t memtable_flush_bytes = 64 * 1024;  // flush MemTable at this size
    size_t compaction_threshold = 4;           // compact when this many SSTables exist
};
```

The REPL sets `memtable_flush_bytes = 4 KB` so flushes are visible during interactive use. For production workloads, the default 64 KB is more appropriate.

---

## Benchmarks

```bash
./build/Release/bench
```

Example output (Windows 11, NVMe SSD, N=10,000, key≈16 B, val≈100 B):

```
  benchmark                        ops/s   p50 µs   p95 µs   p99 µs
  ------------------------------------------------------------------------
  seq write                         1022    954.2   1267.6   1467.8
  write + flush (4 KB)               355   1027.6   7630.8  54561.5

  read  (MemTable hit)            717422      1.2      1.7      2.3
  read  (SSTable, Bloom hit)        7137    123.0    215.2    312.0
  read  (Bloom miss, 0 disk I/O)   65414      0.3    130.3    211.1

  delete (tombstone)                1027    948.9   1247.4   1435.4
```

Write latency is dominated by WAL `fsync` (~1 ms/op). MemTable reads are ~100× faster than SSTable reads. Bloom filter misses skip all disk I/O; p50 is 0.3 µs.

---

## Crash-Recovery Tests

```bash
./build/Release/test_crash
```

Four scenarios, each using `_exit()` to bypass destructors and simulate a hard crash:

```
  [PASS]  WAL replay: 1000/1000 keys recovered after simulated crash
  [PASS]  SSTable+WAL: 500 from SSTable, 500 from WAL replay
  [PASS]  orphan cleanup: orphan SSTable removed, 100 live keys intact
  [PASS]  partial WAL: torn record silently dropped, valid key intact

  4/4 tests passed
```

Tests are also registered with CTest:

```bash
ctest --test-dir build -C Release
```

---

## Project Structure

```
key-val/
├── include/
│   ├── bloom.h          Bloom filter (double hashing, k=7, ~1% FPR)
│   ├── db.h             Public DB API, Options, DBStats
│   ├── manifest.h       MANIFEST read/write (atomic rename protocol)
│   ├── memtable.h       In-memory sorted map with tombstone support
│   ├── sstable.h        SSTable build/lookup, file format, LookupResult
│   └── wal.h            Write-Ahead Log segment, binary record format
├── src/
│   ├── bloom.cpp
│   ├── db.cpp           Core engine: recover, flush, compact, get/put/del
│   ├── main.cpp         Interactive REPL
│   ├── manifest.cpp
│   ├── memtable.cpp
│   ├── sstable.cpp
│   ├── wal.cpp
│   ├── bench.cpp        Throughput + latency benchmarks
│   └── test_crash.cpp   Crash-recovery test suite
├── CMakeLists.txt
├── ARCHITECTURE.md      Deep dive: file formats, data paths, durability guarantees
└── README.md
```

---

## Further Reading

See [ARCHITECTURE.md](ARCHITECTURE.md) for:
- Byte-level file format specifications (SSTable layout, WAL record format)
- Step-by-step flush durability contract with crash-scenario analysis
- Full recovery sequence
- WAL segment lifecycle diagram
- Compaction commit-point logic
- Thread safety model

  See [GUIDE.md](GUIDE.md) for:
- How to start this project
- How to perform operations in this Project.
