// Assets a page loads by URL: ge_load_asset, ge_asset_status, ge_instantiate_model and
// ge_model_emissive_strength (Apps/WebLibrary/ts/src/abi.ts).

#include "AbiAssets.h"

#include "AbiEntities.h"
#include "AbiErrors.h"
#include "AbiLifecycle.h"
#include "AbiQuery.h"
#include "UrlAssetLoads.h"
#include "WebLibraryApplication.h"

#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Engine/Rendering/RenderServices.h"
#include "Types/StringId.h"

#include <emscripten/emscripten.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <variant>

namespace GameEngine::WebLibrary
{

namespace
{

UrlAssetLoads g_Loads;

} // namespace

void AdvanceAssetLoads()
{
    g_Loads.Advance();
}

void DropAssetLoads()
{
    g_Loads.Clear();
}

} // namespace GameEngine::WebLibrary

using namespace GameEngine;
using namespace GameEngine::WebLibrary;

extern "C"
{

/// Starts fetching and loading the asset at `url` and returns its handle at once; kInvalidAsset
/// when the load cannot start. Never suspends: the load moves only inside ge_tick and
/// ge_update_assets.
EMSCRIPTEN_KEEPALIVE uint32_t ge_load_asset(const char* url)
{
    const AbiCallScope scope("ge_load_asset");
    if (scope.Refused() || RefuseAfterShutdown("ge_load_asset"))
        return kInvalidAsset;
    if (!EngineCore::GetInstance().IsInitialized())
    {
        SetLastError("ge_load_asset was called before ge_create succeeded.");
        return kInvalidAsset;
    }
    if (!url || url[0] == '\0')
    {
        SetLastError("ge_load_asset needs the asset's URL; it was empty.");
        return kInvalidAsset;
    }
    std::string error;
    const uint32_t handle = g_Loads.Start(url, error);
    if (handle == kInvalidAsset)
        SetLastError(std::move(error));
    return handle;
}

/// One of abi.ts's AssetStatus; kFailed for a handle no load has. Changes nothing.
EMSCRIPTEN_KEEPALIVE int32_t ge_asset_status(uint32_t handle)
{
    const AbiCallScope scope("ge_asset_status");
    if (scope.Refused() || RefuseAfterShutdown("ge_asset_status"))
        return kFailed;
    UrlAssetStatus status = UrlAssetStatus::Loading;
    std::string failure;
    if (!g_Loads.TryGetStatus(handle, status, failure))
    {
        SetLastError("ge_asset_status: no load has handle {}.", handle);
        return kFailed;
    }
    if (status == UrlAssetStatus::Failed)
        SetLastError("ge_asset_status: {}", failure);
    return static_cast<int32_t>(status);
}

/// Instantiates a Ready model under `parent` (kInvalidEntity for a root); returns its root
/// entity, which carries a LocalBounds enclosing the model, or kInvalidEntity.
EMSCRIPTEN_KEEPALIVE uint32_t ge_instantiate_model(uint32_t handle, uint32_t parentId)
{
    const AbiCallScope scope("ge_instantiate_model");
    if (scope.Refused())
        return ECS::kInvalidEntity;
    if (RefuseWhileQueryRuns("ge_instantiate_model"))
        return ECS::kInvalidEntity;
    constexpr const char* kCall = "ge_instantiate_model";
    ECS::World* world = WorldForCall(kCall);
    if (!world)
        return ECS::kInvalidEntity;
    WebLibraryApplication* application = GetRunningApplication();
    Engine::Renderer::RenderServices* renderServices = application ? application->GetRenderServices() : nullptr;
    if (!renderServices)
    {
        SetLastError("{} needs the renderer that ge_create brings up.", kCall);
        return ECS::kInvalidEntity;
    }
    ECS::EntityHandle parent;
    if (parentId != ECS::kInvalidEntity && !ResolveEntity(*world, parentId, kCall, parent))
        return ECS::kInvalidEntity;

    GUID guid;
    const SharedPtr<Asset> asset = g_Loads.GetAsset(handle, guid);
    if (!asset)
    {
        SetLastError("{}: load {} is not Ready; poll ge_asset_status until it is.", kCall, handle);
        return ECS::kInvalidEntity;
    }
    if (asset->GetType() != AssetType::Model)
    {
        SetLastError("{}: load {} is not a model; only glTF and GLB files instantiate.", kCall, handle);
        return ECS::kInvalidEntity;
    }
    const auto& model = static_cast<const ModelAsset&>(*asset);
    const std::string rootName = std::filesystem::path(asset->GetPath()).stem().string();
    const Engine::Renderer::ModelEntityResult created =
        Engine::Renderer::ModelEntityFactory::CreateFromModel(*renderServices, *world, model, guid, rootName);
    if (!created.IsValid())
    {
        SetLastError("{}: the engine could not create the model's entities; the browser console has its log.",
                     kCall);
        return ECS::kInvalidEntity;
    }
    if (parent.IsValid() && !SetEntityParent(*world, created.rootEntity, parent, kCall))
        return ECS::kInvalidEntity;
    return created.rootEntity.id;
}

/// Sets the emission of an instantiated model's materials to `strength` times the brightness its
/// file gives them. The materials are the model's, shared by every instance of its load, and each
/// ge_instantiate_model of the load sets them back to the file's brightness.
EMSCRIPTEN_KEEPALIVE int32_t ge_model_emissive_strength(uint32_t handle, float strength)
{
    const AbiCallScope scope("ge_model_emissive_strength");
    if (scope.Refused() || RefuseAfterShutdown("ge_model_emissive_strength"))
        return kFailed;
    constexpr const char* kCall = "ge_model_emissive_strength";
    if (!std::isfinite(strength) || strength < 0.0f)
    {
        SetLastError("{}: the strength must be a finite number of 0 or more; it was {}.", kCall, strength);
        return kFailed;
    }
    WebLibraryApplication* application = GetRunningApplication();
    Engine::Renderer::RenderServices* renderServices = application ? application->GetRenderServices() : nullptr;
    if (!renderServices)
    {
        SetLastError("{} needs the renderer that ge_create brings up.", kCall);
        return kFailed;
    }
    GUID guid;
    const SharedPtr<Asset> asset = g_Loads.GetAsset(handle, guid);
    if (!asset || asset->GetType() != AssetType::Model)
    {
        SetLastError("{}: load {} is not a Ready model.", kCall, handle);
        return kFailed;
    }
    const auto& materials = static_cast<const ModelAsset&>(*asset).GetMaterials();
    auto& registry = renderServices->Materials().Registry();
    constexpr StringId kEmissionLuminance = HashStringId("emissionLuminance");
    for (uint32_t index = 0; index < materials.size(); ++index)
    {
        const Engine::Renderer::ConvertedModelMaterial converted =
            Engine::Renderer::ModelMaterialBridge::Convert(guid, index, materials[index]);
        const auto luminance = converted.document.properties.find("emissionLuminance");
        const float* nits = luminance == converted.document.properties.end() ? nullptr : std::get_if<float>(&luminance->second);
        if (!nits)
            continue;   // the file gives this material no emission
        Engine::Renderer::Material* material = registry.Find(converted.derivedGuid);
        if (!material)
        {
            SetLastError("{}: load {} has no material {}; instantiate the model first.", kCall, handle, index);
            return kFailed;
        }
        material->SetFloat(kEmissionLuminance, *nits * strength);
    }
    return kOk;
}

} // extern "C"
