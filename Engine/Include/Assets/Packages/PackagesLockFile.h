#pragma once

// P3 distribution — <project>/Packages/packages-lock.json, the committed pin
// set for git dependencies. The resolver writes it whenever a git dependency
// is (re)acquired and honors it on every later resolve: an unchanged manifest
// spec resolves to the locked commit without touching git or the network. A
// changed spec (different URL/ref/path text) re-resolves and relocks; an
// explicit `--update`-style relock of unchanged moving refs is future work.
// file:/embedded dependencies are never locked — they have no remote identity.

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace GameEngine
{

struct PackagesLockEntry
{
    std::string Spec;      // raw manifest spec text this entry was resolved from
    std::string Url;       // resolved clone URL
    std::string Ref;       // requested ref (tag / branch / sha)
    std::string Commit;    // full resolved commit sha — the pin
    std::string Version;   // package.json version at that commit
    std::string Integrity; // "fnv1a64-<16hex>" over the package.json bytes
};

struct PackagesLock
{
    // Dependency name -> pin. std::map keeps the file diff-stable.
    std::map<std::string, PackagesLockEntry> Packages;
};

inline constexpr const char* kPackagesLockFileName = "packages-lock.json";

// Load <lockFile>. Returns:
//  - true with an empty lock when the file does not exist (the quiet path),
//  - true with parsed contents on success,
//  - false with outError set when the file exists but is malformed.
bool TryLoadPackagesLock(const std::filesystem::path& lockFile,
                         PackagesLock& out,
                         std::string& outError);

// Write <lockFile> (creating parent directories). False on I/O failure.
bool SavePackagesLock(const std::filesystem::path& lockFile, const PackagesLock& lock);

// Drop `packageName`'s pin (load-mutate-save). A missing lock file or absent
// entry is success — the pin is gone either way. Package Manager actions use
// this to force re-acquisition: removing a git dependency drops its dead pin
// immediately (the resolver's lock hygiene only runs when some OTHER git
// dependency loads the lock), and a re-pin drops the old pin so the next
// resolve fetches the requested ref instead of honoring the stale commit.
bool RemovePackagesLockEntry(const std::filesystem::path& lockFile,
                             const std::string& packageName,
                             std::string& outError);

// Insert or replace `packageName`'s pin (load-mutate-save, creating the file
// when absent). The Add flow writes the pin it just acquired so the follow-up
// resolve takes the offline locked fast-path instead of fetching again.
bool UpsertPackagesLockEntry(const std::filesystem::path& lockFile,
                             const std::string& packageName,
                             const PackagesLockEntry& entry,
                             std::string& outError);

// Integrity hash recorded per entry: "fnv1a64-" + 16 hex chars over the raw
// package.json text of the acquired package.
std::string HashPackageManifestIntegrity(std::string_view manifestText);

} // namespace GameEngine
