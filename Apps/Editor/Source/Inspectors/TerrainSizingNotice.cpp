#include "Inspectors/TerrainSizingNotice.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "UI/Controls/InspectorNotice.h"

#include <array>
#include <cstdio>
#include <memory>

namespace GameEngine::Editor
{
namespace
{

// Metres per texel spans four orders of magnitude across the legal range (0.25 m on a small
// terrain to 500 m on a coarse far field), so a fixed precision reads as either noise or a lie.
std::string FormatMetres(float32 metres)
{
    std::array<char, 32> buf{};
    if (metres < 1.0f)
        std::snprintf(buf.data(), buf.size(), "%.2f m", metres);
    else if (metres < 100.0f)
        std::snprintf(buf.data(), buf.size(), "%.1f m", metres);
    else
        std::snprintf(buf.data(), buf.size(), "%.0f m", metres);
    return std::string(buf.data());
}

std::string FormatMebibytes(uint64 bytes)
{
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%.0f MiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return std::string(buf.data());
}

} // namespace

std::string DescribeTerrainSizing(const TerrainECS::TerrainSizingPlan& plan)
{
    if (plan.SampleResolution == 0u)
        return {};

    std::string text = std::to_string(plan.SampleResolution) + " x " +
                       std::to_string(plan.SampleResolution) + " samples, " +
                       FormatMetres(plan.MetresPerTexelNear) + " per texel";

    // Keyed on the SOURCE, not the tile count: the plan derives a tile grid for every terrain
    // (the service needs it), but a terrain below the tiling threshold is one heightmap and does
    // not stream, whatever that grid says.
    if (plan.Source == TerrainECS::TerrainHeightSource::Single)
        text += " - one tile, no streaming";
    else
        text += " - " + std::to_string(plan.TilesPerAxisX) + " x " +
                std::to_string(plan.TilesPerAxisZ) + " streamed tiles of " +
                FormatMetres(plan.TileWorldSize);

    return text;
}

std::string DescribeTerrainResidency(const TerrainECS::TerrainSizingPlan& plan)
{
    if (plan.SampleResolution == 0u)
        return {};

    const std::string vram = FormatMebibytes(plan.PredictedVramBytes);

    if (plan.Source != TerrainECS::TerrainHeightSource::Atlas)
        return "Costs " + vram + " of terrain textures, all of it at full detail.";

    // Engaging the atlas does NOT by itself cost detail: just past the unified ceiling the slot
    // budget still covers the whole tile grid. Only a grid that outruns the budget has a coarse
    // far field, so the two cases must not share a sentence — saying "beyond the window" of a
    // terrain that has no outside is a contradiction the author would have to disprove.
    if (plan.FullyResident())
        return "Streams all " + std::to_string(plan.TotalTiles) + " tiles at full detail for " +
               vram + ". Nothing falls back to the coarse field at this size.";

    // Past that point the window is what stays sharp, and everything beyond it resolves through
    // one coarse field spanning the whole world. That trade is the decision this notice exists
    // to surface.
    return "Streams " + std::to_string(plan.ResidentSlots) + " of " +
           std::to_string(plan.TotalTiles) + " tiles at full detail (a " +
           FormatMetres(plan.ResidentWindowMetres) + " window) for " + vram +
           ". Beyond the window the terrain falls back to " +
           FormatMetres(plan.MetresPerTexelFar) +
           " per texel, so distant ground reads smooth rather than detailed.";
}

void AddTerrainSizingNotice(UIElement* parent, const TerrainECS::TerrainSizingPlan& plan)
{
    if (!parent || plan.SampleResolution == 0u)
        return;

    InspectorUI::AddInfoCard(parent, DescribeTerrainSizing(plan));

    const std::string residency = DescribeTerrainResidency(plan);
    if (residency.empty())
        return;

    // A partially-resident terrain is a look decision the author has to make knowingly, not a
    // footnote: everything past the window is coarse until they change size, density or budget.
    if (plan.FullyResident())
        InspectorUI::AddInfoCard(parent, residency);
    else
        parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(residency));
}

} // namespace GameEngine::Editor
