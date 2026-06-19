#pragma once
#include <map>
#include <optional>
#include <string>

struct MemEntry {
    std::string value;
    bool tombstone = false;
};

// In-memory sorted buffer for recent writes.
// std::map keeps keys sorted, which is required when flushing to an SSTable.
class MemTable {
public:
    void put(const std::string& key, const std::string& value);
    void del(const std::string& key);

    // Returns value if found and not deleted; nullopt otherwise.
    std::optional<std::string> get(const std::string& key) const;

    // Returns raw entry including tombstone flag (needed when flushing).
    const MemEntry* raw(const std::string& key) const;

    const std::map<std::string, MemEntry>& data() const { return data_; }
    size_t bytes() const { return bytes_; }
    size_t size()  const { return data_.size(); }
    bool   empty() const { return data_.empty(); }
    void   clear();

private:
    std::map<std::string, MemEntry> data_;
    size_t bytes_ = 0;
};
