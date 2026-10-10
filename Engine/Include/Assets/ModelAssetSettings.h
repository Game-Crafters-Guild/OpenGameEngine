#pragma once

// Per-asset ModelAsset import settings (kv on the source path), engine-side so
// ModelAsset::PostLoad can resolve them on a worker without an editor
// dependency. Mirrors LodAssetSettings.

#include "Assets/RuntimeAssetMetadata.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>

namespace GameEngine {

class AssetRegistry;
class AssetManager;

// How HumanoidRig auto-import treats this model.
//   Auto         — name coverage plus four-legged rest-pose reject (default).
//   Humanoid     — run auto-import (same gates as Auto).
//   NotHumanoid  — never write a sidecar; delete leftover auto/empty files.
enum class ModelRigKind : uint8
{
    Auto = 0,
    Humanoid = 1,
    NotHumanoid = 2,
};

struct ModelAssetSettings
{
    ModelRigKind RigKind = ModelRigKind::Auto;

    static constexpr const char* kRigKindKey = kModelRigKindMetaKey;

    static ModelAssetSettings Load(const AssetRegistry& registry,
                                   const std::filesystem::path& assetPath);
    bool Save(AssetRegistry& registry, const std::filesystem::path& assetPath) const;

    static const char* RigKindToMeta(ModelRigKind kind);
    static bool ParseRigKindMeta(const std::string& value, ModelRigKind& out);
};

ModelAssetSettings LoadModelAssetSettings(AssetManager* assetManager,
                                          const std::filesystem::path& assetPath);

} // namespace GameEngine
