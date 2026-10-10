#include "Placement/FenceRecipeLayout.h"

namespace GameEngine::Editor
{

FenceLayoutParams FenceLayoutParamsOf(const Components::SplineFence& recipe,
                                      const FenceRecipePieces& pieces)
{
    FenceLayoutParams layout;
    layout.PostPitch = recipe.PostPitch;
    layout.SpanMaxStretch = recipe.SpanMaxStretch;
    layout.Seed = recipe.Seed;
    layout.SpanGrade = recipe.SpanGrade;
    layout.PlantMode = recipe.PlantMode;
    layout.AlignToSurfaceNormal =
        recipe.ConformMode == Components::SplinePlacementConform::HeightAndSlope;
    layout.SlopeBlend = recipe.SlopeBlend;
    layout.HasPostMesh = pieces.PostFootprint != nullptr;
    if (pieces.PostFootprint)
        layout.PostPiece = *pieces.PostFootprint;
    // Spans, gates, crests and caps DO vary in length within a pool — that is
    // the point of the fill, of the cell rule's pitch floor and of the cap fit —
    // so each active slot carries its own bounds.
    layout.SpanPieces = pieces.Spans;
    layout.GatePieces = pieces.Gates;
    layout.SpanOverrides = recipe.Overrides;
    layout.CrestPieces = pieces.Crests;
    layout.CapPieces = pieces.Caps;
    layout.CrestPitch = recipe.CrestPitch;
    return layout;
}

} // namespace GameEngine::Editor
