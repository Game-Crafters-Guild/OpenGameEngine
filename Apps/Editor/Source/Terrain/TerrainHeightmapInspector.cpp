#include "Terrain/TerrainHeightmapInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Terrain/RawHeightmapSettingsCommit.h"
#include "TerrainECS/RawHeightmap.h"
#include "Types/StringUtils.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/IntField.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace GameEngine
{
namespace
{

struct SampleCountRow
{
    const char* Id;
    const char* Label;
    const char* Tooltip;
    const char* Key;
};

constexpr SampleCountRow kSamplesXRow{
    "terrain-heightmap-width", "Samples X",
    "Samples per row: the heightmap's extent along X. A raw file does not record it. Set it to the grid the "
    "heightmap was exported at; 0 reads it from the file and Samples Z (or assumes a square).",
    TerrainECS::kRawHeightmapWidthKey};
constexpr SampleCountRow kSamplesZRow{
    "terrain-heightmap-height", "Samples Z",
    "Rows: the heightmap's extent along Z. A raw file does not record it. Set it to the grid the heightmap was "
    "exported at; 0 reads it from the file and Samples X (or assumes a square).",
    TerrainECS::kRawHeightmapHeightKey};

// The facts a user checks a grid against: what the file is and how many samples it holds.
std::string FileFactsText(const std::filesystem::path& path)
{
    const bool r16 = ToLowerAscii(path.extension().string()) == ".r16";
    const std::string format = r16 ? "R16 unsigned 16-bit" : "R32 float";
    std::error_code ec;
    const uint64 bytes = std::filesystem::file_size(path, ec);
    if (ec)
        return format;
    return format + ", " + FormatGroupedInteger(bytes / (r16 ? 2u : 4u)) + " samples (" + FormatMebibytes(bytes) + ")";
}

// `readOnlyReason` non-empty: the store cannot take the setting here, so the row shows the value
// and says why it cannot be changed.
void AddSampleCountRow(UIElement* parent, const SampleCountRow& row, uint32 value, const std::filesystem::path& path,
                       const GUID& guid, const InspectorContext& ctx, const std::string& readOnlyReason)
{
    UIElement* line = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(line, row.Label, row.Tooltip);
    IntField* field = InspectorUI::AddInt(InspectorUI::AddFieldContainer(line), static_cast<int>(value));
    field->SetId(row.Id);
    field->SetTooltip(row.Tooltip);
    field->SetOnValueChanged(
        [path, guid, key = row.Key, undo = ctx.Undo, refresh = ctx.RequestInspectorRefresh](const int& v)
        { CommitRawHeightmapSampleCount(undo, path, guid, key, v > 0 ? static_cast<uint32>(v) : 0u, refresh); });
    if (!readOnlyReason.empty())
        InspectorUI::DisableRowOfControl(field, readOnlyReason.c_str());
}

// Why the grid cannot be edited here: empty when the asset's store takes import settings.
std::string ReadOnlyReason(const AssetImportSettingsOrigin& origin)
{
    if (origin.Writable)
        return {};
    if (origin.SourceAlias.empty())
        return "No mounted source owns this heightmap, so its Import Settings cannot be saved.";
    return "This heightmap belongs to " + origin.SourceAlias +
           ", whose Import Settings cannot be saved from here; set the grid where that source is authored.";
}

// What the file decodes to on the settings as they stand: the grid, or the reason it cannot.
// `settingsError` is a stored setting that could not be read, which is the reason when present.
std::unique_ptr<EditorUI::InspectorNotice> MakeDecodeNotice(const std::filesystem::path& path, uint32 width,
                                                            uint32 height, const std::string& settingsError)
{
    TerrainECS::RawHeightmapLayout layout;
    const std::string reason = !settingsError.empty()
                                   ? settingsError
                                   : TerrainECS::ResolveRawHeightmapFileLayout(path, width, height, layout);
    if (!reason.empty())
        return std::make_unique<EditorUI::InspectorNotice>("This heightmap does not decode: " + reason + ".");

    std::string text = "Reads as " + FormatGroupedInteger(layout.Width) + " x " +
                       FormatGroupedInteger(layout.Height) + " samples, row by row along X.";
    if (width == 0 && height == 0)
    {
        text += " No grid is set, so a square one is assumed: a non-square export whose sample count happens "
                "to be square (16384 x 4096) reads as stripes until Samples X or Samples Z is set.";
    }
    else if (width == 0 || height == 0)
    {
        text += width == 0 ? " Samples X is read from the file's sample count."
                           : " Samples Z is read from the file's sample count.";
    }
    return std::make_unique<EditorUI::InspectorNotice>(text, EditorUI::InspectorNotice::Kind::Information);
}

void BuildTerrainHeightmapInspector(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.Object)
        return;
    const auto* asset = static_cast<const Asset*>(ctx.Object);
    if (asset->GetType() != AssetType::TerrainHeightmap)
        return;

    const std::filesystem::path path = asset->GetPath();
    const GUID guid = asset->GetGUID();
    const AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

    InspectorUI::AddTextBlock(ctx.Parent, path.string(), "inspector-asset-path");
    InspectorUI::AddTextBlock(ctx.Parent, "Type: Terrain Heightmap", "inspector-asset-type-line");
    InspectorUI::AddTextBlock(ctx.Parent, FileFactsText(path), "inspector-asset-type-line");
    InspectorUI::AddTextBlock(ctx.Parent, "Import Settings", "inspector-section-subheader");

    // Each field shows its own stored value, so a malformed one does not blank the other. A stored
    // value that is not a number shows as 0, and the notice below quotes the stored text.
    const TerrainECS::RawHeightmapSettings settings = TerrainECS::ReadRawHeightmapSettings(registry, path);
    uint32 width = 0;
    uint32 height = 0;
    std::string settingsError =
        TerrainECS::ParseRawHeightmapSampleCount(TerrainECS::kRawHeightmapWidthKey, settings.Width, width);
    if (std::string heightError =
            TerrainECS::ParseRawHeightmapSampleCount(TerrainECS::kRawHeightmapHeightKey, settings.Height, height);
        settingsError.empty())
        settingsError = std::move(heightError);
    const std::string readOnlyReason = ReadOnlyReason(registry.GetImportSettingsOrigin(path));

    AddSampleCountRow(ctx.Parent, kSamplesXRow, width, path, guid, ctx, readOnlyReason);
    AddSampleCountRow(ctx.Parent, kSamplesZRow, height, path, guid, ctx, readOnlyReason);

    ctx.Parent->AddChild(MakeDecodeNotice(path, width, height, settingsError));

    if (ctx.ShowInfoCards)
    {
        InspectorUI::AddInfoCard(
            ctx.Parent,
            ".r32 samples are heights as they are: in meters when the terrain's Height Scale is 1. .r16 samples "
            "span 0 to the terrain's Height Scale. The terrain spreads the grid over its Size X and Size Z, so "
            "one heightmap sample spans Size X / (Samples X - 1) meters along X.");
    }
}

} // namespace

void RegisterTerrainHeightmapInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(AssetType::TerrainHeightmap, BuildTerrainHeightmapInspector);
}

} // namespace GameEngine
