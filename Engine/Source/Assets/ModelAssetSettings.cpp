#include "Assets/ModelAssetSettings.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"

namespace GameEngine {

const char* ModelAssetSettings::RigKindToMeta(ModelRigKind kind)
{
    switch (kind)
    {
    case ModelRigKind::Humanoid:    return "humanoid";
    case ModelRigKind::NotHumanoid: return "none";
    default:                        return "auto";
    }
}

bool ModelAssetSettings::ParseRigKindMeta(const std::string& value, ModelRigKind& out)
{
    if (value == "humanoid")
    {
        out = ModelRigKind::Humanoid;
        return true;
    }
    if (value == "none" || value == "notHumanoid" || value == "generic")
    {
        out = ModelRigKind::NotHumanoid;
        return true;
    }
    if (value == "auto" || value.empty())
    {
        out = ModelRigKind::Auto;
        return true;
    }
    return false;
}

ModelAssetSettings ModelAssetSettings::Load(const AssetRegistry& registry,
                                            const std::filesystem::path& assetPath)
{
    ModelAssetSettings settings;
    if (assetPath.empty())
        return settings;
    std::string value;
    if (registry.TryGetMetaValue(assetPath, kRigKindKey, value))
        ParseRigKindMeta(value, settings.RigKind);
    return settings;
}

bool ModelAssetSettings::Save(AssetRegistry& registry,
                              const std::filesystem::path& assetPath) const
{
    if (assetPath.empty())
        return false;
    return registry.SetMetaValue(assetPath, kRigKindKey, RigKindToMeta(RigKind));
}

ModelAssetSettings LoadModelAssetSettings(AssetManager* assetManager,
                                          const std::filesystem::path& assetPath)
{
    if (!assetManager || assetPath.empty())
        return {};
    return ModelAssetSettings::Load(assetManager->GetRegistry(), assetPath);
}

} // namespace GameEngine
