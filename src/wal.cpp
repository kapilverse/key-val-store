#include "wal.h"
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
  #include <io.h>
  static void platform_fsync(FILE* f) { _commit(_fileno(f)); }
#else
  #include <unistd.h>
  static void platform_fsync(FILE* f) { ::fsync(fileno(f)); }
#endif

// ── binary helpers ──────────────────────────────────────────────────────────

static void wu32(FILE* f, uint32_t v) { fwrite(&v, sizeof(v), 1, f); }

static void wstr(FILE* f, std::string_view s) {
    wu32(f, static_cast<uint32_t>(s.size()));
    fwrite(s.data(), 1, s.size(), f);
}

static bool ru32(FILE* f, uint32_t& v) {
    return fread(&v, sizeof(v), 1, f) == 1;
}

static bool rstr(FILE* f, std::string& s) {
    uint32_t len;
    if (!ru32(f, len)) return false;
    s.resize(len);
    return fread(s.data(), 1, len, f) == len;
}

// ── WAL record format ────────────────────────────────────────────────────────
// [op:1][key_len:4][key][val_len:4][val]
// op 0 = Put, 1 = Del.  val_len/val are always written (0/"" for Del).

WAL::WAL(const std::filesystem::path& path) : path_(path) {
    file_ = fopen(path.string().c_str(), "ab+");
    if (!file_)
        throw std::runtime_error("cannot open WAL: " + path.string());
}

WAL::~WAL() {
    if (file_) { fflush(file_); fclose(file_); }
}

void WAL::append(WalOp op, std::string_view key, std::string_view value) {
    uint8_t b = static_cast<uint8_t>(op);
    fwrite(&b, 1, 1, file_);
    wstr(file_, key);
    wstr(file_, value);
    // No fsync here; the caller decides when durability is needed.
}

void WAL::sync() {
    fflush(file_);
    platform_fsync(file_);
}

void WAL::replay(const std::function<void(WalRecord)>& cb) const {
    FILE* f = fopen(path_.string().c_str(), "rb");
    if (!f) return;   // empty or non-existent — nothing to replay

    uint8_t op;
    while (fread(&op, 1, 1, f) == 1) {
        WalRecord r;
        r.op = static_cast<WalOp>(op);
        if (!rstr(f, r.key) || !rstr(f, r.value)) break;  // truncated record
        cb(std::move(r));
    }
    fclose(f);
}

