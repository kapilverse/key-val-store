#pragma once
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

enum class WalOp : uint8_t { Put = 0, Del = 1 };

struct WalRecord {
    WalOp op;
    std::string key, value;
};

// Append-only Write-Ahead Log segment.
//
// Binary record format (one record per Put / Del):
//   [op:1][key_len:4LE][key:key_len][val_len:4LE][val:val_len]
//
// Segment lifecycle:
//   Active   wal_N.log        — being written; replayed on crash recovery
//   Sealed   wal_N.log.done   — data is in an SSTable; delete without replay
//   Deleted                   — lazy cleanup after sealing
//
// Crash safety: DB::put()/del() call append() then sync() before modifying
// the MemTable.  If the process crashes after sync(), the record is on disk
// and will be replayed.  If it crashes before sync(), the record is lost
// (never reached the MemTable either, so the write was never acknowledged).
//
// append() does NOT call sync() — the caller decides when to fsync.
class WAL {
public:
    explicit WAL(const std::filesystem::path& path);
    ~WAL();
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    // Write a record to the file buffer.  Does NOT fsync.
    void append(WalOp op, std::string_view key, std::string_view value = {});

    // fflush + fsync — guarantees data is on physical storage.
    void sync();

    // Read every complete record and call cb for each.
    // A torn final record (crash mid-write) is silently dropped.
    void replay(const std::function<void(WalRecord)>& cb) const;

private:
    std::filesystem::path path_;
    FILE* file_ = nullptr;
};
