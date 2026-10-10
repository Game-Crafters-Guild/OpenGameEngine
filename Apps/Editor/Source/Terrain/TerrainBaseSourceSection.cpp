#include "Terrain/TerrainBaseSourceSection.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Engine.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "Types/StringUtils.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/AssetField.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/InspectorNotice.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using InspectorDrag::CommitComponentWithUndo;

// Option values are the TerrainBaseSource enumerators, so the scene file, set_component and this
// dropdown all name a source by the same number.
const std::vector<Dropdown::Option>& BaseSourceOptions()
{
    static const std::vector<Dropdown::Option> kOptions = {
        {"0", "Procedural Noise"},
        {"1", "Heightmap"},
        {"2", "Flat"},
    };
    return kOptions;
}

void AddBaseSourceRow(UIElement* content, const InspectorContext& ctx, Components::TerrainBaseSource source)
{
    Dropdown* dropdown = InspectorUI::AddDropdownRow(
        content, "Base Source", BaseSourceOptions(), static_cast<int>(source),
        "Where the terrain's heights come from before its modifiers apply. Heightmap reads an imported "
        "heightmap asset (.r16, .r32 or a 16-bit PNG) spread over Size X and Size Z.");
    dropdown->SetId("terrain-base-source");
    dropdown->SetOnValueChanged(
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
         requestRefresh = ctx.RequestInspectorRefresh](const std::string& value)
        {
            const auto picked = static_cast<Components::TerrainBaseSource>(std::stoi(value));
            CommitComponentWithUndo<Components::Terrain>(w, e, n, undo, "Change Terrain Base Source",
                                                         [picked](Components::Terrain& u) { u.BaseSource = picked; });
            // The heightmap row belongs to the Heightmap source only.
            if (requestRefresh)
                requestRefresh();
        });
}

void AddHeightmapRow(UIElement* content, const InspectorContext& ctx, const GUID& heightmap)
{
    AssetField* field = InspectorUI::AddAssetFieldRow(
        content, "Heightmap", heightmap, {AssetType::TerrainHeightmap, AssetType::Texture},
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
         requestRefresh = ctx.RequestInspectorRefresh](const GUID& guid)
        {
            CommitComponentWithUndo<Components::Terrain>(w, e, n, undo, "Change Terrain Heightmap",
                                                         [guid](Components::Terrain& u) {
                                                             if (guid.IsNull())
                                                                 u.TerrainAssetGuid.Clear();
                                                             else
                                                                 u.TerrainAssetGuid.Set(guid);
                                                         });
            if (requestRefresh)
                requestRefresh();
        },
        ctx.Thumbnails,
        "The heightmap the terrain's base is read from. A raw .r16 or .r32 file needs its Samples X (or "
        "Samples Z) set in its own inspector unless it is square.");
    if (field)
        field->SetId("terrain-heightmap");
}

// Ratios that differ by less than this read as the same shape: a DEM's sample grid is the
// terrain's footprint to within a sample's rounding, never to within 1%.
constexpr double kShapeTolerance = 0.01;

// "4:1" or "1:4": the longer side first as a whole number where it is one.
std::string FormatRatio(double ratio)
{
    char text[32];
    if (ratio >= 1.0)
        std::snprintf(text, sizeof(text), "%.3g:1", ratio);
    else
        std::snprintf(text, sizeof(text), "1:%.3g", 1.0 / ratio);
    return text;
}

void AddSelectHeightmapAction(EditorUI::InspectorNotice& notice, const InspectorContext& ctx, const GUID& heightmap,
                              const char* tooltip)
{
    AssetMetadata meta{};
    if (ctx.PingAsset &&
        EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(heightmap, meta) &&
        !meta.Path.empty())
    {
        notice.SetAction("Select Heightmap", tooltip, [ping = ctx.PingAsset, path = meta.Path] { ping(path); });
    }
}

// A heightmap whose sample grid does not have the terrain's shape is stretched to fit it. When the
// transposed grid would fit, Samples X and Samples Z were most likely typed the wrong way round.
void AddShapeNotice(UIElement* content, const InspectorContext& ctx, const GUID& heightmap,
                    const Terrain::HeightfieldData& decoded, const Components::Terrain& terrain)
{
    if (decoded.GetWidth() < 2 || decoded.GetHeight() < 2 || terrain.SizeX <= 0.0f || terrain.SizeZ <= 0.0f)
        return;
    const double grid = static_cast<double>(decoded.GetWidth() - 1) / static_cast<double>(decoded.GetHeight() - 1);
    const double footprint = static_cast<double>(terrain.SizeX) / static_cast<double>(terrain.SizeZ);
    if (std::abs(grid / footprint - 1.0) <= kShapeTolerance)
        return;

    std::string text = "The heightmap is " + FormatGroupedInteger(decoded.GetWidth()) + " x " +
                       FormatGroupedInteger(decoded.GetHeight()) + " samples (" + FormatRatio(grid) +
                       ") but the terrain is " + FormatGroupedInteger(static_cast<uint64>(terrain.SizeX)) + " x " +
                       FormatGroupedInteger(static_cast<uint64>(terrain.SizeZ)) + " m (" + FormatRatio(footprint) +
                       "), so its relief is stretched to the terrain's shape.";
    if (std::abs((1.0 / grid) / footprint - 1.0) <= kShapeTolerance)
        text += " Samples X and Samples Z look swapped: the other way round, the heightmap fits.";
    else
        text += " Set Size X and Size Z to the area the heightmap covers.";
    auto notice = std::make_unique<EditorUI::InspectorNotice>(text);
    AddSelectHeightmapAction(*notice, ctx, heightmap, "Selects the heightmap so its Samples X and Samples Z can be checked.");
    content->AddChild(std::move(notice));
}

// Samples Per Meter above the heightmap's own density adds samples the heightmap has no data for, at
// the memory price of real ones. Match Heightmap sets the terrain to the heightmap's density along X.
void AddDensityNotice(UIElement* content, const InspectorContext& ctx, const Terrain::HeightfieldData& decoded,
                      const Components::Terrain& terrain)
{
    if (decoded.GetWidth() < 2 || terrain.SizeX <= 0.0f)
        return;
    const float32 heightmapDensity = static_cast<float32>(decoded.GetWidth() - 1) / terrain.SizeX;
    if (terrain.SamplesPerMeter <= heightmapDensity * (1.0f + static_cast<float32>(kShapeTolerance)))
        return;

    const uint64 asSet =
        TerrainECS::DeriveTerrainSizingPlan(terrain.SizeX, terrain.SizeZ, terrain.SamplesPerMeter).PredictedVramBytes;
    const uint64 matched =
        TerrainECS::DeriveTerrainSizingPlan(terrain.SizeX, terrain.SizeZ, heightmapDensity).PredictedVramBytes;
    char numbers[96];
    std::snprintf(numbers, sizeof(numbers), "%.1fx finer than the heightmap (%.3g per meter along X)",
                  terrain.SamplesPerMeter / heightmapDensity, heightmapDensity);
    auto notice = std::make_unique<EditorUI::InspectorNotice>(
        std::string("Samples Per Meter is ") + numbers + ": " + FormatMebibytes(asSet) +
            " of terrain maps where the heightmap's own density needs " + FormatMebibytes(matched) + ".",
        EditorUI::InspectorNotice::Kind::Information);
    notice->SetAction("Match Heightmap", "Sets Samples Per Meter to the heightmap's density.",
                      [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
                       requestRefresh = ctx.RequestInspectorRefresh, heightmapDensity]
                      {
                          CommitComponentWithUndo<Components::Terrain>(
                              w, e, n, undo, "Match Terrain To Heightmap",
                              [heightmapDensity](Components::Terrain& u) { u.SamplesPerMeter = heightmapDensity; });
                          if (requestRefresh)
                              requestRefresh();
                      });
    content->AddChild(std::move(notice));
}

// What the bound heightmap gives the terrain: nothing (no asset picked, or the decode's own reason,
// with the way to the fix), or a grid whose shape or density does not suit the terrain.
void AddHeightmapNotices(UIElement* content, const InspectorContext& ctx, const GUID& heightmap,
                         const Components::Terrain& terrain)
{
    if (heightmap.IsNull())
    {
        content->AddChild(std::make_unique<EditorUI::InspectorNotice>("No heightmap is picked, so the base is flat."));
        return;
    }
    // Resolving here decodes the heightmap now rather than on the terrain's next frame, so the panel
    // can say at once why a heightmap does not decode. The decode is cached either way.
    auto* service = TerrainECS::TerrainService::TryGet();
    if (!service)
        return;
    if (const std::shared_ptr<const Terrain::HeightfieldData> decoded = service->ResolveHeightmapAsset(heightmap))
    {
        AddShapeNotice(content, ctx, heightmap, *decoded, terrain);
        AddDensityNotice(content, ctx, *decoded, terrain);
        return;
    }
    auto notice = std::make_unique<EditorUI::InspectorNotice>("The heightmap does not decode: " +
                                                              service->GetHeightmapDecodeError(heightmap) +
                                                              ". The base is flat until it does.");
    AddSelectHeightmapAction(*notice, ctx, heightmap, "Selects the heightmap so its Samples X and Samples Z can be set.");
    content->AddChild(std::move(notice));
}

} // namespace

void AddTerrainBaseSourceSection(const InspectorContext& ctx)
{
    const auto* terrain = ctx.World ? ctx.World->GetComponent<Components::Terrain>(ctx.Entity) : nullptr;
    if (!terrain)
        return;
    Foldout* section = InspectorUI::AddComponentSection(ctx.Parent, "Terrain/Base", "Base");
    UIElement* content = section ? section->GetContentContainer() : nullptr;
    if (!content)
        return;

    AddBaseSourceRow(content, ctx, terrain->BaseSource);
    if (terrain->BaseSource != Components::TerrainBaseSource::HeightmapAsset)
        return;
    const GUID heightmap = terrain->TerrainAssetGuid.ToGuid();
    AddHeightmapRow(content, ctx, heightmap);
    AddHeightmapNotices(content, ctx, heightmap, *terrain);
}

} // namespace GameEngine
