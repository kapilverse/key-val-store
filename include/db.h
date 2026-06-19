#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>
#include "memtable.h"
#include "wal.h"

class SSTable;

struct DBStats {
    size_t   memtable_entries  = 0;
    size_t   memtable_bytes    = 0;
    size_t   sstable_count     = 0;
    size_t   wal_replayed      = 0;   // entries replayed on startup
    uint64_t active_wal_id     = 0;   // current WAL segment number
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
    size_t   wal_replayed_   = 0;

    void recover();
    void flush_memtable_locked();   // assumes unique lock held
    void compact_locked();          // assumes unique lock held
    void maybe_flush();             // called inside write path (lock already held)
    std::filesystem::path sst_path(uint64_t id) const;
    std::filesystem::path wal_seg_path(uint64_t id) const;
};
