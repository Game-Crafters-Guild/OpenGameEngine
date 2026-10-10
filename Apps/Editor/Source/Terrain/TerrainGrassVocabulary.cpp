#include "Terrain/TerrainGrassVocabulary.h"

namespace GameEngine::Editor::TerrainGrassVocabulary
{

std::string RenderModeTooltip(bool needsAlpha)
{
    std::string tooltip =
        "Dither: depth-correct dithered coverage (alpha-to-coverage under MSAA, "
        "screen-door otherwise). Blend: smooth alpha blending, far band drawn first; "
        "blades within a band composite in placement order.";
    if (!needsAlpha)
        tooltip += " Inactive while this grass carries no live alpha: with no alpha "
                   "anywhere in the view the draw is Opaque and neither mode applies.";
    return tooltip;
}

std::string RenderModeInactiveNote(bool needsAlpha)
{
    if (needsAlpha)
        return {};
    return "No live alpha here, so neither mode has anything to resolve and this row changes "
           "nothing — grass draws Opaque. To use Dither or Blend, enable Texture Grass and "
           "bind an albedo (or alpha map) that carries alpha. One draw serves every active "
           "terrain, so this row still counts while any other terrain needs the alpha path.";
}

std::string RangeTooltip()
{
    return "Distance at which density reaches zero, and the ceiling on how far grass is placed. "
           "The per-view instance budget can shorten it: near density is delivered as authored and "
           "distance is what pays for it, so a high density spends range.";
}

std::string RangeFitNote(float authoredRange, float deliveredRange, bool textureCards)
{
    if (!(deliveredRange > 0.0f) || deliveredRange >= authoredRange - 0.5f)
        return {};
    // Names the density the fit was actually run on. A card terrain is fitted on cards/m², so
    // telling its author to lower a blade density would name a dial that did not shorten anything.
    const std::string unit = textureCards ? "Cards per m²" : "Blades per m²";
    return "Delivering " + std::to_string(static_cast<int>(deliveredRange + 0.5f)) + " m of "
        + std::to_string(static_cast<int>(authoredRange + 0.5f)) + " m — the per-view instance "
        "budget is full at this density. " + unit + " at the camera are unaffected; lower the "
        "density or accept the shorter range.";
}

} // namespace GameEngine::Editor::TerrainGrassVocabulary
