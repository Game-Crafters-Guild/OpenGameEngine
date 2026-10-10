#pragma once

// packages.index — the packaged game's replacement for Packages/manifest.json.
//
// A dev/editor layout resolves packages from the project's Packages/manifest.json
// (P0). A PACKAGED game ships no resolver inputs; instead the build pipeline
// writes <game>/Packages/packages.index listing every staged package in mount
// order, and the Player mounts each entry directly via MakePackageMount
// (<game>/Packages/<alias>/Assets + its .assetmanifest) — no resolution, no
// semver, no filesystem scan. Chosen over shipping a manifest.json-equivalent
// because the resolver's inputs (package.json files, semver ranges) are already
// burned into the staged layout at build time; re-resolving at every game boot
// would re-do work whose only possible new outcome is a boot failure.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

struct PackagesIndexEntry
{
    std::string Name;    // manifest package name (GUID-namespace identity)
    std::string Alias;   // mount alias == staged subdir Packages/<Alias>/
    std::string Version; // informational (diagnostics)
    int32_t Priority = 0;
    // Native runtime module names staged under Packages/<Alias>/NativeScripts/.
    // The Player loads each via LoadPrebuiltUserModule(<game>/Packages/<Alias>, name).
    std::vector<std::string> NativeModules;
};

// Entries in mount order (dependencies first — the resolver's topo order).
struct PackagesIndex
{
    std::vector<PackagesIndexEntry> Packages;
};

inline constexpr const char* kPackagesIndexFileName = "packages.index";
inline constexpr const char* kPackagesStagingDirName = "Packages";

// Load <indexFile>. Returns:
//  - true with an empty index when the file does not exist (games without
//    packages ship no index — the quiet path),
//  - true with parsed contents on success,
//  - false with outError set when the file exists but is malformed.
bool TryLoadPackagesIndex(const std::filesystem::path& indexFile,
                          PackagesIndex& out,
                          std::string& outError);

// Write <indexFile> (creating parent directories). False on I/O failure.
bool SavePackagesIndex(const std::filesystem::path& indexFile, const PackagesIndex& index);

} // namespace GameEngine
