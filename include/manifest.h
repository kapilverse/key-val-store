#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct ManifestData {
    uint64_t                 generation   = 0;
    std::vector<std::string> live_sstables;  // basenames, newest-first
};

// Atomic manifest writer.
//
// Write protocol: MANIFEST.tmp (fsync) → rename → MANIFEST
// The rename is the commit point.  Recovery deletes any leftover
// MANIFEST.tmp before reading MANIFEST, so a crash mid-write leaves
// the previous manifest intact.
namespace Manifest {
    bool         exists(const std::filesystem::path& dir);
    ManifestData read  (const std::filesystem::path& dir);
    void         write (const std::filesystem::path& dir, const ManifestData& data);
}
