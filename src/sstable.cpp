#include "sstable.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

// ── portable 64-bit seek / tell ──────────────────────────────────────────────

static int fseek64(FILE* f, int64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, static_cast<off_t>(off), whence);
#endif
}

static int64_t ftell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

// ── binary helpers ───────────────────────────────────────────────────────────

static void wu32(FILE* f, uint32_t v) { fwrite(&v, sizeof(v), 1, f); }
static void wu64(FILE* f, uint64_t v) { fwrite(&v, sizeof(v), 1, f); }

static void wstr(FILE* f, std::string_view s) {
    wu32(f, static_cast<uint32_t>(s.size()));
    fwrite(s.data(), 1, s.size(), f);
}

static bool ru32(FILE* f, uint32_t& v) { return fread(&v, sizeof(v), 1, f) == 1; }
static bool ru64(FILE* f, uint64_t& v) { return fread(&v, sizeof(v), 1, f) == 1; }

static bool rstr(FILE* f, std::string& s) {
    uint32_t len;
    if (!ru32(f, len)) return false;
    s.resize(len);
    return fread(s.data(), 1, len, f) == len;
}

// ── SSTable::build ───────────────────────────────────────────────────────────
// Layout written:
//   data entries → bloom section → index section → 16-byte footer

void SSTable::build(const std::filesystem::path& path,
                    const std::map<std::string, MemEntry>& entries) {
    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f) throw std::runtime_error("SSTable::build failed: " + path.string());

    // Build Bloom filter over all keys.
    BloomFilter bloom(entries.size());
    for (auto& [key, _] : entries)
        bloom.add(key);

    // Write sorted data entries, tracking one sparse-index point every stride.
    std::vector<std::pair<std::string, uint64_t>> sparse;
    int n = 0;
    for (auto& [key, entry] : entries) {
        if (n % INDEX_STRIDE == 0)
            sparse.push_back({key, static_cast<uint64_t>(ftell64(f))});
        wstr(f, key);
        uint8_t flags = entry.tombstone ? 1 : 0;
        fwrite(&flags, 1, 1, f);
        wstr(f, entry.value);
        ++n;
    }

    // Write Bloom filter section: [num_bits:4][raw bytes].
    uint64_t bloom_offset = static_cast<uint64_t>(ftell64(f));
    wu32(f, bloom.num_bits());
    fwrite(bloom.raw_bytes().data(), 1, bloom.raw_bytes().size(), f);

    // Write sparse index section.
    uint64_t index_offset = static_cast<uint64_t>(ftell64(f));
    wu32(f, static_cast<uint32_t>(sparse.size()));
    for (auto& [k, off] : sparse) {
        wstr(f, k);
        wu64(f, off);
    }

    // Write 16-byte footer: [bloom_offset:8][index_offset:8].
    wu64(f, bloom_offset);
    wu64(f, index_offset);

    fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
    fclose(f);
}

// ── SSTable open ─────────────────────────────────────────────────────────────

void SSTable::load_meta() {
    FILE* f = fopen(path_.string().c_str(), "rb");
    if (!f) throw std::runtime_error("SSTable open failed: " + path_.string());

    // Read 16-byte footer.
    fseek64(f, -16, SEEK_END);
    if (!ru64(f, bloom_offset_) || !ru64(f, index_offset_)) {
        fclose(f); return;
    }

    // Load Bloom filter.
    fseek64(f, static_cast<int64_t>(bloom_offset_), SEEK_SET);
    uint32_t num_bits;
    if (ru32(f, num_bits)) {
        size_t byte_len = (num_bits + 7) / 8;
        std::vector<uint8_t> buf(byte_len);
        if (fread(buf.data(), 1, byte_len, f) == byte_len)
            bloom_ = BloomFilter(buf.data(), byte_len, num_bits);
    }

    // Load sparse index.
    fseek64(f, static_cast<int64_t>(index_offset_), SEEK_SET);
    uint32_t count;
    if (!ru32(f, count)) { fclose(f); return; }
    index_.resize(count);
    for (auto& ip : index_) {
        if (!rstr(f, ip.key) || !ru64(f, ip.offset)) break;
    }
    fclose(f);
}

SSTable::SSTable(const std::filesystem::path& path) : path_(path), bloom_(0) {
    load_meta();
}

// ── SSTable::lookup ──────────────────────────────────────────────────────────

bool SSTable::bloom_may_contain(const std::string& key) const {
    return bloom_.may_contain(key);
}

LookupResult SSTable::lookup(const std::string& key) const {
    // Fast path: Bloom filter says "definitely absent" → skip all disk I/O.
    if (!bloom_.may_contain(key)) return LookupResult::not_found();

    if (index_.empty()) return LookupResult::not_found();

    // Binary-search the sparse index for the last point whose key ≤ target.
    auto it = std::upper_bound(
        index_.begin(), index_.end(), key,
        [](const std::string& k, const IndexPoint& ip) { return k < ip.key; });
    if (it == index_.begin()) return LookupResult::not_found();
    --it;

    FILE* f = fopen(path_.string().c_str(), "rb");
    if (!f) return LookupResult::not_found();

    fseek64(f, static_cast<int64_t>(it->offset), SEEK_SET);

    LookupResult result = LookupResult::not_found();
    // Scan forward until we find the key, overshoot, or reach the Bloom section.
    while (static_cast<uint64_t>(ftell64(f)) < bloom_offset_) {
        std::string k, v;
        uint8_t flags;
        if (!rstr(f, k) || fread(&flags, 1, 1, f) != 1 || !rstr(f, v)) break;
        if (k == key) {
            result = (flags & 1) ? LookupResult::tombstone()
                                 : LookupResult::found(std::move(v));
            break;
        }
        if (k > key) break;
    }
    fclose(f);
    return result;
}

// ── SSTable::scan_all ────────────────────────────────────────────────────────

std::vector<SSTEntry> SSTable::scan_all() const {
    std::vector<SSTEntry> result;
    FILE* f = fopen(path_.string().c_str(), "rb");
    if (!f) return result;

    // Data section ends at bloom_offset_.
    while (static_cast<uint64_t>(ftell64(f)) < bloom_offset_) {
        SSTEntry e;
        uint8_t flags;
        if (!rstr(f, e.key) || fread(&flags, 1, 1, f) != 1 || !rstr(f, e.value)) break;
        e.tombstone = (flags & 1) != 0;
        result.push_back(std::move(e));
    }
    fclose(f);
    return result;
}
