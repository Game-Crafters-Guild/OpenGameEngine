#include "Inspectors/SkyboxInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/Skybox.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenService.h"
#include "EditorChangeNotifications.h"
#include "Editor/Assets/AssetRelativePath.h"
#include "EditorContext.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Dropdown.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

GUID SkyboxHDRIGuid(const Components::Skybox& skybox)
{
    if (skybox.HDRIAssetGuid[0] == '\0')
        return GUID::Null();
    GUID guid(std::string(skybox.GetHDRIAssetGuid()));
    return guid.IsNull() ? GUID::Null() : guid;
}

std::string SkyboxResolution(const Components::Skybox& skybox)
{
    return skybox.Resolution[0] == '\0' ? std::string("1k") : std::string(skybox.GetResolution());
}

std::string SkyboxSourceSlug(const Components::Skybox& skybox)
{
    if (skybox.SourceSlug[0] != '\0')
        return std::string(skybox.GetSourceSlug());

    if (skybox.HDRIPath[0] == '\0')
        return {};
    const std::filesystem::path p{skybox.GetHDRIPath()};
    auto it = p.begin();
    for (; it != p.end(); ++it)
    {
        if (*it == "Polyhaven")
        {
            ++it;
            if (it != p.end())
                return it->string();
            break;
        }
    }
    return {};
}

void NotifySkyboxChanged(ECS::World* world,
                         ECS::EntityHandle entity,
                         Editor::EditorChangeNotifications* notifications)
{
    if (!notifications)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent e{};
    e.world = world;
    e.entity = entity;
    e.componentType = ECS::GetComponentTypeId<Components::Skybox>();
    e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
    notifications->NotifyComponentChanged(e);
}

void ApplySkyboxHDRIGuid(ECS::World* world,
                         ECS::EntityHandle entity,
                         Editor::EditorChangeNotifications* notifications,
                         const GUID& guid)
{
    if (!world || !entity.IsValid())
        return;
    auto* current = world->GetComponent<Components::Skybox>(entity);
    if (!current)
        return;

    Components::Skybox updated = *current;
    if (guid.IsNull())
    {
        std::memset(updated.HDRIAssetGuid, 0, sizeof(updated.HDRIAssetGuid));
        std::memset(updated.HDRIPath, 0, sizeof(updated.HDRIPath));
        std::memset(updated.SourceSlug, 0, sizeof(updated.SourceSlug));
    }
    else
    {
        auto& am = EngineCore::GetInstance().GetAssetManager();
        AssetMetadata meta{};
        const std::string guidString = guid.ToString();
        std::memset(updated.HDRIAssetGuid, 0, sizeof(updated.HDRIAssetGuid));
        std::strncpy(updated.HDRIAssetGuid, guidString.c_str(), sizeof(updated.HDRIAssetGuid) - 1);
        std::memset(updated.HDRIPath, 0, sizeof(updated.HDRIPath));
        std::memset(updated.SourceSlug, 0, sizeof(updated.SourceSlug));
        if (am.GetRegistry().TryGetAssetMetadata(guid, meta) && !meta.Path.empty())
        {
            const std::string pathString = meta.Path.generic_string();
            std::strncpy(updated.HDRIPath, pathString.c_str(), sizeof(updated.HDRIPath) - 1);
            const std::string inferred = SkyboxSourceSlug(updated);
            if (!inferred.empty())
                std::strncpy(updated.SourceSlug, inferred.c_str(), sizeof(updated.SourceSlug) - 1);
        }
    }

    updated.HDRIIntensity = std::max(0.0f, updated.HDRIIntensity);
    world->AddComponentImmediate(entity, updated);
    NotifySkyboxChanged(world, entity, notifications);
}

void ApplySkyboxHDRIPath(ECS::World* world,
                         ECS::EntityHandle entity,
                         Editor::EditorChangeNotifications* notifications,
                         AssetManager* assets,
                         const std::filesystem::path& hdriPath,
                         const std::string& sourceSlug,
                         const std::string& resolution)
{
    if (!world || !entity.IsValid() || !assets || hdriPath.empty())
        return;
    auto* current = world->GetComponent<Components::Skybox>(entity);
    if (!current)
        return;

    GUID guid = assets->ResolveAssetGuid(hdriPath);
    if (guid.IsNull())
        return;

    Components::Skybox updated = *current;
    updated.HDRIIntensity = std::max(0.0f, updated.HDRIIntensity);
    if (updated.HDRIIntensity <= 0.0f)
        updated.HDRIIntensity = 1.0f;

    // HDRIPath is persisted into the scene, so it must be project-relative.
    // The path can arrive as a registry path (folded on case-insensitive
    // platforms) while the root carries its own spelling, and a lexical
    // subtraction compares case — it would store an absolute path.
    std::string pathString = Editor::TryMakeAssetRelativePathString(*assets, hdriPath);
    if (pathString.empty())
        pathString = hdriPath.generic_string();

    const std::string guidString = guid.ToString();
    std::memset(updated.HDRIAssetGuid, 0, sizeof(updated.HDRIAssetGuid));
    std::memset(updated.HDRIPath, 0, sizeof(updated.HDRIPath));
    std::memset(updated.SourceSlug, 0, sizeof(updated.SourceSlug));
    std::memset(updated.Resolution, 0, sizeof(updated.Resolution));
    std::strncpy(updated.HDRIAssetGuid, guidString.c_str(), sizeof(updated.HDRIAssetGuid) - 1);
    std::strncpy(updated.HDRIPath, pathString.c_str(), sizeof(updated.HDRIPath) - 1);
    std::strncpy(updated.SourceSlug, sourceSlug.c_str(), sizeof(updated.SourceSlug) - 1);
    std::strncpy(updated.Resolution, resolution.c_str(), sizeof(updated.Resolution) - 1);
    world->AddComponentImmediate(entity, updated);
    ECS::Entity(world, entity).SetEnabled<Components::Skybox>(true);
    NotifySkyboxChanged(world, entity, notifications);
}

int ResolutionOptionIndex(const std::string& value)
{
    static const std::vector<std::string> values = {"1k", "2k", "4k", "8k", "16k"};
    auto it = std::find(values.begin(), values.end(), value);
    return it == values.end() ? 0 : static_cast<int>(std::distance(values.begin(), it));
}

void ChangeSkyboxResolution(ECS::World* world,
                            ECS::EntityHandle entity,
                            Editor::EditorChangeNotifications* notifications,
                            const EditorContext* editorCtx,
                            std::string resolution)
{
    if (!world || !entity.IsValid())
        return;
    auto* current = world->GetComponent<Components::Skybox>(entity);
    if (!current)
        return;

    if (resolution.empty())
        resolution = "1k";

    const std::string slug = SkyboxSourceSlug(*current);
    if (!slug.empty() && editorCtx && editorCtx->Assets)
    {
        std::filesystem::path file = PolyhavenService::FindOrMoveDownloadedHDRI(slug, editorCtx->AssetsRoot, resolution);
        if (!file.empty())
        {
            ApplySkyboxHDRIPath(world, entity, notifications, editorCtx->Assets,
                                file, slug, resolution);
            if (editorCtx->OnSceneDirty)
                editorCtx->OnSceneDirty();
        }
        else if (editorCtx->DownloadManager)
        {
            if (resolution != "1k" && current->HDRIAssetGuid[0] == '\0')
            {
                const std::string previewResolution = "1k";
                std::filesystem::path previewFile = PolyhavenService::FindOrMoveDownloadedHDRI(
                    slug, editorCtx->AssetsRoot, previewResolution);
                if (!previewFile.empty())
                {
                    ApplySkyboxHDRIPath(world, entity, notifications, editorCtx->Assets,
                                        previewFile, slug, previewResolution);
                }
                else
                {
                    editorCtx->DownloadManager->AssignHDRIToSkyboxOnComplete(
                        slug, previewResolution, entity, notifications, true);
                    editorCtx->DownloadManager->StartHDRIDownload(slug, previewResolution, editorCtx->AssetsRoot);
                }
            }
            editorCtx->DownloadManager->AssignHDRIToSkyboxOnComplete(slug, resolution, entity, notifications);
            editorCtx->DownloadManager->StartHDRIDownload(slug, resolution, editorCtx->AssetsRoot);
            if (notifications)
            {
                Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                ev.world = world;
                ev.entity = entity;
                ev.componentType = ECS::GetComponentTypeId<Components::Skybox>();
                ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                notifications->NotifyComponentChanged(ev);
            }
        }
        return;
    }

    Components::Skybox updated = *current;
    updated.HDRIIntensity = std::max(0.0f, updated.HDRIIntensity);
    std::memset(updated.Resolution, 0, sizeof(updated.Resolution));
    std::strncpy(updated.Resolution, resolution.c_str(), sizeof(updated.Resolution) - 1);
    world->AddComponentImmediate(entity, updated);
    NotifySkyboxChanged(world, entity, notifications);

    if (editorCtx && editorCtx->OnSceneDirty)
        editorCtx->OnSceneDirty();
}

} // namespace

void RegisterSkyboxInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* skybox = ctx.World->GetComponent<Components::Skybox>(ctx.Entity);
        if (!skybox)
        {
            InspectorUI::AddLine(ctx.Parent, "(Skybox missing)");
            return;
        }

        // ctx.GetWorld is captured for any callback that may fire across
        // frames (drops are queued by DragDropManager, dropdown/value
        // callbacks may run during async refresh). Synchronous helpers below
        // can keep using `w` directly since they execute inline.
        auto getWorld = ctx.GetWorld;
        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        auto extras = InspectorDrag::GetAdditionalEntities(ctx);

        InspectorUI::AddAssetFieldRow(ctx.Parent, "HDRI", SkyboxHDRIGuid(*skybox), {AssetType::Texture},
            &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
            [getWorld, e, n](const GUID& guid)
            {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                ApplySkyboxHDRIGuid(world, e, n, guid);
            },
            ctx.Thumbnails);

        {
            static const std::vector<Dropdown::Option> kResolutionOptions = {
                {"1k", "1K"},
                {"2k", "2K"},
                {"4k", "4K"},
                {"8k", "8K"},
                {"16k", "16K"},
            };
            std::string resolutionRowLabel = "Resolution";
            std::string resolutionTooltip = "Polyhaven HDRI resolution";
            std::string pendingResolution;
            if (n && ctx.EditorCtx && ctx.EditorCtx->DownloadManager &&
                ctx.EditorCtx->DownloadManager->TryGetPendingHdriSkyboxResolution(e, pendingResolution))
            {
                resolutionRowLabel = "Resolution — Downloading";
                resolutionTooltip += " — fetching ";
                resolutionTooltip += pendingResolution;
                resolutionTooltip += "; viewport keeps the current HDRI until the download finishes.";
            }
            Dropdown* dd = InspectorUI::AddDropdownRow(
                ctx.Parent, resolutionRowLabel, kResolutionOptions,
                ResolutionOptionIndex(SkyboxResolution(*skybox)),
                resolutionTooltip.c_str());
            dd->SetOnValueChanged([getWorld, e, n, editorCtx = ctx.EditorCtx](const std::string& value)
            {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                ChangeSkyboxResolution(world, e, n, editorCtx, value);
            });
        }

        InspectorDrag::AddComponentFloatRowWithDrag<Components::Skybox>(ctx.Parent, "Intensity", skybox->HDRIIntensity, w, e, n,
            undo, "Change Skybox Intensity",
            [](Components::Skybox& u, float v) { u.HDRIIntensity = std::max(0.0f, v); },
            skybox->HDRIIntensity, "Brightness multiplier for the HDRI skybox", extras, 0.0f);

        InspectorDrag::AddComponentFloatRowWithDrag<Components::Skybox>(ctx.Parent, "IBL intensity", skybox->IblIntensity, w, e, n,
            undo, "Change Skybox IBL Intensity",
            [](Components::Skybox& u, float v) { u.IblIntensity = std::clamp(v, 0.0f, 4.0f); },
            skybox->IblIntensity, "Multiplier on image-based ambient lighting and reflections", extras, 0.0f, 4.0f);

        InspectorDrag::AddComponentFloatRowWithDrag<Components::Skybox>(ctx.Parent, "IBL ground darkening", skybox->IblLowerHemisphereDarkness, w, e, n,
            undo, "Change Skybox IBL Ground Darkening",
            [](Components::Skybox& u, float v) { u.IblLowerHemisphereDarkness = std::clamp(v, 0.0f, 1.0f); },
            skybox->IblLowerHemisphereDarkness,
            "Darkens the lower hemisphere of baked IBL only, leaving the visible skybox unchanged", extras, 0.0f, 1.0f);

        InspectorDrag::AddComponentFloatRowWithDrag<Components::Skybox>(ctx.Parent, "Rotation", skybox->RotationDegrees, w, e, n,
            undo, "Change Skybox Rotation",
            [](Components::Skybox& u, float v) { u.RotationDegrees = v; },
            skybox->RotationDegrees, "Y-axis rotation in degrees", extras);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::Skybox>(std::move(fn));
}

} // namespace GameEngine
