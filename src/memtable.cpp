#include "memtable.h"

void MemTable::put(const std::string& key, const std::string& value) {
    auto [it, inserted] = data_.emplace(key, MemEntry{value, false});
    if (!inserted) {
        bytes_ -= it->second.value.size();
        it->second = {value, false};
    } else {
        bytes_ += key.size();
    }
    bytes_ += value.size();
}

void MemTable::del(const std::string& key) {
    auto [it, inserted] = data_.emplace(key, MemEntry{"", true});
    if (!inserted) {
        bytes_ -= it->second.value.size();
        it->second = {"", true};
    } else {
        bytes_ += key.size();
    }
}

std::optional<std::string> MemTable::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end() || it->second.tombstone) return std::nullopt;
    return it->second.value;
}

const MemEntry* MemTable::raw(const std::string& key) const {
    auto it = data_.find(key);
    return it == data_.end() ? nullptr : &it->second;
}

void MemTable::clear() { data_.clear(); bytes_ = 0; }
