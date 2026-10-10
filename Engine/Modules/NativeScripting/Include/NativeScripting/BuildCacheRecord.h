#pragma once

// BuildCacheRecord — the on-disk record of the last successful native user-script
// build ("last_build.txt"), shared by every reader/writer of the format: the
// editor's live build cache (NativeScriptManager), the packaged-game staging steps
// (BuildPipeline / MacBundleAssembler), and the Player's prebuilt-module loader.
// Also home of the engine-ABI digest the record carries and the packaged
// `engine_abi` marker the Player validates it against (ship-safety item C2).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace NativeScripting
{

struct NativeBuildConfig; // NativeBuildConfig.h

// "<digest>\n<dllPath>\n<abiDigest>\n<engineBuildId>\n" in <dir>/last_build.txt.
// Trailing lines are absent in older records (2- and 3-line files) and read back
// empty; callers treat an empty field as "unknown", never as a match.
struct BuildCacheRecord
{
    std::string Digest;    // full staleness digest (ABI inputs + source stats)
    std::string DllPath;   // absolute (editor cache) or projectRoot-relative (packaged game)
    std::string AbiDigest; // ABI-inputs-only digest — the engine this DLL was built against

    // EngineBuildIdentity() of the engine that BUILT this DLL. The AbiDigest above
    // cannot serve this purpose at runtime: it is hashed from build inputs (import
    // lib path/size/mtime) that a Player has no access to, so a runtime cannot
    // recompute it to compare. This one the running engine reads off its own image.
    //
    // Set by the EDITOR's live build cache only, and read only for a dev cache
    // (a record with no engine_abi marker beside it). The packaging writers leave
    // it empty on purpose: a packaged game ships its own staged engine, which is
    // legitimately a different build from the editor that compiled the scripts, so
    // the building editor's identity is not the one a Player must match. Their gate
    // is the engine_abi marker instead. Empty always reads as "unknown", never as
    // a match — so leaving it empty can only cost a rebuild, never allow a bad load.
    std::string EngineBuildId;
};

inline constexpr const char* kBuildCacheFileName = "last_build.txt";

// Packaged games stage this marker beside the record, naming the engine identity the
// game actually ships. The Player refuses the recorded DLL when it differs from the
// record's AbiDigest — a stale user DLL against a patched engine is UB otherwise.
inline constexpr const char* kEngineAbiMarkerFileName = "engine_abi.txt";

// nullopt when <dir>/last_build.txt is absent or names no DLL. Digest comparisons and
// the DLL's on-disk existence are the caller's business.
std::optional<BuildCacheRecord> ReadBuildCacheRecord(const std::filesystem::path& dir);

// Writes <dir>/last_build.txt (creating <dir>); false on I/O failure.
bool WriteBuildCacheRecord(const std::filesystem::path& dir, const BuildCacheRecord& record);

// First line of <dir>/engine_abi.txt, or empty when absent (dev caches have none).
std::string ReadEngineAbiMarker(const std::filesystem::path& dir);
bool WriteEngineAbiMarker(const std::filesystem::path& dir, const std::string& abiDigest);

// FNV-1a over `data`, chainable via `seed`: pass a prior result to keep hashing one
// logical stream across several pieces. Shared by the digests below, the manager's
// content-addressed shadow-copy naming, and the full staleness digest.
std::uint64_t Fnv1aHash(std::string_view data, std::uint64_t seed = 0xcbf29ce484222325ULL);
std::string ToHexDigest(std::uint64_t hash);

// Hash of the ABI inputs alone: config + engine import-lib identity (size+mtime —
// any relink bumps it) + compile definitions. The manager's full staleness digest
// CONTINUES this hash over the source stats, which is what makes the ABI digest a
// mid-stream snapshot of the full digest.
std::uint64_t HashEngineAbiInputs(const NativeBuildConfig& config);

// ToHexDigest(HashEngineAbiInputs(...)) — the engine identity string recorded in
// build caches, packaged records, and the packaged engine_abi marker.
std::string ComputeEngineAbiDigest(const NativeBuildConfig& config);

// ComputeEngineAbiDigest with the EditorSDK interface cleared first — the digest
// of a RUNTIME user-script module. Runtime modules never link the EditorSDK: the
// editor clears EditorImportLib/EditorIncludeDirs off their build configs before
// stamping build records (EditorApplication::WireNativeScriptingForProject), so
// any recompute of a runtime module's digest — the packaged-build C2 staleness
// gate — must hash the same runtime-only inputs. Hashing a manifest-loaded config
// directly folds the editor's staged EditorSDK lib in and falsely fails the gate
// for every packaged native-script build.
std::string ComputeRuntimeEngineAbiDigest(NativeBuildConfig config);

} // namespace NativeScripting
} // namespace GameEngine
