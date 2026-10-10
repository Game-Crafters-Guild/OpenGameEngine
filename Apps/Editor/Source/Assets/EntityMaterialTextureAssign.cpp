#include "Editor/Entities/EntityMaterialTextureAssign.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/EditorPaths.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace GameEngine::Editor
{

void BackfillMaterialTexturePaths(MaterialDocument& doc)
{
    if (!EngineCore::GetInstance().IsInitialized())
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    auto& registry = am.GetRegistry();
    const std::filesystem::path assetRoot = am.GetAssetRoot();

    for (const auto& [slot, guidStr] : doc.textures)
    {
        if (guidStr.empty())
            continue;
        auto pIt = doc.texturePaths.find(slot);
        if (pIt != doc.texturePaths.end() && !pIt->second.empty())
            continue; // already carries a path companion

        // Only real, resolvable GUID refs get a path companion. "__embedded:N"
        // sentinels and legacy path strings parse to Null and are left alone.
        const GUID g(guidStr);
        if (g.IsNull())
            continue;
        AssetMetadata meta{};
        if (!registry.TryGetAssetMetadata(g, meta) || meta.Path.empty())
            continue;

        // Store the texture's source-relative path exactly as the registry's own
        // canonical-relative helper computes it: lexical (temp+rename-safe), with
        // containment decided in the identity domain and the tail kept in the
        // path's own spelling. Returns false for a texture outside the project
        // root (editor/engine asset): those keep their stable stored GUID rather
        // than a wrong-root path that would re-derive to nothing.
        std::string relStr;
        if (!AssetRegistry::TryComputeCanonicalRelativePath(assetRoot, meta.Path, relStr))
            continue;
        doc.texturePaths[slot] = relStr;
    }
}

std::string SerializeMaterialDocumentForSave(const MaterialDocument& doc)
{
    // Upgrade guid-only texture slots to self-healing [path,guid] before writing,
    // so a plain save makes the material survive the asset-identity flip.
    MaterialDocument enriched = doc;
    BackfillMaterialTexturePaths(enriched);
    return SerializeMaterialDocument(enriched).dump(2);
}

namespace
{

// Create a new .material file in <project>/Assets/Materials/, register it with
// the asset system, register a runtime Material with RenderServices, and point
// the entity's MeshRenderer at the new GUID. Returns the new GUID on success,
// or GUID::Null() on failure.
// surfaceShaderOverride: optional path to surface shader (e.g., "Surfaces/standard_pbr_extended.glsl")
GUID CreateAndAssignFreshMaterial(ECS::World& world, ECS::EntityHandle entity,
                                    const char* surfaceShaderOverride = nullptr)
{
    auto projectPaths = GetCurrentEditorProjectPaths();
    if (projectPaths.workspaceRoot.empty())
        return GUID::Null();

    const std::filesystem::path dir = projectPaths.workspaceRoot / "Assets" / "Materials";
    auto& assetMgr = EngineCore::GetInstance().GetAssetManager();
    auto result = CreateDefaultMaterialFile(dir, "NewMaterial", nullptr, &assetMgr);
    if (result.path.empty())
        return GUID::Null();

    // Resolve through AssetManager, which registers the just-created file when
    // nothing has yet: a raw registry lookup finds nothing for an unregistered path.
    const GUID newGuid = assetMgr.ResolveAssetGuid(result.path);
    if (newGuid.IsNull())
        return GUID::Null();

    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR(result.path.stem().string());
        if (surfaceShaderOverride)
        {
            doc.surfaceShader = surfaceShaderOverride;
        }
        rs->RegisterAndPrewarmMaterial(newGuid, doc);
    }

    auto* mr = world.GetComponentForWrite<Components::MeshRenderer>(entity);
    if (!mr)
        return GUID::Null();
    mr->materialAssetGuid.Set(newGuid);

    return newGuid;
}

// Writes doc to disk at the given material asset path (matches
// MaterialInspector::SaveDocToDisk). Returns true on success. When outJson is
// non-null, also fills it with the serialized JSON written to disk.
bool WriteDocToDisk(const std::filesystem::path& path,
                    const MaterialDocument& doc,
                    std::string* outJson = nullptr)
{
    try
    {
        std::string text = SerializeMaterialDocumentForSave(doc);
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
        {
            Logger::Log::Warning("EntityMaterialTextureAssign: failed to open '{}'", path.string());
            return false;
        }
        out << text;
        if (outJson)
            *outJson = std::move(text);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("EntityMaterialTextureAssign: serialize failed: {}", e.what());
        return false;
    }
    return true;
}

} // namespace

void RemoveCreatedMaterialFile(const GUID& matGuid, const std::filesystem::path& path)
{
    if (!path.empty())
    {
        std::error_code ec;
        (void)std::filesystem::remove(path, ec);
    }

    if (!matGuid.IsNull())
    {
        auto& am = EngineCore::GetInstance().GetAssetManager();
        am.GetRegistry().UnregisterAsset(matGuid);
    }
}

bool RecreateMaterialFile(const std::filesystem::path& path, const std::string& serializedJson)
{
    if (path.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Logger::Log::Warning("EntityMaterialTextureAssign: failed to open '{}' for recreate",
                                 path.string());
            return false;
        }
        out << serializedJson;
    }

    auto& engine = EngineCore::GetInstance();
    auto& am = engine.GetAssetManager();

    // Single register-and-resolve entry point; the RegisterAsset + raw
    // GetAssetGUID pair it replaces performed the same sequence.
    GUID guid = GUID::Null();
    try
    {
        guid = am.ResolveAssetGuid(path);
    }
    catch (...)
    {
    }
    if (guid.IsNull())
        return false;

    // Load the asset so MaterialAsset::GetDocument() reflects the JSON on disk,
    // then re-register the runtime Material so RenderServices has state to
    // drive the GPU side (UpdateMaterialTextures expects it pre-registered).

    SharedPtr<Asset> asset = am.GetAsset(guid);
    if (!asset)
        asset = am.LoadAssetAsync(guid, AssetLoadPriority::High).get();
    auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get());
    if (!matAsset)
        return false;

    (void)matAsset->Reload();

    if (auto* rs = engine.GetRenderServices())
    {
        rs->RegisterAndPrewarmMaterial(guid, matAsset->GetDocument());
    }
    return true;
}

void SetEntityMaterialPointer(ECS::World& world,
                              ECS::EntityHandle entity,
                              const GUID& matGuid)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return;
    auto* mr = world.GetComponentForWrite<Components::MeshRenderer>(entity);
    if (!mr)
        return;

    if (matGuid.IsNull())
        mr->materialAssetGuid.Clear();
    else
        mr->materialAssetGuid.Set(matGuid);
}

bool RewriteMaterialTextureSlot(const GUID& matGuid,
                                std::string_view slotKey,
                                std::string_view slotValue)
{
    if (matGuid.IsNull())
        return true; // nothing to rewrite

    auto& engine = EngineCore::GetInstance();
    auto& am = engine.GetAssetManager();

    SharedPtr<Asset> asset = am.GetAsset(matGuid);
    if (!asset)
        asset = am.LoadAssetAsync(matGuid, AssetLoadPriority::High).get();
    auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get());
    if (!matAsset)
        return false;

    MaterialDocument doc = matAsset->GetDocument();
    doc.textures[std::string(slotKey)] = std::string(slotValue);
    doc.texturePaths.erase(std::string(slotKey)); // drop stale companion; back-fill re-derives

    if (!WriteDocToDisk(matAsset->GetPath(), doc))
        return false;

    (void)matAsset->Reload();

    if (auto* rs = engine.GetRenderServices())
    {
        rs->Materials().Compiler().Clear(matGuid);
        rs->Textures().UpdateMaterialTextures(matGuid, doc);
    }
    return true;
}

bool AssignTextureToEntityMaterialSlot(ECS::World& world,
                                       ECS::EntityHandle entity,
                                       const GUID& textureGuid,
                                       std::string_view slotKey,
                                       TextureOnMeshAssignState* outState)
{
    return AssignTextureToEntityMaterialSlot(world, entity, textureGuid, slotKey, outState, nullptr);
}

bool AssignTextureToEntityMaterialSlot(ECS::World& world,
                                       ECS::EntityHandle entity,
                                       const GUID& textureGuid,
                                       std::string_view slotKey,
                                       TextureOnMeshAssignState* outState,
                                       const char* surfaceShaderOverride)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return false;

    auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return false;

    // Capture MeshRenderer fields as they were before we touch anything.
    const GUID prevMatGuid = mr->materialAssetGuid.ToGuid();

    auto& engine = EngineCore::GetInstance();
    auto& am = engine.GetAssetManager();

    GUID matGuid = prevMatGuid;
    SharedPtr<Asset> asset;
    MaterialAsset* matAsset = nullptr;
    if (!matGuid.IsNull())
    {
        asset = am.GetAsset(matGuid);
        if (!asset)
            asset = am.LoadAssetAsync(matGuid, AssetLoadPriority::High).get();
        matAsset = dynamic_cast<MaterialAsset*>(asset.get());
    }

    // Material isn't file-backed (primitives use a synthetic default material).
    // Create a fresh .material file and point the MeshRenderer at it so the
    // texture assignment has a real document to edit.
    bool createdNewMaterial = false;
    if (!matAsset)
    {
        const GUID newGuid = CreateAndAssignFreshMaterial(world, entity, surfaceShaderOverride);
        if (newGuid.IsNull())
            return false;
        matGuid = newGuid;
        asset = am.GetAsset(matGuid);
        if (!asset)
            asset = am.LoadAssetAsync(matGuid, AssetLoadPriority::High).get();
        matAsset = dynamic_cast<MaterialAsset*>(asset.get());
        if (!matAsset)
            return false;
        createdNewMaterial = true;
    }

    MaterialDocument doc = matAsset->GetDocument();
    const std::string slot(slotKey);

    // Capture previous slot value for undo (only meaningful when we didn't
    // create a fresh material, since a fresh doc's slots are defaults).
    std::string prevSlotValue;
    if (!createdNewMaterial)
    {
        auto it = doc.textures.find(slot);
        if (it != doc.textures.end())
            prevSlotValue = it->second;
    }

    if (textureGuid.IsNull())
        doc.textures[slot] = "";
    else
        doc.textures[slot] = textureGuid.ToString();
    // Drop any stale path companion from a prior assignment to this slot — the
    // save-time back-fill re-derives a fresh [path] for the new GUID. Without
    // this, a re-assigned slot keeps {guid:new, path:old} and would self-heal to
    // the OLD texture if the new GUID ever became unresolvable.
    doc.texturePaths.erase(slot);

    std::string serializedJson;
    if (!WriteDocToDisk(matAsset->GetPath(), doc,
                        createdNewMaterial ? &serializedJson : nullptr))
        return false;

    (void)matAsset->Reload();

    if (auto* rs = engine.GetRenderServices())
    {
        rs->Materials().Compiler().Clear(matGuid);
        rs->Textures().UpdateMaterialTextures(matGuid, doc);
    }

    if (outState)
    {
        outState->entity = entity;
        outState->slotKey = slot;
        outState->textureGuid = textureGuid;

        outState->prevMaterialGuid = prevMatGuid;
        outState->prevSlotValue = std::move(prevSlotValue);
        outState->createdNewMaterial = createdNewMaterial;
        if (createdNewMaterial)
        {
            outState->newMaterialPath = matAsset->GetPath();
            outState->newMaterialJson = std::move(serializedJson);
        }

        outState->newMaterialGuid = matGuid;
    }

    return true;
}

} // namespace GameEngine::Editor
