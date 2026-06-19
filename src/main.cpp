#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include "db.h"

static void print_help() {
    std::cout <<
        "  put <key> <value>   write a key\n"
        "  get <key>           read a key\n"
        "  del <key>           delete a key\n"
        "  stats               show engine internals\n"
        "  flush               force MemTable → SSTable\n"
        "  compact             merge all SSTables into one\n"
        "  help                this message\n"
        "  exit / quit         close\n";
}

int main(int argc, char* argv[]) {
    std::string dir = (argc > 1) ? argv[1] : "./kvdata";

    std::cout << "kvstore  —  LSM-tree key-value store\n";
    std::cout << "data dir : " << dir << "\n\n";

    DB::Options opts;
    opts.memtable_flush_bytes = 4 * 1024;   // 4 KB — low so you can see flushes happen
    opts.compaction_threshold = 4;

    DB db(dir, opts);

    auto s = db.stats();
    if (s.wal_replayed > 0)
        std::cout << "[recovery] replayed " << s.wal_replayed << " WAL entries\n";
    std::cout << "type 'help' for commands\n\n";

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "put") {
            std::string key, value;
            ss >> key;
            std::getline(ss >> std::ws, value);
            if (key.empty() || value.empty()) {
                std::cout << "usage: put <key> <value>\n";
                continue;
            }
            db.put(key, value);
            std::cout << "OK\n";

        } else if (cmd == "get") {
            std::string key;
            ss >> key;
            if (key.empty()) { std::cout << "usage: get <key>\n"; continue; }
            auto v = db.get(key);
            std::cout << (v ? *v : "(not found)") << "\n";

        } else if (cmd == "del") {
            std::string key;
            ss >> key;
            if (key.empty()) { std::cout << "usage: del <key>\n"; continue; }
            db.del(key);
            std::cout << "OK\n";

        } else if (cmd == "stats") {
            auto st = db.stats();
            std::cout
                << "memtable : " << st.memtable_entries << " entries, "
                                 << st.memtable_bytes   << " bytes\n"
                << "sstables : " << st.sstable_count    << " file(s)\n"
                << "wal seg  : wal_" << std::setfill('0') << std::setw(6)
                                 << st.active_wal_id    << ".log\n"
                << "recovery : " << st.wal_replayed     << " WAL entries replayed\n";

        } else if (cmd == "flush") {
            db.flush_memtable();
            auto st = db.stats();
            std::cout << "OK  (sstables now: " << st.sstable_count << ")\n";

        } else if (cmd == "compact") {
            db.compact();
            auto st = db.stats();
            std::cout << "OK  (sstables now: " << st.sstable_count << ")\n";

        } else if (cmd == "help") {
            print_help();

        } else if (cmd == "exit" || cmd == "quit") {
            break;

        } else if (!cmd.empty()) {
            std::cout << "unknown: '" << cmd << "'  (type help)\n";
        }
    }

    std::cout << "\nshutting down...\n";
    return 0;
}
