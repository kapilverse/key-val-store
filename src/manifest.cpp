#include "manifest.h"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
  #include <io.h>
  static void fsync_file(const std::filesystem::path& p) {
      // Open for read+write so _commit has a writable handle.
      FILE* f = fopen(p.string().c_str(), "r+b");
      if (f) { _commit(_fileno(f)); fclose(f); }
  }
#else
  #include <fcntl.h>
  #include <unistd.h>
  static void fsync_file(const std::filesystem::path& p) {
      int fd = open(p.c_str(), O_RDONLY);
      if (fd >= 0) { fsync(fd); close(fd); }
  }
#endif

// ── File format ───────────────────────────────────────────────────────────────
// generation <N>
// file <sst_basename>
// file <sst_basename>
// ...
//
// Lines not matching a known prefix are silently ignored (forward compat).

bool Manifest::exists(const std::filesystem::path& dir) {
    return std::filesystem::exists(dir / "MANIFEST");
}

ManifestData Manifest::read(const std::filesystem::path& dir) {
    std::ifstream f(dir / "MANIFEST");
    if (!f) throw std::runtime_error("cannot open MANIFEST");

    ManifestData d;
    std::string  line;
    while (std::getline(f, line)) {
        if (line.rfind("generation ", 0) == 0) {
            d.generation = std::stoull(line.substr(11));
        } else if (line.rfind("file ", 0) == 0) {
            d.live_sstables.push_back(line.substr(5));
        }
    }
    return d;
}

void Manifest::write(const std::filesystem::path& dir, const ManifestData& data) {
    auto tmp  = dir / "MANIFEST.tmp";
    auto dest = dir / "MANIFEST";

    // Write to temp file — not yet visible as authoritative.
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write MANIFEST.tmp");
        f << "generation " << data.generation << "\n";
        for (auto& s : data.live_sstables)
            f << "file " << s << "\n";
        if (!f) throw std::runtime_error("MANIFEST.tmp write failed");
    }

    // fsync: data must be on disk before we commit via rename.
    fsync_file(tmp);

    // Atomic rename — this IS the commit point.
    // On POSIX this is guaranteed atomic.
    // On Windows it is not truly atomic but is effectively so for our purposes.
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        // Some Windows builds refuse to rename over an existing file; remove first.
        std::filesystem::remove(dest, ec);
        std::filesystem::rename(tmp, dest);
    }
}
