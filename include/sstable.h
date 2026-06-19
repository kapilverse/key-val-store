#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "bloom.h"
#include "memtable.h"

struct SSTEntry {
    std::string key, value;
    bool tombstone = false;
};

struct IndexPoint {
    std::string key;
    uint64_t    offset = 0;
};

// Result of looking up a key in a single SSTable.
// We need three states because a tombstone in a newer SSTable must shadow
// a live value in an older one — returning nullopt alone can't express that.
struct LookupResult {
    enum class Tag { NotFound, Tombstone, Found };
    Tag         tag = Tag::NotFound;
    std::string value;

    static LookupResult not_found()        { return {Tag::NotFound, {}}; }
    static LookupResult tombstone()        { return {Tag::Tombstone, {}}; }
    static LookupResult found(std::string v) { return {Tag::Found, std::move(v)}; }
};

// Immutable sorted file on disk.
//
// File layout (v2 — with Bloom filter):
//   Data entries (sorted by key):
//     [key_len:4][key][flags:1][val_len:4][val]
//     flags bit 0 = 1 → tombstone
//   Bloom filter section:
//     [num_bits:4][bytes: ceil(num_bits/8)]
//   Index section:
//     [count:4] { [key_len:4][key][data_offset:8] } × count
//     One entry every INDEX_STRIDE data entries (sparse index).
//   Footer (16 bytes):
//     [bloom_section_offset:8][index_section_offset:8]
class SSTable {
public:
    static constexpr int INDEX_STRIDE = 16;

    static void build(const std::filesystem::path& path,
                      const std::map<std::string, MemEntry>& entries);

    explicit SSTable(const std::filesystem::path& path);

    LookupResult lookup(const std::string& key) const;

    // Returns all entries in order (used during compaction).
    std::vector<SSTEntry> scan_all() const;

    const std::filesystem::path& path() const { return path_; }
    bool bloom_may_contain(const std::string& key) const;

private:
    std::filesystem::path   path_;
    std::vector<IndexPoint> index_;
    uint64_t                bloom_offset_ = 0;
    uint64_t                index_offset_ = 0;
    BloomFilter             bloom_{0};     // loaded from file

    void load_meta();   // reads footer + bloom + index
};
