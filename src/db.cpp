#include "db.h"
#include "sstable.h"
#include <algorithm>
#include <cstdio>
#include <set>
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
//   1. Delete MANIFEST.tmp (leftover from a crashed atomic write — the old
//      MANIFEST was not replaced, so it remains authoritative).
//   2. Delete sealed WAL segments (.log.done).
//   3. Load SSTable list from MANIFEST (deterministic).
//      Fallback: directory scan if no MANIFEST exists (first-ever start, or
//      upgrade from a build that predates MANIFEST).
//   4. Delete orphaned .sst files not listed in MANIFEST — they come from
//      interrupted compactions where the new SSTable was written but the
//      MANIFEST rename had not yet committed.
//   5. Replay active WAL segments into the MemTable.
//   6. Open (or create) the active WAL segment.

void DB::recover() {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Step 1
    fs::remove(dir_ / "MANIFEST.tmp", ec);

    // Step 2
    for (auto& de : fs::directory_iterator(dir_)) {
        auto& p = de.path();
        if (p.extension() == ".done" && p.stem().extension() == ".log")
            fs::remove(p, ec);
    }

    // Step 3 — load SSTables
    if (Manifest::exists(dir_)) {
        auto data = Manifest::read(dir_);
        manifest_gen_ = data.generation;
        for (auto& name : data.live_sstables) {
            auto p = dir_ / name;
            // Extract numeric ID from "sst_000003.sst"
            auto stem = fs::path(name).stem().string();   // "sst_000003"
            uint64_t id = std::stoull(stem.substr(4));
            next_sst_id_ = std::max(next_sst_id_, id + 1);
            sstables_.push_back(std::make_shared<SSTable>(p));
        }
        // sstables_ stays in MANIFEST order (newest-first, as written).
    } else {
        // First startup — scan directory (no MANIFEST yet).
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
        // Write initial MANIFEST so future restarts are deterministic.
        update_manifest_locked();
    }

    // Step 4 — delete orphaned SSTable files not listed in the MANIFEST.
    // These are new SSTables from interrupted compactions (built, but MANIFEST
    // rename hadn't committed yet).
    {
        std::set<std::string> live;
        for (auto& sst : sstables_)
            live.insert(sst->path().filename().string());
        for (auto& de : fs::directory_iterator(dir_)) {
            if (de.path().extension() != ".sst") continue;
            if (live.find(de.path().filename().string()) == live.end())
                fs::remove(de.path(), ec);
        }
    }

    // Step 5 — replay active WAL segments (oldest-first).
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

    // Step 6 — open active WAL segment (ID must be above all SST IDs).
    current_wal_id_ = std::max(current_wal_id_, next_sst_id_);
    wal_ = std::make_unique<WAL>(wal_seg_path(current_wal_id_));
}

// ── update_manifest_locked ────────────────────────────────────────────────────
// Writes the current sstables_ list to MANIFEST atomically.
// Caller must hold a unique lock.

void DB::update_manifest_locked() {
    ManifestData data;
    data.generation = ++manifest_gen_;
    for (auto& sst : sstables_)
        data.live_sstables.push_back(sst->path().filename().string());
    Manifest::write(dir_, data);
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

    // 1. Build SSTable and fsync it — data is now durable on disk.
    uint64_t sst_id = next_sst_id_++;
    auto sst = sst_path(sst_id);
    SSTable::build(sst, memtable_.data());
    sstables_.insert(sstables_.begin(), std::make_shared<SSTable>(sst));

    // 2. Write MANIFEST — this makes the new SSTable officially live.
    //    This must happen BEFORE sealing the WAL so that, on a crash between
    //    here and the WAL seal, recovery finds the SSTable in the MANIFEST and
    //    replays the (still-active) WAL without data loss.
    update_manifest_locked();

    // 3. Seal the WAL segment (rename → .done).  The WAL data is now in
    //    both the SSTable and the MANIFEST; recovery no longer needs it.
    auto old_seg  = wal_seg_path(current_wal_id_);
    wal_.reset();                                       // fflush + fclose
    auto done_seg = fs::path(old_seg.string() + ".done");
    std::error_code ec;
    fs::rename(old_seg, done_seg, ec);

    // 4. Open the next WAL segment.
    current_wal_id_ = next_sst_id_;
    wal_ = std::make_unique<WAL>(wal_seg_path(current_wal_id_));

    // 5. Clear the MemTable — writes now go into the fresh WAL segment.
    memtable_.clear();

    // 6. Delete the sealed WAL (lazy cleanup — safe to defer).
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

    auto new_id   = next_sst_id_++;
    auto new_path = sst_path(new_id);
    SSTable::build(new_path, clean);   // fsyncs internally

    // Swap sstables_ to point only at the merged file.
    auto old = std::move(sstables_);
    sstables_.clear();
    sstables_.push_back(std::make_shared<SSTable>(new_path));

    // MANIFEST update is the commit point for compaction.
    // After this rename: new SSTable is authoritative.
    // Old files on disk are now orphans — safe to delete below.
    // If we crash before this: old SSTables are still in MANIFEST and will
    // be loaded on recovery; the new file is an orphan (deleted on startup).
    update_manifest_locked();

    // Delete old SSTable files — they're no longer in the MANIFEST.
    for (auto& sst : old) {
        std::error_code ec;
        std::filesystem::remove(sst->path(), ec);
    }
}

void DB::compact() {
    std::unique_lock lock(mu_);
    compact_locked();
}

// ── stats ─────────────────────────────────────────────────────────────────────

DBStats DB::stats() const {
    std::shared_lock lock(mu_);
    return {memtable_.size(), memtable_.bytes(),
            sstables_.size(), wal_replayed_, current_wal_id_, manifest_gen_};
}
