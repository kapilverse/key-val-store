#include "bloom.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

// ── bit helpers ──────────────────────────────────────────────────────────────

void BloomFilter::set_bit(uint64_t i) {
    bits_[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
}

bool BloomFilter::get_bit(uint64_t i) const {
    return (bits_[i >> 3] >> (i & 7)) & 1;
}

// ── hash functions ────────────────────────────────────────────────────────────
// Double hashing: probe[i] = (h1 + i * h2) & (m - 1)
// Uses two FNV-1a variants with different primes.

static uint64_t fnv1a_a(std::string_view s) {
    uint64_t h = 14695981039346656037ULL;
    for (uint8_t c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static uint64_t fnv1a_b(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint8_t c : s) { h ^= c; h *= 0x00000100000001b3ULL; }
    return h | 1;   // force odd so gcd(h2, 2^k) = 1 — covers all slots
}

void BloomFilter::probe_positions(std::string_view key, uint64_t* pos) const {
    uint64_t h1 = fnv1a_a(key);
    uint64_t h2 = fnv1a_b(key);
    uint64_t mask = static_cast<uint64_t>(m_) - 1;   // m_ is power of two
    for (int i = 0; i < K; ++i)
        pos[i] = (h1 + static_cast<uint64_t>(i) * h2) & mask;
}

// ── construction ──────────────────────────────────────────────────────────────

static size_t next_pow2(size_t v) {
    if (v == 0) return 1;
    --v;
    for (size_t i = 1; i < sizeof(v) * 8; i <<= 1) v |= v >> i;
    return v + 1;
}

BloomFilter::BloomFilter(size_t n) {
    // m = -n * ln(p) / (ln2)^2  rounded up to next power of two.
    double bits_exact = -static_cast<double>(n) * std::log(FPR)
                        / (std::log(2.0) * std::log(2.0));
    m_ = next_pow2(static_cast<size_t>(std::ceil(bits_exact)));
    if (m_ < 64) m_ = 64;   // minimum
    bits_.assign((m_ + 7) / 8, 0);
}

BloomFilter::BloomFilter(const uint8_t* data, size_t byte_len, uint32_t num_bits)
    : m_(next_pow2(num_bits)) {
    if ((m_ + 7) / 8 > byte_len)
        throw std::runtime_error("BloomFilter: truncated data");
    bits_.assign(data, data + (m_ + 7) / 8);
}

// ── public API ────────────────────────────────────────────────────────────────

void BloomFilter::add(std::string_view key) {
    uint64_t pos[K];
    probe_positions(key, pos);
    for (int i = 0; i < K; ++i) set_bit(pos[i]);
}

bool BloomFilter::may_contain(std::string_view key) const {
    uint64_t pos[K];
    probe_positions(key, pos);
    for (int i = 0; i < K; ++i)
        if (!get_bit(pos[i])) return false;
    return true;
}
