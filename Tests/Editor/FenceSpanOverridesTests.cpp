// Writing a fence recipe's span overrides: what the scene view's span section
// and the fence's own override rows commit. The table is fixed-capacity, so
// every write is either placed or refused, never dropped.

#include <gtest/gtest.h>

#include "Components/Spline/SplineFence.h"
#include "Placement/FenceSpanOverrides.h"

using namespace GameEngine;
using Components::SplineFence;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

TEST(FenceSpanOverrides, ANewOverrideTakesTheFirstEmptySlotAndIsRewrittenInPlace)
{
    SplineFence recipe;
    recipe.Overrides[0] = {1u, 0u, SplineSpanOverrideKind::ExplicitPiece, 2u};
    ASSERT_TRUE(Editor::SetSpanOverride(recipe, 3u, 2u, SplineSpanOverrideKind::Gate, 0u));
    EXPECT_EQ(recipe.Overrides[1], (SplineSpanOverride{3u, 2u, SplineSpanOverrideKind::Gate, 0u}));
    EXPECT_EQ(Editor::CountSpanOverrides(recipe), 2u);

    // The same span again rewrites its slot rather than taking another.
    ASSERT_TRUE(Editor::SetSpanOverride(recipe, 3u, 2u, SplineSpanOverrideKind::ExplicitPiece, 4u));
    EXPECT_EQ(recipe.Overrides[1],
              (SplineSpanOverride{3u, 2u, SplineSpanOverrideKind::ExplicitPiece, 4u}));
    EXPECT_EQ(Editor::CountSpanOverrides(recipe), 2u);
    EXPECT_EQ(recipe.Overrides[0], (SplineSpanOverride{1u, 0u, SplineSpanOverrideKind::ExplicitPiece, 2u}));
}

TEST(FenceSpanOverrides, FindReturnsTheFirstFilledSlotNamingTheSpan)
{
    SplineFence recipe;
    EXPECT_EQ(Editor::FindSpanOverride(recipe, 0u, 0u), nullptr)
        << "an empty table is sixteen empty slots, not sixteen gates on the first span";
    recipe.Overrides[2] = {5u, 1u, SplineSpanOverrideKind::Gate, 0u};
    recipe.Overrides[6] = {5u, 1u, SplineSpanOverrideKind::ExplicitPiece, 3u};
    const SplineSpanOverride* found = Editor::FindSpanOverride(recipe, 5u, 1u);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, &recipe.Overrides[2]);
    EXPECT_EQ(Editor::FindSpanOverride(recipe, 5u, 2u), nullptr);
}

TEST(FenceSpanOverrides, NoneClearsEveryOverrideOnTheSpanAndNothingElse)
{
    SplineFence recipe;
    recipe.Overrides[2] = {5u, 1u, SplineSpanOverrideKind::Gate, 0u};
    recipe.Overrides[6] = {5u, 1u, SplineSpanOverrideKind::ExplicitPiece, 3u};
    recipe.Overrides[7] = {5u, 2u, SplineSpanOverrideKind::Gate, 0u};
    ASSERT_TRUE(Editor::SetSpanOverride(recipe, 5u, 1u, SplineSpanOverrideKind::None, 0u));
    EXPECT_EQ(recipe.Overrides[2], SplineSpanOverride{});
    EXPECT_EQ(recipe.Overrides[6], SplineSpanOverride{});
    EXPECT_EQ(recipe.Overrides[7], (SplineSpanOverride{5u, 2u, SplineSpanOverrideKind::Gate, 0u}));
}

TEST(FenceSpanOverrides, AFullTableRefusesANewSpanAndLeavesTheRecipeUntouched)
{
    SplineFence recipe;
    for (uint32 i = 0; i < Components::kSplineFenceMaxSpanOverrides; ++i)
        recipe.Overrides[i] = {0u, static_cast<uint16>(i), SplineSpanOverrideKind::Gate, 0u};
    const SplineFence before = recipe;
    EXPECT_FALSE(Editor::SetSpanOverride(recipe, 1u, 0u, SplineSpanOverrideKind::Gate, 0u));
    EXPECT_TRUE(recipe == before);
    // A span that already has a slot can still be changed.
    EXPECT_TRUE(Editor::SetSpanOverride(recipe, 0u, 3u, SplineSpanOverrideKind::ExplicitPiece, 1u));
    EXPECT_EQ(recipe.Overrides[3].Kind, SplineSpanOverrideKind::ExplicitPiece);
}
