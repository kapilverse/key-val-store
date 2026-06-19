#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

// Bloom filter sized for a given number of keys at ~1% false-positive rate.
//
// Uses double hashing — k probes per key, each probe is (h1 + i*h2) % m.
// m is rounded up to the next power of two so probe arithmetic is a fast mask.
//
// "May contain" returns false  → key is DEFINITELY absent  (skip SSTable I/O)
// "May contain" returns true   → key is POSSIBLY present    (check SSTable)
class BloomFilter {
public:
    static constexpr int    K   = 7;     // hash probes per key
    static constexpr double FPR = 0.01;  // target false-positive rate

    // Create a fresh filter sized for n keys.
    explicit BloomFilter(size_t n);

    // Deserialize from raw bytes loaded from an SSTable file.
    BloomFilter(const uint8_t* data, size_t byte_len, uint32_t num_bits);

    void add(std::string_view key);
    bool may_contain(std::string_view key) const;

    const std::vector<uint8_t>& raw_bytes() const { return bits_; }
    uint32_t num_bits() const { return static_cast<uint32_t>(m_); }

private:
    size_t               m_;     // total bits (power of two)
    std::vector<uint8_t> bits_;  // packed bit array

    void   set_bit(uint64_t i);
    bool   get_bit(uint64_t i) const;
    void   probe_positions(std::string_view key, uint64_t* pos) const;
};
