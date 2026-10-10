#include "Inspectors/OceanDepthCacheSourceInspector.h"

#include "Components/Rendering/Ocean.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "Engine/Rendering/OceanDepthCacheBaker.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{

std::string ReadCachePath(const Components::OceanDepthCacheSource& source)
{
    const GUID assetGuid = source.CacheAsset.ToGuid();
    if (!assetGuid.IsNull())
    {
        AssetMetadata metadata{};
        AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        if (registry.TryGetAssetMetadata(assetGuid, metadata) &&
            (metadata.Type == AssetType::OceanDepthCache || metadata.Type == AssetType::Unknown))
        {
            return metadata.Path.string();
        }
    }

    return std::string(source.GetPath());
}

Ocean::OceanDepthCacheBakeDesc MakeBakeDesc(const Components::OceanDepthCacheSource& source)
{
    Ocean::OceanDepthCacheBakeDesc desc{};
    desc.Width = std::clamp<uint32>(source.BakeWidth, 1u, 8192u);
    desc.Height = std::clamp<uint32>(source.BakeHeight, 1u, 8192u);
    desc.OriginX = source.BakeOriginX;
    desc.OriginZ = source.BakeOriginZ;
    desc.SizeX = source.BakeSizeX;
    desc.SizeZ = source.BakeSizeZ;
    desc.SeaLevel = source.BakeSeaLevel;
    desc.DeepWaterDepth = source.BakeDeepWaterDepth;
    return desc;
}

std::string FormatBakeSuccess(const std::filesystem::path& path,
                              const Engine::Renderer::OceanSceneDepthBakeStats& stats)
{
    std::ostringstream oss;
    oss << "Saved " << path.string()
        << " from " << stats.MeshSources << " mesh source";
    if (stats.MeshSources != 1u)
        oss << "s";
    oss << " and " << stats.TerrainSources << " terrain source";
    if (stats.TerrainSources != 1u)
        oss << "s";
    oss << " (" << stats.Triangles << " tris).";
    if (stats.SkippedUntagged > 0u)
        oss << " Skipped " << stats.SkippedUntagged << " untagged mesh";
    if (stats.SkippedUntagged > 1u)
        oss << "es";
    if (stats.SkippedUntagged > 0u)
        oss << ".";
    return oss.str();
}

void AddBakeAction(const InspectorContext& ctx)
{
    if (!ctx.Parent)
        return;

    InspectorUI::AddTextBlock(ctx.Parent, "Bake", "inspector-section-subheader");

    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-asset-actions");
    row->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::AlignSelf, AlignItems::Stretch);
    UIElement* rowPtr = row.get();
    ctx.Parent->AddChild(std::move(row));

    auto button = std::make_unique<Button>();
    button->SetText("Bake Scene Mesh Depth Cache");
    button->AddClass("inspector-button");
    button->SetTooltip(
        "Bake scene MeshRenderer geometry matching the source bake settings into Path.");
    Button* buttonPtr = button.get();
    rowPtr->AddChild(std::move(button));

    auto dirtyButton = std::make_unique<Button>();
    dirtyButton->SetText("Mark Depth Cache Dirty");
    dirtyButton->AddClass("inspector-button");
    dirtyButton->SetTooltip("Increment SourceRevision so tools/runtime know the baked cache is stale.");
    Button* dirtyButtonPtr = dirtyButton.get();
    rowPtr->AddChild(std::move(dirtyButton));

    auto status = std::make_unique<Label>();
    status->AddClass("inspector-text");
    status->SetText("Uses the bake bounds, sea level, resolution, and render layer mask above.");
    Label* statusLabel = status.get();
    rowPtr->AddChild(std::move(status));

    dirtyButtonPtr->RegisterEventHandler(kEventButtonClick, [ctx, statusLabel](UIEvent&)
    {
        ECS::World* world = ctx.GetWorld ? ctx.GetWorld() : ctx.World;
        if (!world || !ctx.Entity.IsValid())
        {
            if (statusLabel)
                statusLabel->SetText("Dirty mark failed: no active ECS world.");
            return;
        }

        const auto* source = world->GetComponent<Components::OceanDepthCacheSource>(ctx.Entity);
        if (!source)
        {
            if (statusLabel)
                statusLabel->SetText("Dirty mark failed: OceanDepthCacheSource is missing.");
            return;
        }

        Components::OceanDepthCacheSource updated = *source;
        ++updated.SourceRevision;
        Editor::CommitComponentUpdate(world, ctx.Entity, ctx.ChangeNotifications, updated);
        if (ctx.EditorCtx && ctx.EditorCtx->OnSceneDirty)
            ctx.EditorCtx->OnSceneDirty();

        if (statusLabel)
        {
            std::ostringstream oss;
            oss << "Marked dirty: source revision " << updated.SourceRevision
                << ", baked revision " << updated.BakedRevision << ".";
            statusLabel->SetText(oss.str());
        }
    });

    buttonPtr->RegisterEventHandler(kEventButtonClick, [ctx, statusLabel](UIEvent&)
    {
        ECS::World* world = ctx.GetWorld ? ctx.GetWorld() : ctx.World;
        if (!world || !ctx.Entity.IsValid())
        {
            if (statusLabel)
                statusLabel->SetText("Bake failed: no active ECS world.");
            return;
        }

        const auto* source = world->GetComponent<Components::OceanDepthCacheSource>(ctx.Entity);
        if (!source)
        {
            if (statusLabel)
                statusLabel->SetText("Bake failed: OceanDepthCacheSource is missing.");
            return;
        }

        const std::string pathText = ReadCachePath(*source);
        if (pathText.empty())
        {
            if (statusLabel)
                statusLabel->SetText("Bake failed: set Path first.");
            return;
        }

        Engine::Renderer::OceanSceneDepthBakeOptions options{};
        options.RenderLayerMask = source->RenderLayerMask;
        options.LoadMissingAssets = source->BakeLoadMissingAssets;
        options.IncludeDisabledRenderers = source->BakeIncludeDisabledRenderers;
        options.IncludeSkinnedMeshes = source->BakeIncludeSkinnedMeshes;
        options.IncludeTerrainHeightfields = source->BakeIncludeTerrainHeightfields;
        options.OnlyOceanMeshDepthContributors = source->BakeOnlyMeshDepthContributors;

        Engine::Renderer::OceanSceneDepthBakeStats stats{};
        std::string error;
        const std::filesystem::path outPath(pathText);
        const bool ok = Engine::Renderer::SaveOceanDepthCacheFromSceneMeshes(
            *world, MakeBakeDesc(*source), options, outPath, &stats, &error);

        if (ok)
        {
            if (const auto* current = world->GetComponent<Components::OceanDepthCacheSource>(ctx.Entity))
            {
                Components::OceanDepthCacheSource updated = *current;
                updated.BakedRevision = updated.SourceRevision;
                ++updated.CacheRevision;
                Editor::CommitComponentUpdate(world, ctx.Entity, ctx.ChangeNotifications, updated);
                if (ctx.EditorCtx && ctx.EditorCtx->OnSceneDirty)
                    ctx.EditorCtx->OnSceneDirty();
            }
        }

        if (statusLabel)
        {
            statusLabel->SetText(ok ? FormatBakeSuccess(outPath, stats)
                                    : ("Bake failed: " + error));
        }
    });
}

} // namespace

void RegisterOceanDepthCacheSourceInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        RenderDefaultComponentInspector(ctx, ECS::GetComponentTypeId<Components::OceanDepthCacheSource>());
        AddBakeAction(ctx);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::OceanDepthCacheSource>(std::move(fn));
}

} // namespace GameEngine
