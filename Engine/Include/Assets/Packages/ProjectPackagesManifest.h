#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace GameEngine
{

// Parsed <project>/Packages/manifest.json.
//
//   {
//     "dependencies": {
//       "ocean-pack": "file:../SharedPackages/OceanPack",   // local path
//       "debug-tools": "embedded"                           // <project>/Packages/debug-tools/
//     },
//     "disabled": ["heavy-megapack"]
//   }
//
// P0 supports the "file:<path>" and "embedded" specs only (git+... is the P3
// distribution slice). A missing manifest file means zero packages, zero noise.
struct ProjectPackagesManifest
{
    // Dependency name -> raw spec text ("file:<path>" or "embedded").
    std::map<std::string, std::string> Dependencies;
    std::set<std::string> Disabled;
};

// Load <manifestFile>. Returns:
//  - true with an empty manifest when the file does not exist (the quiet path),
//  - true with parsed contents on success,
//  - false with outError set when the file exists but is malformed.
bool TryLoadProjectPackagesManifest(const std::filesystem::path& manifestFile,
                                    ProjectPackagesManifest& out,
                                    std::string& outError);

// Add or remove `packageName` in the manifest's "disabled" array, preserving
// every other field (parse -> mutate -> rewrite; JSON comments do not survive
// the rewrite). The Package Manager panel's enable/disable toggle; mounts and
// code modules pick the change up on the next project open. Disabling creates
// a missing manifest and parent directory; enabling with no manifest is a
// successful no-op. Fails when an existing manifest is malformed or writing
// fails.
bool SetPackageDisabledInProjectManifest(const std::filesystem::path& manifestFile,
                                         const std::string& packageName,
                                         bool disabled,
                                         std::string& outError);

// Append "dependencies.<packageName>": <spec>, creating the manifest file
// (and parent directories) when it does not exist yet. Fails when the name is
// invalid, the dependency is already declared, or the file is malformed —
// spec-level validation (path exists / git URL shape) is the Add flow's job
// (PackageManagementActions.h), not this JSON edit's.
bool AddPackageToProjectManifest(const std::filesystem::path& manifestFile,
                                 const std::string& packageName,
                                 const std::string& spec,
                                 std::string& outError);

// Delete "dependencies.<packageName>" and any matching "disabled" entry.
// Fails when the dependency is not declared (the caller is confused about
// state — surface it) or the file is missing/malformed.
bool RemovePackageFromProjectManifest(const std::filesystem::path& manifestFile,
                                      const std::string& packageName,
                                      std::string& outError);

// Rewrite "dependencies.<packageName>" to `newSpec` (re-pin a git ref,
// relocate a file: path). Fails when the dependency is not declared.
bool SetPackageSpecInProjectManifest(const std::filesystem::path& manifestFile,
                                     const std::string& packageName,
                                     const std::string& newSpec,
                                     std::string& outError);

} // namespace GameEngine
