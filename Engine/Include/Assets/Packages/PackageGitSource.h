#pragma once

// P3 distribution — git dependency specs and the global immutable package
// cache. Pure parsing/path derivation; the process work (clone/fetch) lives in
// PackageGitFetcher.h so these stay unit-testable without git or network.

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine
{

// A parsed git dependency spec from Packages/manifest.json:
//
//   "git+https://host/owner/repo.git#<tag-or-branch-or-sha>"
//   "https://host/owner/repo.git#<ref>"                        (bare https also accepted)
//   "git+https://host/owner/repo.git#<ref>&path=packages/water" (monorepo subdir)
//
// The ref is REQUIRED — git dependencies are pinned by construction (the lock
// file then pins the resolved commit, so even a moving branch/tag ref is
// reproducible until the manifest spec text changes).
struct GitPackageSpec
{
    std::string Url;    // clone URL (git+ prefix and #fragment stripped)
    std::string Ref;    // tag, branch, or commit sha
    std::string Subdir; // package dir inside the repo ("" = repo root), '/'-separated
};

// True when `spec` should be handled by git resolution ("git+..." or a bare
// http(s) URL). Malformed git-ish specs still return true — they must fail
// loudly in TryParseGitPackageSpec, not fall through to the file: error.
bool IsGitPackageSpec(std::string_view spec);

// Parse `spec` for dependency `name` (used only in error text). On failure
// returns false with a precise outError.
bool TryParseGitPackageSpec(const std::string& name,
                            const std::string& spec,
                            GitPackageSpec& out,
                            std::string& outError);

// Global immutable package cache root: %LOCALAPPDATA%/GameEngine/PackageCache
// on Windows, ~/.cache/GameEngine/PackageCache elsewhere. Overridable via the
// GE_PACKAGE_CACHE_DIR environment variable (tests, CI). Entries are populated
// once and never mutated; there is no eviction in P3 (a `ge package cache
// prune` command is future work).
std::filesystem::path GlobalPackageCacheRoot();

// Cache entry directory name, content-addressed by the resolved commit:
// "<sanitized-name>@<version>-<sha12>" (sanitization = SanitizePackageAlias,
// so scoped names stay one path segment).
std::string PackageCacheEntryName(std::string_view packageName,
                                  std::string_view version,
                                  std::string_view commitSha);

// Managed .native root for module build outputs (module CacheDir). Consumers
// lease entries before use; inactive generations are bounded.
//
// MSVC limits generated object/PDB paths even when the input source path fits.
// Windows roots derived builds at <temp>/GameEngine/.native and hashes the
// original cacheRoot with the entry identity, keeping isolated caches separate.
// Other desktop platforms use <cacheRoot>/.native. Both retain the layout
// "<prefix8>-<fnv1a12(package identity)>/<fnv1a12(engine)>": a package retention
// scope containing isolated Engine-build generations. Browser uses .derived
// with a single package segment.
//
// `engineBuildId` (NativeScripting::EngineBuildIdentity) names the second
// segment. Distinct builds must not share a generation: the cache root is
// machine-global and its DLLs link against one specific Engine.dll.
std::filesystem::path PackageCacheDerivedNativeRoot(const std::filesystem::path& cacheRoot,
                                                    std::string_view entryName,
                                                    std::string_view engineBuildId);

} // namespace GameEngine
