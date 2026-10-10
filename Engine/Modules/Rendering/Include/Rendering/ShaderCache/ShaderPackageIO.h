#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageContainer.h"

namespace GameEngine::Rendering
{
enum class ShaderSourceKind : uint8_t;

// A shader package as the engine uses it: the reflection metadata plus each
// stage's bytes. The file layout and its chunks are ShaderPackageContainer's;
// this is the engine-side reading and writing of those chunks.
struct ShaderPackage
{
    uint32_t version = kShaderPackageVersion;
    ShaderMeta meta{};
    // stage -> cooked bytes in the ShaderSourceKind the parse was asked for.
    // The reader picks between the package's forms once, here, so no consumer
    // has to know what a backend ingests.
    std::unordered_map<std::string, std::vector<uint8_t>> stageBytes;
    // Stage keys the package carries a WGSL chunk for, whatever kind was
    // requested. A stage with no WGSL chunk is served as SPIR-V, so the bytes
    // alone cannot tell a web-cooked package from an un-cooked one — the cook's
    // servability check asks this instead.
    std::unordered_set<std::string> wgslStages;
    std::optional<std::string> cacheInfoJson; // optional JSON
};

// Minimal cache-info payload (S3): include dependency list + content hashes.
struct ShaderCacheInfo
{
    struct Include
    {
        std::string path;    // resolved absolute path
        uint64_t hash64 = 0; // hash of file contents
    };

    std::vector<Include> includes;
};

// Build/parse cache-info JSON. Implemented in .cpp (uses nlohmann::json internally).
std::string BuildShaderCacheInfoJson(const ShaderCacheInfo& info);
bool ParseShaderCacheInfoJson(const std::string& jsonText, ShaderCacheInfo& out, std::string* outError = nullptr);

// Parse a `.shaderpkg` from in-memory bytes (no file I/O), serving each stage
// in `kind` — the form the consuming device ingests
// (IDevice::PreferredShaderSource). A stage the package carries only as SPIR-V
// stays SPIR-V under ShaderSourceKind::Wgsl: an un-cooked package then fails at
// module creation with the backend's own "SPIR-V ingestion is absent" error,
// which names the package instead of silently dropping its stages.
// Read on success; otherwise the failure kind, with the reason in `outError`.
ShaderPackageReadResult ParseShaderPkgFromBytes(const std::vector<uint8_t>& bytes,
                                                ShaderSourceKind kind,
                                                ShaderPackage& out,
                                                std::string* outError = nullptr);

// Resolve a package path from either an absolute/relative path or a bare name.
// If extension is missing, ".shaderpkg" is tried.
std::string ResolveShaderPkgPath(const std::string& pathOrName);

// Host-provided rebuild action for a package this build refuses because it was
// written in another format version. Given the name the loader was called with,
// it returns one sentence saying what to rebuild; LoadShaderPkg and
// LoadComputeStageBytes append it to their version-mismatch error. Only the host
// knows which asset source a name belongs to (the engine build's staged packages
// or a project's own), so EngineCore::Initialize installs it, beside
// Utils::SetShaderPathResolver and under the same rules: it is a file-static of
// this module read without synchronization, so install it before any package
// load starts and do not change it while loads can run (they also run on job
// threads). The function must stay valid for as long as packages are loaded and
// may be called from any loading thread. With none installed, as in a tool or a
// test that loads packages without the engine, the error is the version
// mismatch alone.
using ShaderPackageRebuildActionFunc = std::string (*)(const std::string& packageName);
void SetShaderPackageRebuildAction(ShaderPackageRebuildActionFunc action);
/// Current host callback, for preserving a scoped host's predecessor.
ShaderPackageRebuildActionFunc GetShaderPackageRebuildAction();

// Read a .shaderpkg from disk, serving each stage in `kind` (see
// ParseShaderPkgFromBytes). A package of another format version is refused with
// the host's rebuild action (SetShaderPackageRebuildAction) in `outError`.
bool LoadShaderPkg(const std::string& pathOrName,
                   ShaderSourceKind kind,
                   ShaderPackage& out,
                   std::string* outError = nullptr);

// Load a .shaderpkg and extract its compute ("cs") stage in `kind`. Uses the
// injected `loader` when provided, otherwise falls back to the module-local
// Utils::LoadShaderFile path, so a caller that was never handed a loader still
// resolves staged packages on its own. Never throws; returns empty and fills
// outError on failure, with the host's rebuild action for a package of another
// format version as in LoadShaderPkg.
std::vector<uint8_t> LoadComputeStageBytes(const char* pkgPath,
                                           ShaderSourceKind kind,
                                           std::vector<uint8_t> (*loader)(const char* name),
                                           std::string* outError = nullptr);

// Write a .shaderpkg to disk (atomically when possible).
bool SaveShaderPkg(const std::filesystem::path& outPath,
                   const ShaderMeta& meta,
                   const std::unordered_map<std::string, std::vector<uint8_t>>& stageSpvBytes,
                   const std::optional<std::string>& cacheInfoJson = std::nullopt,
                   std::string* outError = nullptr,
                   const std::unordered_map<std::string, std::vector<uint8_t>>& stageWgslBytes = {});
} // namespace GameEngine::Rendering

