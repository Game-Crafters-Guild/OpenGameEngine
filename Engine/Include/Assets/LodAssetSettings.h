#pragma once

// Per-asset LOD import settings: a GUID-keyed kv block (assets.lod.*) mirroring
// the FbxPerAssetImportSettings pattern, but engine-side so ModelAsset::PostLoad
// can resolve it on a background worker (no editor dependency).
//
// Two tiers:
//   - project-global defaults: the process-global LODImportSettings singleton,
//     seeded by the app (editor SettingsStore / Player game.config) at startup.
//   - per-asset overrides: the kv block below. UseGlobal=true falls through to
//     the global tier.

#include "Assets/MeshLODGenerator.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine {

class AssetRegistry;
class AssetManager;

// Per-asset kv representation. Defaults match a fresh asset with no kv written.
struct LodAssetSettings {
    bool   UseGlobal = true;
    bool   Generate = false;                        // opt-in (Phase A)
    uint32 LodCount = 4;
    float  TargetRatios[4] = {1.0f, 0.5f, 0.25f, 0.10f};
    float  TargetError[4]  = {0.0f, 0.06f, 0.16f, 0.35f};
    MeshLODBorderRule BorderRule = MeshLODBorderRule::SeamPlanes; // see MeshLODConfig
    bool   GenerateSkinned = false;                 // skinned skip-by-default

    static constexpr const char* kUseGlobalKey = kLodUseGlobalMetaKey;
    static constexpr const char* kGenerateKey = kLodGenerateMetaKey;
    static constexpr const char* kCountKey = kLodCountMetaKey;
    static constexpr const char* kRatiosKey = kLodRatiosMetaKey;
    static constexpr const char* kErrorsKey = kLodErrorsMetaKey;
    static constexpr const char* kBorderRuleKey = kLodBorderRuleMetaKey;
    static constexpr const char* kSkinnedKey = kLodSkinnedMetaKey;

    // Map the per-asset override fields to a generator config. AllowSloppy /
    // SloppyRatioThreshold / attribute weights stay at engine defaults (not
    // artist-authored).
    MeshLODConfig ToConfig() const;

    // Read the per-asset kv block (leaves fields at their defaults for absent
    // or malformed keys). Never throws.
    static LodAssetSettings Load(const AssetRegistry& registry,
                                 const std::filesystem::path& assetPath);

    // Persist the per-asset kv block. Returns false if the path is empty or a
    // write fails.
    bool Save(AssetRegistry& registry, const std::filesystem::path& assetPath) const;

    // C-locale CSV helpers (locale-independent serialization and parsing).
    // DecodeFloatCsv overwrites only the slots it successfully parses, leaving
    // the caller's defaults for missing/malformed tokens.
    static std::string EncodeFloatCsv(const float* values, uint32 count);
    static void        DecodeFloatCsv(std::string_view csv, float* out, uint32 maxCount);
};

// Config resolved for a single asset: per-asset kv layered over the global tier.
struct ResolvedLodSettings {
    bool          Generate = false;
    bool          GenerateSkinned = false;
    MeshLODConfig Config{};
};

// Layer a per-asset kv block over the process-global LODImportSettings.
ResolvedLodSettings ResolveLodSettings(const LodAssetSettings& perAsset,
                                       const LODImportSettings& global);

// Convenience for the PostLoad bridge: resolve directly from an AssetManager
// (worker-safe — pass AssetManager::GetThreadCurrent()). A null manager or empty
// path resolves the global tier alone.
ResolvedLodSettings ResolveLodSettings(AssetManager* assetManager,
                                       const std::filesystem::path& assetPath);

} // namespace GameEngine
