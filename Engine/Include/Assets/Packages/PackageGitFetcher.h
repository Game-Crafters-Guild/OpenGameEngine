#pragma once

// P3 distribution — the process half of git resolution: shallow-fetch a pinned
// ref, read the package.json out of the checkout, and populate the global
// immutable cache entry (content-addressed by resolved commit). Requires git
// on PATH; every failure is returned as a precise error string for the
// resolver's loud per-package reporting.

#include "Assets/Packages/PackageGitSource.h"

#include <filesystem>
#include <string>

namespace GameEngine
{

struct GitPackageAcquisition
{
    std::filesystem::path PackageDir; // populated cache entry (contains package.json)
    std::string Name;                 // package.json name at that commit
    std::string Commit;               // full resolved commit sha
    std::string Version;              // package.json version at that commit
    std::string Integrity;            // HashPackageManifestIntegrity of package.json
};

// Locate git ("git --version" through the PATH). Result is cached for the
// process; a miss returns false with an actionable outError (git resolution is
// impossible without it).
bool TryFindGitExecutable(std::string& outExe, std::string& outError);

// Fetch spec.Ref from spec.Url (shallow, detached), read the package manifest
// at spec.Subdir, and populate <cacheRoot>/<name>@<version>-<sha12>/ with the
// package payload (no .git — entries are plain immutable trees). A non-empty
// `packageName` is verified against the manifest name (the resolver's
// dependency-key contract); the Package Manager's Add flow passes an empty
// name — it learns the name FROM the acquisition (out.Name) before writing the
// manifest entry. An entry that already exists for the resolved commit is
// reused as-is. Temp work happens under <cacheRoot>/.tmp and is cleaned up on
// every path.
bool AcquireGitPackage(const std::string& gitExe,
                       const std::string& packageName,
                       const GitPackageSpec& spec,
                       const std::filesystem::path& cacheRoot,
                       GitPackageAcquisition& out,
                       std::string& outError);

// Resolve the CURRENT commit sha of `ref` on the remote (git ls-remote; peeled
// tags resolve to the commit, matching what a fetch of the ref lands on). A
// 40-hex `ref` IS the pin — returned as-is without touching the network. The
// Package Manager's read-only "check for updates" compares this against the
// locked commit; nothing is written.
bool QueryGitRemoteRefCommit(const std::string& gitExe,
                             const std::string& url,
                             const std::string& ref,
                             std::string& outCommit,
                             std::string& outError);

} // namespace GameEngine
