// Crash-recovery tests for the LSM-tree key-value store.
//
// Crash simulation: _exit() terminates the process immediately, bypassing all
// destructors — the MemTable is never flushed, the WAL is never sealed.
// This is the closest approximation of a power failure / OS crash for a user-
// space program without a kernel-mode fault injector.
//
// Usage:
//   test_crash                    run all tests (spawns subprocesses)
//   test_crash <phase> <dir>      run one crash-simulation phase (subprocess only)

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include "db.h"
#include "sstable.h"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Result tracking (used in both subprocess and driver modes)
// ─────────────────────────────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

static void pass(const std::string& msg) {
    std::cout << "  [PASS]  " << msg << "\n";
    ++g_passed;
}
static void fail(const std::string& msg) {
    std::cout << "  [FAIL]  " << msg << "\n";
    ++g_failed;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 1: WAL replay
//
// Phase wal_write: write 1000 keys, _exit() without flushing.
// Data lives only in the WAL. Recovery must replay it.
// ─────────────────────────────────────────────────────────────────────────────

static void phase_wal_write(const fs::path& dir) {
    DB::Options o;
    o.memtable_flush_bytes = 256u << 20; // never auto-flush
    o.compaction_threshold = 999;
    DB db(dir, o);
    for (int i = 0; i < 1000; ++i)
        db.put("wk" + std::to_string(i), "wv" + std::to_string(i));
    std::cout << "  [CRASH] WAL written, _exit before flush\n";
    _exit(0);
}

static int verify_wal_write(const fs::path& dir) {
    DB db(dir);
    int found = 0;
    for (int i = 0; i < 1000; ++i) {
        auto v = db.get("wk" + std::to_string(i));
        if (v && *v == "wv" + std::to_string(i)) ++found;
    }
    if (found == 1000) {
        pass("WAL replay: 1000/1000 keys recovered after simulated crash");
        return 0;
    }
    fail("WAL replay: only " + std::to_string(found) + "/1000 keys recovered");
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 2: SSTable + WAL recovery
//
// Phase mixed_write: flush keys 0–499 to an SSTable (durable), then write
// keys 500–999 only to the WAL, then _exit().
// Recovery must read the SSTable AND replay the WAL.
// ─────────────────────────────────────────────────────────────────────────────

static void phase_mixed_write(const fs::path& dir) {
    DB::Options o;
    o.memtable_flush_bytes = 256u << 20;
    DB db(dir, o);
    for (int i = 0; i < 500; ++i)
        db.put("mk" + std::to_string(i), "mv" + std::to_string(i));
    db.flush_memtable(); // keys 0–499 → SSTable + MANIFEST (durable)
    for (int i = 500; i < 1000; ++i)
        db.put("mk" + std::to_string(i), "mv" + std::to_string(i));
    std::cout << "  [CRASH] 500 in SSTable, 500 in WAL, _exit\n";
    _exit(0);
}

static int verify_mixed_write(const fs::path& dir) {
    DB db(dir);
    int from_sst = 0, from_wal = 0;
    for (int i = 0;   i < 500;  ++i) {
        auto v = db.get("mk" + std::to_string(i));
        if (v && *v == "mv" + std::to_string(i)) ++from_sst;
    }
    for (int i = 500; i < 1000; ++i) {
        auto v = db.get("mk" + std::to_string(i));
        if (v && *v == "mv" + std::to_string(i)) ++from_wal;
    }
    if (from_sst == 500 && from_wal == 500) {
        pass("SSTable+WAL: 500 from SSTable, 500 from WAL replay");
        return 0;
    }
    fail("SSTable+WAL: SSTable=" + std::to_string(from_sst) +
         "/500  WAL=" + std::to_string(from_wal) + "/500");
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 3: Orphan SSTable cleanup (in-process, no crash needed)
//
// Simulates a crashed compaction: a new SSTable is written to disk but the
// MANIFEST rename never committed — so the file is not in the live set.
// Recovery must delete the orphan and leave all live keys intact.
// ─────────────────────────────────────────────────────────────────────────────

static void test_orphan(const fs::path& dir) {
    fs::remove_all(dir);

    // Create a DB with 100 keys and flush them to an SSTable.
    {
        DB db(dir);
        for (int i = 0; i < 100; ++i)
            db.put("ok" + std::to_string(i), "ov" + std::to_string(i));
        db.flush_memtable();
    }

    // Plant an orphan SSTable not listed in MANIFEST.
    auto orphan = dir / "sst_999999.sst";
    {
        std::map<std::string, MemEntry> fake;
        fake["orphan_key"] = MemEntry{"orphan_val", false};
        SSTable::build(orphan, fake);
    }

    // Reopen — recovery should remove the orphan.
    { DB db(dir); (void)db; }

    if (fs::exists(orphan)) {
        fail("orphan cleanup: sst_999999.sst still on disk after recovery");
        return;
    }

    // Live data must still be accessible.
    DB db(dir);
    auto v = db.get("ok50");
    if (!v || *v != "ov50") {
        fail("orphan cleanup: live key lost after orphan removal");
        return;
    }

    // The orphan's data must NOT be accessible (it was never in MANIFEST).
    if (db.get("orphan_key").has_value()) {
        fail("orphan cleanup: orphan data is accessible — should not be");
        return;
    }

    pass("orphan cleanup: orphan SSTable removed, 100 live keys intact");
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 4: Partial / torn WAL record
//
// Phase write_one: write exactly one key, _exit().
// Driver appends a torn write: op byte + key_len but no key bytes.
// Recovery must: replay the complete record, silently drop the torn record.
// ─────────────────────────────────────────────────────────────────────────────

static void phase_write_one(const fs::path& dir) {
    DB::Options o;
    o.memtable_flush_bytes = 256u << 20;
    DB db(dir, o);
    db.put("sole_key", "sole_val");
    std::cout << "  [CRASH] one key in WAL, _exit before flush\n";
    _exit(0);
}

static int verify_partial_wal(const fs::path& dir) {
    DB db(dir);
    auto v = db.get("sole_key");
    if (!v || *v != "sole_val") {
        fail("partial WAL: valid key not recovered after torn write");
        return 1;
    }
    pass("partial WAL: torn record silently dropped, valid key intact");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase dispatcher (called in subprocess mode)
// ─────────────────────────────────────────────────────────────────────────────

static int dispatch_phase(const std::string& phase, const fs::path& dir) {
    fs::create_directories(dir);
    if (phase == "wal_write")    { phase_wal_write(dir);  return 0; } // _exit inside
    if (phase == "wal_verify")   return verify_wal_write(dir);
    if (phase == "mixed_write")  { phase_mixed_write(dir); return 0; }
    if (phase == "mixed_verify") return verify_mixed_write(dir);
    if (phase == "write_one")    { phase_write_one(dir);  return 0; }
    if (phase == "part_verify")  return verify_partial_wal(dir);
    std::cerr << "unknown phase: " << phase << "\n";
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Driver: runs all tests, spawning subprocesses for crash simulation
// ─────────────────────────────────────────────────────────────────────────────

static int spawn(const std::string& exe, const std::string& phase, const fs::path& dir) {
    auto d = dir.string();
#ifdef _WIN32
    // cmd.exe /c strips the outermost quotes when the argument starts with a quote.
    // Wrap the full command in a second outer pair so cmd.exe sees it as one unit.
    std::string cmd = "cmd /c \"\"" + exe + "\" " + phase + " \"" + d + "\"\"";
#else
    std::string cmd = "\"" + exe + "\" " + phase + " \"" + d + "\"";
#endif
    return std::system(cmd.c_str());
}

static int run_all(const std::string& exe) {
    const int LINE = 52;
    std::cout << "\n  kvstore crash-recovery tests\n";
    std::cout << "  " << std::string(LINE, '-') << "\n";

    fs::path tmp = fs::temp_directory_path() / "kv_crash_test";
    fs::remove_all(tmp);

    int subproc_pass = 0, subproc_fail = 0;
    auto run = [&](const std::string& phase, const fs::path& dir) {
        int rc = spawn(exe, phase, dir);
        if (rc == 0) ++subproc_pass; else ++subproc_fail;
    };

    // Test 1: WAL replay
    spawn(exe, "wal_write",  tmp / "t1"); // crash phase — exit code irrelevant
    run  ("wal_verify",      tmp / "t1");

    // Test 2: SSTable + WAL recovery
    spawn(exe, "mixed_write",  tmp / "t2");
    run  ("mixed_verify",      tmp / "t2");

    // Test 3: Orphan SSTable cleanup (no subprocess needed)
    test_orphan(tmp / "t3");

    // Test 4: Partial WAL record
    {
        auto dir = tmp / "t4";
        spawn(exe, "write_one", dir);

        // Append a torn write: op byte (Put=0) + key_len=30, but no key bytes.
        // The WAL replayer reads key_len=30, gets 0 bytes, breaks the loop.
        if (fs::exists(dir)) {
            for (auto& de : fs::directory_iterator(dir)) {
                if (de.path().extension() == ".log") {
                    FILE* f = fopen(de.path().string().c_str(), "ab");
                    if (f) {
                        uint8_t  op  = 0;
                        uint32_t kl  = 30;
                        fwrite(&op, 1, 1, f);
                        fwrite(&kl, 4, 1, f); // key bytes intentionally absent
                        fclose(f);
                    }
                    break;
                }
            }
            run("part_verify", dir);
        } else {
            fail("partial WAL: crash phase did not create test directory");
        }
    }

    // Tally: subprocess results + in-process results (g_passed/g_failed).
    int total_pass = subproc_pass + g_passed;
    int total_fail = subproc_fail + g_failed;
    int total      = total_pass + total_fail;

    std::cout << "  " << std::string(LINE, '-') << "\n";
    std::cout << "  " << total_pass << "/" << total << " tests passed";
    if (total_fail > 0) std::cout << "   (" << total_fail << " failed)";
    std::cout << "\n\n";

    fs::remove_all(tmp);
    return total_fail > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    if (argc == 3) return dispatch_phase(argv[1], argv[2]);
    if (argc == 1) return run_all(argv[0]);
    std::cerr << "usage: test_crash\n"
              << "       test_crash <phase> <dir>\n";
    return 1;
}
