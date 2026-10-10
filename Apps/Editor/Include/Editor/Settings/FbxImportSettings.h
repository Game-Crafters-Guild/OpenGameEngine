#pragma once

#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Logger/Logger.h"

#include <filesystem>
#include <string>

namespace GameEngine::Editor
{
struct FbxImportSettings
{
    bool ImportSceneExtras = true;
    bool ImportCameras = true;
    bool ImportLights = true;
    bool ImportHelperNodes = true;
    bool GenerateMissingTangents = true;
    bool CleanSkinWeights = true;
    bool AdjustPivots = true;
    bool PreserveGeometryTransforms = true;
    bool ImportEmbeddedTextures = true;

    static constexpr const char* kImportSceneExtrasKey = "assets.fbx.importSceneExtras";
    static constexpr const char* kImportCamerasKey = "assets.fbx.importCameras";
    static constexpr const char* kImportLightsKey = "assets.fbx.importLights";
    static constexpr const char* kImportHelperNodesKey = "assets.fbx.importHelperNodes";
    static constexpr const char* kGenerateMissingTangentsKey = "assets.fbx.generateMissingTangents";
    static constexpr const char* kCleanSkinWeightsKey = "assets.fbx.cleanSkinWeights";
    static constexpr const char* kAdjustPivotsKey = "assets.fbx.adjustPivots";
    static constexpr const char* kPreserveGeometryTransformsKey = "assets.fbx.preserveGeometryTransforms";
    static constexpr const char* kImportEmbeddedTexturesKey = "assets.fbx.importEmbeddedTextures";

    static FbxImportSettings Load(const std::filesystem::path& workspaceRoot)
    {
        FbxImportSettings settings{};
        auto store = OpenProjectSettings(workspaceRoot);
        std::string err;
        (void)store.Load(&err);
        store.TryGetBool(kImportSceneExtrasKey, settings.ImportSceneExtras);
        store.TryGetBool(kImportCamerasKey, settings.ImportCameras);
        store.TryGetBool(kImportLightsKey, settings.ImportLights);
        store.TryGetBool(kImportHelperNodesKey, settings.ImportHelperNodes);
        store.TryGetBool(kGenerateMissingTangentsKey, settings.GenerateMissingTangents);
        store.TryGetBool(kCleanSkinWeightsKey, settings.CleanSkinWeights);
        store.TryGetBool(kAdjustPivotsKey, settings.AdjustPivots);
        store.TryGetBool(kPreserveGeometryTransformsKey, settings.PreserveGeometryTransforms);
        store.TryGetBool(kImportEmbeddedTexturesKey, settings.ImportEmbeddedTextures);
        return settings;
    }

    bool Save(const std::filesystem::path& workspaceRoot) const
    {
        auto store = OpenProjectSettings(workspaceRoot);
        std::string err;
        (void)store.Load(&err);
        store.SetBool(kImportSceneExtrasKey, ImportSceneExtras);
        store.SetBool(kImportCamerasKey, ImportCameras);
        store.SetBool(kImportLightsKey, ImportLights);
        store.SetBool(kImportHelperNodesKey, ImportHelperNodes);
        store.SetBool(kGenerateMissingTangentsKey, GenerateMissingTangents);
        store.SetBool(kCleanSkinWeightsKey, CleanSkinWeights);
        store.SetBool(kAdjustPivotsKey, AdjustPivots);
        store.SetBool(kPreserveGeometryTransformsKey, PreserveGeometryTransforms);
        store.SetBool(kImportEmbeddedTexturesKey, ImportEmbeddedTextures);
        if (!store.Save(&err))
        {
            Logger::Log::Error("FbxImportSettings: failed to save project settings: {}", err);
            return false;
        }
        return true;
    }

    Engine::Renderer::ModelEntityFactoryOptions ToModelEntityFactoryOptions() const
    {
        Engine::Renderer::ModelEntityFactoryOptions options{};
        options.SpawnImportedCameras = ImportSceneExtras && ImportCameras;
        options.SpawnImportedLights = ImportSceneExtras && ImportLights;
        options.SpawnImportedHelperNodes = ImportSceneExtras && ImportHelperNodes;
        return options;
    }
};

inline Engine::Renderer::ModelEntityFactoryOptions GetFbxModelEntityFactoryOptions()
{
    return FbxImportSettings::Load(EngineCore::GetInstance().GetWorkspaceRoot()).ToModelEntityFactoryOptions();
}

struct FbxPerAssetImportSettings
{
    bool UseGlobalSettings = true;
    FbxImportSettings Settings{};

    static constexpr const char* kUseGlobalSettingsKey = "assets.fbx.useGlobalSettings";

    static FbxPerAssetImportSettings Load(const std::filesystem::path& assetPath)
    {
        FbxPerAssetImportSettings settings{};
        if (assetPath.empty())
            return settings;

        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        std::string value;
        if (registry.TryGetMetaValue(assetPath, kUseGlobalSettingsKey, value))
            settings.UseGlobalSettings = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kImportSceneExtrasKey, value))
            settings.Settings.ImportSceneExtras = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kImportCamerasKey, value))
            settings.Settings.ImportCameras = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kImportLightsKey, value))
            settings.Settings.ImportLights = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kImportHelperNodesKey, value))
            settings.Settings.ImportHelperNodes = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kGenerateMissingTangentsKey, value))
            settings.Settings.GenerateMissingTangents = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kCleanSkinWeightsKey, value))
            settings.Settings.CleanSkinWeights = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kAdjustPivotsKey, value))
            settings.Settings.AdjustPivots = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kPreserveGeometryTransformsKey, value))
            settings.Settings.PreserveGeometryTransforms = !(value == "0" || value == "false" || value == "False");
        if (registry.TryGetMetaValue(assetPath, FbxImportSettings::kImportEmbeddedTexturesKey, value))
            settings.Settings.ImportEmbeddedTextures = !(value == "0" || value == "false" || value == "False");
        return settings;
    }

    bool Save(const std::filesystem::path& assetPath) const
    {
        if (assetPath.empty())
            return false;

        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        bool ok = true;
        ok &= registry.SetMetaValue(assetPath, kUseGlobalSettingsKey, UseGlobalSettings ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kImportSceneExtrasKey, Settings.ImportSceneExtras ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kImportCamerasKey, Settings.ImportCameras ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kImportLightsKey, Settings.ImportLights ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kImportHelperNodesKey, Settings.ImportHelperNodes ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kGenerateMissingTangentsKey, Settings.GenerateMissingTangents ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kCleanSkinWeightsKey, Settings.CleanSkinWeights ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kAdjustPivotsKey, Settings.AdjustPivots ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kPreserveGeometryTransformsKey, Settings.PreserveGeometryTransforms ? "1" : "0");
        ok &= registry.SetMetaValue(assetPath, FbxImportSettings::kImportEmbeddedTexturesKey, Settings.ImportEmbeddedTextures ? "1" : "0");
        return ok;
    }

    Engine::Renderer::ModelEntityFactoryOptions Resolve(
        const std::filesystem::path& workspaceRoot) const
    {
        return UseGlobalSettings
            ? FbxImportSettings::Load(workspaceRoot).ToModelEntityFactoryOptions()
            : Settings.ToModelEntityFactoryOptions();
    }
};

inline Engine::Renderer::ModelEntityFactoryOptions GetFbxModelEntityFactoryOptions(
    const std::filesystem::path& assetPath)
{
    if (assetPath.empty())
        return GetFbxModelEntityFactoryOptions();
    return FbxPerAssetImportSettings::Load(assetPath).Resolve(
        EngineCore::GetInstance().GetWorkspaceRoot());
}
} // namespace GameEngine::Editor
