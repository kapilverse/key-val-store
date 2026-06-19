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

// Append-only Write-Ahead Log.
// Every mutation is written here before touching the MemTable so that
// the MemTable can be rebuilt from this file after a crash.
class WAL {
public:
    explicit WAL(const std::filesystem::path& path);
    ~WAL();
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    void append(WalOp op, std::string_view key, std::string_view value = {});

    // fflush + fsync — guarantees data is on physical storage.
    void sync();

    // Read every record back and call cb for each.
    void replay(const std::function<void(WalRecord)>& cb) const;

private:
    std::filesystem::path path_;
    FILE* file_ = nullptr;
};
