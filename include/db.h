#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>
#include "manifest.h"
#include "memtable.h"
#include "wal.h"

class SSTable;

// LSM-tree key-value store.
//
// Thread safety: all public methods are safe to call from multiple threads.
// put/del/flush/compact take an exclusive lock; get/stats take a shared lock
// (multiple readers run concurrently without blocking each other).
//
// Durability contract: put() and del() return only after the WAL record has
// been fsynced to disk.  A crash after the call returns will not lose the
// write.  A crash during the call may lose it (the write was never
// acknowledged to the caller).
//
// Crash-safe flush sequence (flush_memtable_locked):
//   1. Build SSTable + fsync            → data on disk
//   2. MANIFEST atomic write (rename)   ← commit point: SSTable is now live
//   3. Seal WAL: rename .log → .log.done  (data already in SSTable)
//   4. Open next WAL segment
//   5. Clear MemTable
//   6. Delete .done file (lazy)
//
// Recovery on constructor (recover()):
//   1. Remove MANIFEST.tmp (partial write from a previous run)
//   2. Delete .log.done files (sealed WAL segments — data already durable)
//   3. Load SSTable list from MANIFEST
//   4. Delete orphan .sst files not listed in MANIFEST
//   5. Replay active .log WAL files oldest-first into the MemTable
//   6. Open (or create) the active WAL segment

struct DBStats {
    size_t   memtable_entries  = 0;
    size_t   memtable_bytes    = 0;
    size_t   sstable_count     = 0;
    size_t   wal_replayed      = 0;   // entries replayed on startup
    uint64_t active_wal_id     = 0;   // current WAL segment number
    uint64_t manifest_gen      = 0;   // MANIFEST generation number
};

class DB {
public:
    struct Options {
        // Flush MemTable to SSTable once it exceeds this many bytes.
        size_t memtable_flush_bytes = 64 * 1024;
        // Trigger compaction once this many SSTable files accumulate.
        size_t compaction_threshold = 4;
    };

    explicit DB(const std::filesystem::path& dir, Options opts = {});
    ~DB();

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    void del(const std::string& key);

    DBStats stats() const;

    // Exposed so the REPL/tests can trigger these explicitly.
    void flush_memtable();
    void compact();

private:
    std::filesystem::path dir_;
    Options               opts_;

    mutable std::shared_mutex          mu_;
    MemTable                           memtable_;
    std::unique_ptr<WAL>               wal_;
    std::vector<std::shared_ptr<SSTable>> sstables_;  // newest first

    uint64_t next_sst_id_    = 0;
    uint64_t current_wal_id_ = 0;  // ID of the active wal_NNNNNN.log segment
    uint64_t manifest_gen_   = 0;  // monotonically increasing MANIFEST generation
    size_t   wal_replayed_   = 0;

    void recover();
    void flush_memtable_locked();     // assumes unique lock held
    void compact_locked();            // assumes unique lock held
    void maybe_flush();               // called inside write path (lock already held)
    void update_manifest_locked();    // writes current sstables_ to MANIFEST atomically
    std::filesystem::path sst_path(uint64_t id) const;
    std::filesystem::path wal_seg_path(uint64_t id) const;
};
