// What a fence layout takes from its recipe: the controller builds its layout
// parameters through FenceLayoutParamsOf and adds only what the scene decides,
// so these tests read a recipe exactly the way a rebuild does. The controller
// itself needs live RenderServices to resolve its pools and is compiled into no
// test target.

#include <gtest/gtest.h>

#include "Components/Spline/SplineFence.h"
#include "Placement/FenceLayout.h"
#include "Placement/FenceRecipeLayout.h"
#include "Placement/RecipeValidation.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace GameEngine;
using Components::SplineFence;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;
using Editor::CenterSample;
using Editor::FenceLayoutParams;
using Editor::FenceLayoutResult;
using Editor::FencePieceBounds;
using Editor::FenceRecipePieces;
using V3 = Mathematics::Vector3;

namespace
{

// The castle kit's 5 m wall, corner-pivoted, laid along local X.
FencePieceBounds CastleWall()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 2.5f, 0.25f);
    b.Center = V3(-2.5f, 2.5f, 0.0f);
    return b;
}

// Its battlement: as long as the wall, so the row registers to the walls.
FencePieceBounds CastleBattlement()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 0.5f, 0.25f);
    b.Center = V3(-2.5f, 0.5f, 0.0f);
    return b;
}

// Its gatehouse, 1.47 walls long.
FencePieceBounds CastleGate()
{
    FencePieceBounds b;
    b.HalfExtents = V3(3.675f, 3.0f, 0.4f);
    b.Center = V3(-3.675f, 3.0f, 0.0f);
    return b;
}

// One wall's worth of straight run down +Z: a single span, so a gate on it is
// the whole registered row.
FenceLayoutResult BuildOneWallRun(const SplineFence& recipe, const FenceRecipePieces& pieces)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i <= 10u; ++i)
        center.push_back({V3(0.0f, 0.0f, 0.5f * static_cast<float32>(i)), V3(0, 1, 0), true});
    const float32 boundaries[2] = {0.0f, 10.0f};
    FenceLayoutParams params = Editor::FenceLayoutParamsOf(recipe, pieces);
    params.RunBoundaries = boundaries;
    return Editor::BuildFenceLayout(center, params);
}

bool AnyValidationContains(const FenceLayoutResult& result, const std::string& needle)
{
    return std::any_of(result.Validation.begin(), result.Validation.end(),
                       [&](const std::string& line) { return line.find(needle) != std::string::npos; });
}

} // namespace

// The recipe's overrides and gate pool reach the layout: a span marked as a
// gate is laid as one, from the gate pool.
TEST(FenceRecipeLayout, TheRecipesOverridesAndGatePoolReachTheLayout)
{
    SplineFence recipe;
    recipe.Overrides[3] = {0u, 0u, SplineSpanOverrideKind::Gate, 0u};
    const FencePieceBounds spans[1] = {CastleWall()};
    const FencePieceBounds gates[1] = {CastleGate()};
    FenceRecipePieces pieces;
    pieces.Spans = spans;
    pieces.Gates = gates;

    const FenceLayoutParams params = Editor::FenceLayoutParamsOf(recipe, pieces);
    ASSERT_EQ(params.SpanOverrides.size(), Components::kSplineFenceMaxSpanOverrides);
    EXPECT_EQ(params.SpanOverrides.data(), recipe.Overrides);
    EXPECT_EQ(params.GatePieces.size(), 1u);
    EXPECT_FALSE(params.HasPostMesh);

    const FenceLayoutResult result = BuildOneWallRun(recipe, pieces);
    ASSERT_EQ(result.Spans.size(), 1u);
    EXPECT_TRUE(result.Spans[0].IsGate);
}

// The editor's path to the message a registered crest row gives when a
// reservation takes every wall: before gates, nothing the editor built could
// reserve a registered row, so the message could only come from a caller of
// the layout. A gate on the run's one wall reaches it, and the editor's report
// logs it once.
TEST(FenceRecipeLayout, AGateOnTheOnlyWallReachesTheEmptyRegisteredRowMessage)
{
    const FencePieceBounds spans[1] = {CastleWall()};
    const FencePieceBounds gates[1] = {CastleGate()};
    const FencePieceBounds battlements[1] = {CastleBattlement()};
    FenceRecipePieces pieces;
    pieces.Spans = spans;
    pieces.Gates = gates;
    pieces.Crests = battlements;
    const std::string message = "a reserved stretch reaches every wall they stand on";

    SplineFence walled;
    const FenceLayoutResult control = BuildOneWallRun(walled, pieces);
    EXPECT_EQ(control.Crests.size(), 1u);
    EXPECT_FALSE(AnyValidationContains(control, message));

    SplineFence gated;
    gated.Overrides[0] = {0u, 0u, SplineSpanOverrideKind::Gate, 0u};
    const FenceLayoutResult result = BuildOneWallRun(gated, pieces);
    EXPECT_TRUE(result.Crests.empty());
    ASSERT_TRUE(AnyValidationContains(result, message));

    std::vector<std::string> logged;
    EXPECT_TRUE(Editor::ReportRecipeValidation(result.Validation, "Gatehouse", 9u, logged));
    EXPECT_FALSE(Editor::ReportRecipeValidation(result.Validation, "Gatehouse", 9u, logged))
        << "the same report twice is logged once";
}

// Everything else the layout reads from a recipe is carried over as authored.
TEST(FenceRecipeLayout, EveryRecipeFieldTheLayoutReadsIsCarriedOver)
{
    SplineFence recipe;
    recipe.PostPitch = 3.5f;
    recipe.SpanMaxStretch = 1.4f;
    recipe.Seed = 17u;
    recipe.SpanGrade = Components::SplineSpanGrade::Sheared;
    recipe.PlantMode = Components::SplinePlantMode::BoundsMin;
    recipe.ConformMode = Components::SplinePlacementConform::HeightAndSlope;
    recipe.SlopeBlend = 0.25f;
    recipe.CrestPitch = 2.5f;
    const FencePieceBounds post = CastleGate();
    FenceRecipePieces pieces;
    pieces.PostFootprint = &post;

    const FenceLayoutParams params = Editor::FenceLayoutParamsOf(recipe, pieces);
    EXPECT_EQ(params.PostPitch, 3.5f);
    EXPECT_EQ(params.SpanMaxStretch, 1.4f);
    EXPECT_EQ(params.Seed, 17u);
    EXPECT_EQ(params.SpanGrade, Components::SplineSpanGrade::Sheared);
    EXPECT_EQ(params.PlantMode, Components::SplinePlantMode::BoundsMin);
    EXPECT_TRUE(params.AlignToSurfaceNormal);
    EXPECT_EQ(params.SlopeBlend, 0.25f);
    EXPECT_EQ(params.CrestPitch, 2.5f);
    EXPECT_TRUE(params.HasPostMesh);
    EXPECT_EQ(params.PostPiece.HalfExtents.x, post.HalfExtents.x);
    EXPECT_EQ(params.PostPiece.Center.x, post.Center.x);

    recipe.ConformMode = Components::SplinePlacementConform::Height;
    EXPECT_FALSE(Editor::FenceLayoutParamsOf(recipe, pieces).AlignToSurfaceNormal);
}
