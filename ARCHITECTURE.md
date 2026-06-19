# Architecture

A from-scratch LSM-tree key-value store written in C++17. Supports `put`, `get`, `del` with WAL-based durability, Bloom-filtered SSTable reads, MANIFEST-tracked SSTable inventory, and crash-safe compaction.

---

## Table of Contents

1. [Components](#1-components)
2. [Directory Layout](#2-directory-layout)
3. [Write Path](#3-write-path)
4. [Read Path](#4-read-path)
5. [File Formats](#5-file-formats)
6. [Flush — Durability Contract](#6-flush--durability-contract)
7. [WAL Segment Lifecycle](#7-wal-segment-lifecycle)
8. [Recovery on Startup](#8-recovery-on-startup)
9. [Compaction](#9-compaction)
10. [Thread Safety](#10-thread-safety)
11. [Benchmarks](#11-benchmarks)

---

## 1. Components

```
┌──────────────────────────────────────────────────┐
│                   DB  (db.h)                     │
│  put · get · del · flush_memtable · compact       │
│  shared_mutex (readers share, writers exclusive) │
└────────────┬──────────────────┬──────────────────┘
             │                  │
     ┌───────▼──────┐   ┌───────▼──────────────┐
     │  MemTable    │   │  WAL segment         │
     │  (memtable.h)│   │  (wal.h)             │
     │  std::map    │   │  wal_000003.log      │
     │  sorted, RAM │   │  binary append-only  │
     └───────┬──────┘   └──────────────────────┘
             │ flush
     ┌───────▼──────────────────────────────────┐
     │  SSTables  (sstable.h)  newest → oldest  │
     │  sst_000003.sst  sst_000002.sst  …       │
     │  each: data | bloom | sparse index | footer│
     └──────────────────────┬───────────────────┘
                            │ update on flush/compact
                    ┌───────▼────────────┐
                    │  MANIFEST          │
                    │  (manifest.h)      │
                    │  live SSTable list │
                    │  atomic write      │
                    └────────────────────┘
```

### MemTable
In-memory sorted map (`std::map<string, MemEntry>`). Supports tombstones. All writes land here first (after WAL sync). Flushed to disk when `memtable_bytes >= memtable_flush_bytes`.

### WAL (Write-Ahead Log)
One active binary segment per DB instance. Every `put`/`del` is appended and fsynced here before the MemTable is touched. Segment is rotated (sealed) when the MemTable flushes.

### SSTable (Sorted String Table)
Immutable sorted file on disk. Contains a data section, embedded Bloom filter, sparse index, and a 16-byte footer. Built during flush, never modified after creation. Stored newest-first in `sstables_`.

### Bloom Filter
Double-hashing (FNV1a variant A and B), `k=7` probes, `~1% FPR`. Bit array sized to next power of two of the theoretical optimum. Stored inside each SSTable. Eliminates disk I/O for absent keys.

### MANIFEST
Plain-text file listing all live SSTables by basename, newest-first. Written atomically (`MANIFEST.tmp` → fsync → rename). The canonical source of truth for which SSTables exist. Orphaned `.sst` files (not in MANIFEST) are deleted on startup.

---

## 2. Directory Layout

```
kvdata/
├── MANIFEST                  authoritative live SSTable list
├── MANIFEST.tmp              only during an atomic write (deleted on startup)
├── wal_000004.log            active WAL segment (replayed on crash)
├── wal_000003.log.done       sealed WAL segment (safe to delete, no replay)
├── sst_000003.sst            newest SSTable
├── sst_000002.sst
└── sst_000001.sst            oldest SSTable
```

**Naming conventions**

| Pattern | Description |
|---|---|
| `wal_NNNNNN.log` | Active WAL segment — replayed on recovery |
| `wal_NNNNNN.log.done` | Sealed WAL segment — deleted on recovery without replay |
| `sst_NNNNNN.sst` | SSTable file — only those listed in MANIFEST are loaded |
| `MANIFEST` | Live SSTable registry |
| `MANIFEST.tmp` | In-progress MANIFEST write — leftover means crash mid-write |

---

## 3. Write Path

```
DB::put(key, value)
  │
  ├─ 1. acquire unique_lock
  ├─ 2. WAL::append()           write record to OS buffer
  ├─ 3. WAL::sync()             fflush + fsync  ←  durable from here
  ├─ 4. MemTable::put()         update sorted map in RAM
  └─ 5. maybe_flush()           if bytes ≥ threshold → flush_memtable_locked()
                                if sstable_count ≥ threshold → compact_locked()
```

`del(key)` follows the same path with `WalOp::Del` and `MemTable::del()`, which inserts a tombstone entry.

---

## 4. Read Path

```
DB::get(key)
  │
  ├─ 1. acquire shared_lock  (concurrent readers do not block each other)
  ├─ 2. MemTable::raw(key)
  │      ├─ tombstone entry  → return nullopt
  │      ├─ live entry       → return value
  │      └─ absent           → continue
  └─ 3. for each SSTable (newest → oldest):
           ├─ Bloom::may_contain(key)
           │    └─ false  → skip entirely (no disk I/O)
           ├─ binary search sparse index → nearest data offset
           ├─ fseek + scan ≤ INDEX_STRIDE (16) entries
           └─ LookupResult:
                 NotFound  → continue to next SSTable
                 Tombstone → return nullopt  (stop searching)
                 Found     → return value    (stop searching)
```

The three-state `LookupResult` (NotFound / Tombstone / Found) is critical: a tombstone in a *newer* SSTable must shadow a live value in an *older* one. A two-state `optional<string>` cannot express this distinction.

---

## 5. File Formats

### SSTable (`sst_NNNNNN.sst`)

```
┌────────────────────────────────────────────────── offset 0
│  DATA SECTION
│  ┌──────────┬────────┬───────┬──────────┬───────┐
│  │ key_len  │  key   │ flags │ val_len  │ value │  × N entries
│  │  4 bytes │ k_len B│ 1 byte│  4 bytes │ v_lenB│
│  └──────────┴────────┴───────┴──────────┴───────┘
│  flags bit 0 = 1 → tombstone (val_len is present but ignored)
│  entries stored in ascending key order
│
├────────────────────────────────────────────────── offset = bloom_offset
│  BLOOM FILTER SECTION
│  ┌──────────┬────────────────────────────┐
│  │ num_bits │ packed bit array           │
│  │  4 bytes │ ceil(num_bits / 8) bytes   │
│  └──────────┴────────────────────────────┘
│  k=7 probes, FPR ≈ 1%, m = next power-of-two of ceil(-n·ln(p)/ln(2)²)
│
├────────────────────────────────────────────────── offset = index_offset
│  SPARSE INDEX SECTION
│  ┌───────┬──────────────────────────────────────┐
│  │ count │ { key_len, key, data_offset:8 } × N  │
│  │4 bytes│   one entry per INDEX_STRIDE=16 rows  │
│  └───────┴──────────────────────────────────────┘
│  binary-searchable; each entry points to a data-section offset
│
└────────────────────────────────────────────────── EOF - 16 bytes
   FOOTER (always last 16 bytes)
   ┌────────────────────────┬────────────────────────┐
   │ bloom_section_offset   │ index_section_offset   │
   │       8 bytes LE       │       8 bytes LE       │
   └────────────────────────┴────────────────────────┘
```

`load_meta()` seeks to EOF−16, reads the footer, then loads the Bloom filter and index into memory. Lookups check the Bloom filter first (in RAM), then use the index to bound the disk seek.

### WAL Record (`wal_NNNNNN.log`)

Each record is variable length, appended sequentially:

```
┌──────┬──────────┬─────────┬──────────┬─────────┐
│  op  │ key_len  │   key   │ val_len  │  value  │
│ 1 B  │  4 B LE  │ k_len B │  4 B LE  │ v_len B │
└──────┴──────────┴─────────┴──────────┴─────────┘

op: 0x00 = Put,  0x01 = Del
```

`replay()` reads until `fread` returns 0 mid-record. A truncated final record (torn write) is silently dropped — all complete records before it are replayed successfully.

---

## 6. Flush — Durability Contract

`flush_memtable_locked()` executes these steps in order:

| Step | Action | Notes |
|------|--------|-------|
| 1 | Build SSTable + fsync | Data section, Bloom, index, footer written; `_commit`/`fsync` called |
| 2 | **MANIFEST atomic write** | **← commit point.** MANIFEST.tmp → fsync → rename. After this rename, the SSTable is officially live |
| 3 | Seal WAL segment | `rename(wal_N.log → wal_N.log.done)`. Data is in the SSTable; segment will be deleted on recovery without replay |
| 4 | Open next WAL segment | New writes go to `wal_{N+1}.log` immediately |
| 5 | Clear MemTable | Safe only after SSTable and MANIFEST commit are done |
| 6 | Delete `.done` file | Lazy cleanup — safe to crash before this |

**Crash scenarios:**

- *Crash after step 1, before step 2:* SSTable on disk but not in MANIFEST → orphan, deleted on recovery. No data loss (MemTable was not cleared, WAL still active).
- *Crash after step 2, before step 3:* MANIFEST points to the new SSTable; WAL is still active. Recovery replays WAL — harmless duplicates; newer MemTable writes overwrite older SSTable entries.
- *Crash after step 3:* WAL is sealed. Recovery deletes the `.done` file and does not replay it.

---

## 7. WAL Segment Lifecycle

```
  ACTIVE: wal_000003.log
  │  append(op, key, val) — writes to OS buffer
  │  sync()               — fflush + fsync, durable
  │  [crash here]         → replayed on recovery
  │
  ▼  (MemTable threshold reached → flush)
  SSTable built + MANIFEST committed
  │
  ▼  rename wal_000003.log → wal_000003.log.done
  SEALED: wal_000003.log.done
  │  [crash here]         → deleted on recovery WITHOUT replay
  │
  ▼  lazy delete after new WAL is open
  (gone)
```

Recovery sees `.log` → replay it. Recovery sees `.log.done` → delete it without replay.

---

## 8. Recovery on Startup

`DB::recover()` runs in the constructor before any user operation:

```
1. Delete MANIFEST.tmp        (leftover from a partial atomic write;
                               the previous MANIFEST is still intact)

2. Delete *.log.done files    (sealed segments whose data is already in
                               an SSTable; no replay needed)

3. Load SSTables from MANIFEST
   └─ fallback: scan directory for *.sst if no MANIFEST exists
      (first ever start or pre-MANIFEST build)

4. Delete orphan *.sst files  (on disk but not listed in MANIFEST;
                               left by an interrupted compaction)

5. Replay *.log WAL files     (oldest-first, into MemTable;
   oldest → newest             a torn final record is silently dropped)

6. Open active WAL segment    (ID = max(found WAL IDs, next SSTable ID))
```

After step 6 the DB is ready to accept operations.

---

## 9. Compaction

`compact_locked()` merges all SSTables into one:

```
1. Flush MemTable             (so its entries participate and tombstones
                               in RAM shadow any on-disk values)

2. Merge all SSTables         oldest → newest; per-key, newer entry wins
   (drop tombstones)          safe because MemTable was flushed above —
                               nothing older remains to be shadowed

3. Build merged SSTable       new Bloom filter and index from scratch;
   + fsync                    only live entries (no tombstones)

4. MANIFEST update            ← commit point. New SSTable is authoritative.
                               Old SSTables are now orphans.

5. Delete old SSTable files   safe; they are no longer in MANIFEST.
                               A crash before step 4 leaves the new file as
                               an orphan (cleaned up on next startup).
```

Triggered automatically when `sstable_count >= compaction_threshold` after a flush. Also callable directly via `DB::compact()`.

---

## 10. Thread Safety

`DB` uses a single `std::shared_mutex`:

| Operation | Lock type | Concurrency |
|-----------|-----------|-------------|
| `get`, `stats` | `shared_lock` | Multiple concurrent readers |
| `put`, `del` | `unique_lock` | Exclusive; blocks all others |
| `flush_memtable`, `compact` | `unique_lock` | Exclusive |

Internal helpers (`flush_memtable_locked`, `compact_locked`, `maybe_flush`, `update_manifest_locked`) assume the appropriate lock is already held by the caller and do not re-acquire it.

---

## 11. Benchmarks

Measured on Windows 11, NVMe SSD, N=10,000 operations, key≈16 B, value≈100 B.

| Benchmark | ops/s | p50 µs | p95 µs | p99 µs |
|---|---|---|---|---|
| seq write | 1,022 | 954 | 1,268 | 1,468 |
| write + flush (4 KB threshold) | 355 | 1,028 | 7,631 | 54,562 |
| read — MemTable hit | 717,422 | 1.2 | 1.7 | 2.3 |
| read — SSTable (Bloom hit) | 7,137 | 123 | 215 | 312 |
| read — Bloom miss (no disk I/O) | 65,414 | 0.3 | 130 | 211 |
| delete (tombstone) | 1,027 | 949 | 1,247 | 1,435 |

Key observations:

- **Write latency** is dominated by WAL `fsync` (~1 ms/op). The p99 spike for write+flush (~54 ms) is the occasional SSTable build + MANIFEST rename on the write critical path.
- **MemTable reads** are pure `std::map` lookups (1–2 µs), ~100× faster than SSTable reads.
- **Bloom misses** skip all disk I/O; p50 is 0.3 µs. The elevated p95/p99 (~130–211 µs) comes from the ~1% false-positive rate hitting disk.
- **SSTable reads** require a sparse-index seek + ≤16-entry linear scan; median ~123 µs on a warm page cache.

Run benchmarks: `build\Release\bench.exe`  
Run crash-recovery tests: `build\Release\test_crash.exe`
