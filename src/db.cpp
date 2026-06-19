#include "db.h"
#include "sstable.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

// ── helpers ──────────────────────────────────────────────────────────────────

std::filesystem::path DB::sst_path(uint64_t id) const {
    char buf[32];
    snprintf(buf, sizeof(buf), "sst_%06llu.sst",
             static_cast<unsigned long long>(id));
    return dir_ / buf;
}

std::filesystem::path DB::wal_seg_path(uint64_t id) const {
    char buf[32];
    snprintf(buf, sizeof(buf), "wal_%06llu.log",
             static_cast<unsigned long long>(id));
    return dir_ / buf;
}

// ── recovery ─────────────────────────────────────────────────────────────────
// On startup:
//   1. Delete any sealed (.log.done) WAL segments — their data is already in
//      an SSTable that was fully written before the seal was committed.
//   2. Load all SSTable files.
//   3. Replay every active (.log) WAL segment into the MemTable.
//      In the normal case there is exactly one; after a crash there may be
//      more, and replaying all of them is always correct (worst case: some
//      keys end up in both MemTable and SSTables, which is harmless — MemTable
//      is checked first and the duplicate gets cleaned up at next flush).
//   4. Open the most-recent active segment for future appends (or create one).

void DB::recover() {
    namespace fs = std::filesystem;

    // Step 1 — delete sealed segments (commit already happened before sealing).
    for (auto& de : fs::directory_iterator(dir_)) {
        auto& p = de.path();
        // Sealed files have extension ".done"; their stem ends in ".log".
        if (p.extension() == ".done" && p.stem().extension() == ".log") {
            std::error_code ec;
            fs::remove(p, ec);
        }
    }

    // Step 2 — load SSTables.
    for (auto& de : fs::directory_iterator(dir_)) {
        auto& p = de.path();
        if (p.extension() != ".sst") continue;
        std::string stem = p.stem().string();
        if (stem.size() < 4 || stem.substr(0, 4) != "sst_") continue;
        uint64_t id = std::stoull(stem.substr(4));
        next_sst_id_ = std::max(next_sst_id_, id + 1);
        sstables_.push_back(std::make_shared<SSTable>(p));
    }
    std::sort(sstables_.begin(), sstables_.end(),
        [](const auto& a, const auto& b) {
            return a->path().stem().string() > b->path().stem().string();
        });

    // Step 3 — collect and replay active WAL segments (oldest first).
    std::vector<std::pair<uint64_t, fs::path>> segs;
    for (auto& de : fs::directory_iterator(dir_)) {
        auto& p = de.path();
        if (p.extension() != ".log") continue;
        std::string stem = p.stem().string();
        if (stem.size() < 4 || stem.substr(0, 4) != "wal_") continue;
        uint64_t id = std::stoull(stem.substr(4));
        segs.push_back({id, p});
    }
    std::sort(segs.begin(), segs.end());

    for (auto& [id, path] : segs) {
        current_wal_id_ = std::max(current_wal_id_, id);
        WAL reader(path);
        reader.replay([&](WalRecord r) {
            if (r.op == WalOp::Put) memtable_.put(r.key, r.value);
            else                     memtable_.del(r.key);
            ++wal_replayed_;
        });
    }

    // Step 4 — open the active segment (reuse found one, or create fresh).
    // current_wal_id_ must be >= next_sst_id_ to avoid naming collisions.
    current_wal_id_ = std::max(current_wal_id_, next_sst_id_);
    wal_ = std::make_unique<WAL>(wal_seg_path(current_wal_id_));
}

// ── construction / destruction ────────────────────────────────────────────────

DB::DB(const std::filesystem::path& dir, Options opts)
    : dir_(dir), opts_(opts) {
    std::filesystem::create_directories(dir);
    recover();   // WAL is opened inside recover()
}

DB::~DB() {
    // Persist any unflushed MemTable writes before shutdown.
    if (!memtable_.empty())
        flush_memtable_locked();
}

// ── write path ───────────────────────────────────────────────────────────────
// WAL first, then MemTable.  WAL is synced so a crash after this point is safe.

void DB::put(const std::string& key, const std::string& value) {
    std::unique_lock lock(mu_);
    wal_->append(WalOp::Put, key, value);
    wal_->sync();
    memtable_.put(key, value);
    maybe_flush();
}

void DB::del(const std::string& key) {
    std::unique_lock lock(mu_);
    wal_->append(WalOp::Del, key);
    wal_->sync();
    memtable_.del(key);
    maybe_flush();
}

// ── read path ─────────────────────────────────────────────────────────────────
// MemTable first (most recent), then SSTables newest-to-oldest.
// The first layer that "knows" about the key wins — including tombstones.

std::optional<std::string> DB::get(const std::string& key) {
    std::shared_lock lock(mu_);

    if (const MemEntry* e = memtable_.raw(key)) {
        return e->tombstone ? std::nullopt : std::optional<std::string>{e->value};
    }

    for (auto& sst : sstables_) {
        LookupResult r = sst->lookup(key);
        switch (r.tag) {
        case LookupResult::Tag::NotFound:  continue;
        case LookupResult::Tag::Tombstone: return std::nullopt;
        case LookupResult::Tag::Found:     return r.value;
        }
    }

    return std::nullopt;
}

// ── flush ─────────────────────────────────────────────────────────────────────
// WAL segment rotation — the durability contract step by step:
//
//   1. Build SSTable and fsync it.          (data on disk)
//   2. Close the active WAL segment.
//   3. Rename wal_N.log → wal_N.log.done.  ← COMMIT POINT (atomic rename)
//      After this rename, recovery will delete the segment without replaying.
//      Before this rename, recovery would replay it (harmless duplicate).
//   4. Open the next WAL segment.
//   5. Clear the MemTable.
//   6. Delete the .done file (lazy cleanup — safe to defer or skip).

void DB::flush_memtable_locked() {
    if (memtable_.empty()) return;
    namespace fs = std::filesystem;

    // Step 1
    uint64_t sst_id = next_sst_id_++;
    auto sst = sst_path(sst_id);
    SSTable::build(sst, memtable_.data());   // fsyncs internally
    sstables_.insert(sstables_.begin(), std::make_shared<SSTable>(sst));

    // Steps 2 & 3 — seal the current WAL segment.
    auto old_seg  = wal_seg_path(current_wal_id_);
    wal_.reset();                           // fflush + fclose
    auto done_seg = fs::path(old_seg.string() + ".done");
    std::error_code ec;
    fs::rename(old_seg, done_seg, ec);      // commit point

    // Step 4 — next segment ID sits above both SST IDs and old WAL IDs.
    current_wal_id_ = next_sst_id_;
    wal_ = std::make_unique<WAL>(wal_seg_path(current_wal_id_));

    // Step 5
    memtable_.clear();

    // Step 6 — clean up the sealed segment (best-effort).
    fs::remove(done_seg, ec);
}

void DB::flush_memtable() {
    std::unique_lock lock(mu_);
    flush_memtable_locked();
}

void DB::maybe_flush() {
    if (memtable_.bytes() >= opts_.memtable_flush_bytes) {
        flush_memtable_locked();
        if (sstables_.size() >= opts_.compaction_threshold)
            compact_locked();
    }
}

// ── compaction ────────────────────────────────────────────────────────────────
// Merge all existing SSTables into one, keeping only the newest version of
// each key and discarding tombstones (since flushing the MemTable first means
// the SSTables now contain the full truth).

void DB::compact_locked() {
    if (sstables_.size() < 2) return;

    // Flush MemTable so its entries participate in the merge.
    flush_memtable_locked();

    // Merge: iterate SSTables oldest-first so newer entries overwrite older ones.
    std::map<std::string, SSTEntry> merged;
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        for (auto& e : (*it)->scan_all())
            merged[e.key] = e;
    }

    // Build merged SSTable — tombstones are dropped here because the MemTable
    // was flushed above; nothing older remains that they need to shadow.
    std::map<std::string, MemEntry> clean;
    for (auto& [k, e] : merged) {
        if (!e.tombstone)
            clean[k] = MemEntry{e.value, false};
    }

    auto new_id = next_sst_id_++;
    auto new_path = sst_path(new_id);
    SSTable::build(new_path, clean);

    // Swap in the new SSTable and delete the old files.
    auto old = std::move(sstables_);
    sstables_.clear();
    sstables_.push_back(std::make_shared<SSTable>(new_path));

    for (auto& sst : old)
        std::filesystem::remove(sst->path());
}

void DB::compact() {
    std::unique_lock lock(mu_);
    compact_locked();
}

// ── stats ─────────────────────────────────────────────────────────────────────

DBStats DB::stats() const {
    std::shared_lock lock(mu_);
    return {memtable_.size(), memtable_.bytes(),
            sstables_.size(), wal_replayed_, current_wal_id_};
}
