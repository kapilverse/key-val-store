# Getting Started Guide

This guide walks you through building and using the key-value store from scratch. No prior database experience needed.

---

## What is this?

A key-value store lets you save and retrieve data using a **key** (a name) and a **value** (the data you want to store). Think of it like a dictionary or a hashmap that persists to disk.

```
put username alice       →  stores "alice" under the key "username"
get username             →  retrieves "alice"
del username             →  deletes it
```

Under the hood this uses an **LSM-tree** — the same storage engine design used by LevelDB, RocksDB, and Apache Cassandra. Your data is written to disk safely so it survives crashes and restarts.

---

## Step 1 — Build

You need **CMake 3.15+** and a C++ compiler (MSVC on Windows, GCC or Clang on Linux/macOS).

```bash
# Clone or download the repo, then from the project root:
cmake -S . -B build
cmake --build build --config Release
```

This creates the main program at:

- Windows: `build\Release\kvstore.exe`
- Linux/macOS: `build/Release/kvstore`

---

## Step 2 — Run

```bash
# Windows
.\build\Release\kvstore.exe

# Linux / macOS
./build/Release/kvstore
```

You'll see:

```
kvstore  —  LSM-tree key-value store
data dir : ./kvdata

type 'help' for commands

>
```

Your data is stored in the `./kvdata` folder. You can point it to a different directory by passing a path as an argument:

```bash
./build/Release/kvstore my-data
```

---

## Step 3 — Basic Commands

### Store a value

```
> put city London
OK
```

The key is `city`, the value is `London`. Keys and values are plain text strings.

**Values with spaces** — everything after the key is the value:

```
> put greeting Hello, world
OK
> get greeting
Hello, world
```

### Read a value

```
> get city
London
```

If the key does not exist:

```
> get country
(not found)
```

### Delete a value

```
> del city
OK
> get city
(not found)
```

### See all stored keys

There is no built-in `list` command, but you can check how many entries are in memory with `stats`:

```
> stats
memtable : 3 entries, 312 bytes
sstables : 0 file(s)
manifest : generation 0
wal seg  : wal_000000.log
recovery : 0 WAL entries replayed
```

---

## Step 4 — Your data persists

Stop the program (`exit` or Ctrl+C), then start it again pointing at the same directory:

```
> put name Alice
OK
> exit

shutting down...
```

```bash
./build/Release/kvstore
```

```
> get name
Alice
```

Your data is on disk. It will survive restarts and even hard crashes (power loss, process kill). The engine writes to a **Write-Ahead Log** before updating anything in memory, so there is nothing to lose.

---

## Step 5 — What happens as you write more data

The engine keeps recent writes in a fast in-memory buffer. Once that buffer fills up (default: 64 KB), it automatically flushes to a sorted file on disk called an **SSTable**.

You can trigger this manually:

```
> flush
OK  (sstables now: 1)
```

After a flush, `stats` shows the data moved to disk:

```
> stats
memtable : 0 entries, 0 bytes
sstables : 1 file(s)
manifest : generation 1
wal seg  : wal_000001.log
recovery : 0 WAL entries replayed
```

Over time, multiple SSTable files accumulate. You can merge them all into one with:

```
> compact
OK  (sstables now: 1)
```

Compaction reclaims space from deleted keys and speeds up future reads. The engine also runs compaction automatically in the background when enough files pile up.

---

## Common Workflows

### Store structured data

Keys can be anything — use a naming convention to group related data:

```
> put user:1:name Alice
OK
> put user:1:email alice@example.com
OK
> put user:2:name Bob
OK

> get user:1:name
Alice
> get user:1:email
alice@example.com
```

### Overwrite a value

Just `put` to the same key again:

```
> put score 42
OK
> put score 99
OK
> get score
99
```

### Check engine state

```
> stats
memtable : 5 entries, 520 bytes
sstables : 2 file(s)
manifest : generation 3
wal seg  : wal_000003.log
recovery : 0 WAL entries replayed
```

| Field | What it means |
|---|---|
| `memtable` | Unsaved writes in RAM (fast to read, flushed to disk periodically) |
| `sstables` | Number of sorted files on disk |
| `manifest` | How many times the SSTable list has changed |
| `wal seg` | The current write-ahead log file |
| `recovery` | How many entries were replayed from the log on startup |

---

## All Commands

| Command | What it does |
|---|---|
| `put <key> <value>` | Store or overwrite a key |
| `get <key>` | Retrieve a value (prints `(not found)` if absent) |
| `del <key>` | Delete a key |
| `stats` | Show engine internals |
| `flush` | Force in-memory data to disk immediately |
| `compact` | Merge all on-disk files into one |
| `help` | Print command reference |
| `exit` or `quit` | Shut down cleanly |

---

## What lives on disk

After using the store for a bit, your data directory looks like this:

```
kvdata/
├── MANIFEST              list of active data files
├── wal_000002.log        recent writes not yet flushed (safe to lose on disk — WAL protects them)
└── sst_000001.sst        sorted data file from a previous flush
```

You do not need to manage these files manually. The engine handles creation, rotation, and cleanup automatically.

---

## Where to go next

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the engine works internally (file formats, durability guarantees, recovery sequence)
- [README.md](README.md) — build instructions, benchmark numbers, test suite
