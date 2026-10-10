// The surface term of a spline recipe's rebuild-settle observation. The
// controllers rebuild only after their observed inputs hold still for the
// settle window, so any term that moves every frame starves rebuilds for as
// long as it moves. The world's surface digest legitimately moves with tile
// streaming and modifier bakes — a recipe that never probes the ground must
// therefore contribute NO surface term, or unrelated terrain activity pins its
// settle clock at zero and its edits never regenerate.
//
// "Never probes the ground" is the load-bearing phrase, and it is NOT a synonym
// for "does not conform": an extrude recipe fitting its rings to the banks
// reads the ground at every station whatever its ConformMode. This suite pins
// both directions, because the two failures are opposites and each looks
// correct from the other's side — a missing term never re-fits, a blanket term
// starves.

#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplinePlacement.h"
#include "Placement/SplineSurfaceConform.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using Components::ReadsSurface;
using Components::SplineExtrude;
using Components::SplineExtrudeWidthMode;
using Components::SplinePlacementConform;
using Editor::ObservedSurfaceRevision;

namespace
{

// "Conforms at all" — the whole ground-read story for the placement and fence
// recipes, which have no width fit.
bool Conforms(SplinePlacementConform mode)
{
    return mode != SplinePlacementConform::None;
}

} // namespace

// THE starvation case: a None recipe observed across a moving digest must see
// the same value every frame, so terrain churn cannot reset its settle clock.
TEST(SplineSurfaceObservation, ANoneRecipeObservesNoSurfaceTerm)
{
    const uint64 digestFrameA = 0x1234'5678'9ABC'DEF0ull;
    const uint64 digestFrameB = 0x0FED'CBA9'8765'4321ull;

    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::None), digestFrameA),
              ObservedSurfaceRevision(Conforms(SplinePlacementConform::None), digestFrameB))
        << "a recipe that never probes the ground must be immune to surface churn";
    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::None), digestFrameA), 0u);
}

// Conforming recipes hold measured altitudes, so the digest passes through
// unchanged: ground movement must re-arm their rebuild.
TEST(SplineSurfaceObservation, AConformingRecipeObservesTheWorldDigest)
{
    const uint64 digest = 0xA5A5'5A5A'DEAD'BEEFull;

    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::Height), digest), digest);
    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::HeightAndSlope), digest), digest);
}

// The premise the None rule rested on — "a None recipe never probes the ground"
// — stopped being true when the extrude gained a width mode that measures the
// banks. A FitToBanks recipe reads the ground at every station whatever its
// ConformMode is, so it must observe the world digest: without it, carving the
// bed under a non-conforming river never re-fits its width, and the surface
// keeps the channel it was built against.
TEST(SplineSurfaceObservation, ANoneFitToBanksRecipeObservesTheWorldDigest)
{
    const uint64 digestFrameA = 0x1234'5678'9ABC'DEF0ull;
    const uint64 digestFrameB = 0x0FED'CBA9'8765'4321ull;

    SplineExtrude recipe{};
    recipe.ConformMode = SplinePlacementConform::None;
    recipe.WidthMode = SplineExtrudeWidthMode::FitToBanks;

    EXPECT_TRUE(ReadsSurface(recipe))
        << "fitting to banks is a ground read, conform mode notwithstanding";
    EXPECT_EQ(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA), digestFrameA);
    EXPECT_NE(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA),
              ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameB))
        << "ground movement must re-arm a fitted recipe's rebuild";
}

// The control that keeps the fix from becoming "always observe the digest":
// a None recipe on the authored width channel reads no ground and must stay
// immune to terrain churn, which is the starvation the None rule exists to
// prevent.
TEST(SplineSurfaceObservation, ANoneChannelRecipeStillObservesNoSurfaceTerm)
{
    const uint64 digestFrameA = 0x1234'5678'9ABC'DEF0ull;
    const uint64 digestFrameB = 0x0FED'CBA9'8765'4321ull;

    SplineExtrude recipe{};
    recipe.ConformMode = SplinePlacementConform::None;
    recipe.WidthMode = SplineExtrudeWidthMode::Channel;

    EXPECT_FALSE(ReadsSurface(recipe));
    EXPECT_EQ(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA), 0u);
    EXPECT_EQ(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA),
              ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameB));
}

// A conforming recipe reads the ground for its drape whatever its width mode,
// so neither mode may switch the surface term off.
TEST(SplineSurfaceObservation, AConformingRecipeReadsTheSurfaceInEitherWidthMode)
{
    SplineExtrude channel{};
    channel.ConformMode = SplinePlacementConform::Height;
    channel.WidthMode = SplineExtrudeWidthMode::Channel;
    EXPECT_TRUE(ReadsSurface(channel));

    SplineExtrude fitted{};
    fitted.ConformMode = SplinePlacementConform::Height;
    fitted.WidthMode = SplineExtrudeWidthMode::FitToBanks;
    EXPECT_TRUE(ReadsSurface(fitted));
}

// ---------------------------------------------------------------------------
// The readiness half: whether the ground is THERE yet, as opposed to whether it
// MOVED. The digest above cannot answer it — an unprovisioned terrain folds no
// terms, so it reads exactly like a settled one — and a recipe's first build has
// no earlier state to re-place from when the real ground finally arrives. These
// pin the gate that holds that first build, and each direction is a live failure
// mode: holding too little reinstates paving on rooftops, holding too much
// leaves a terrainless scene permanently empty.
// ---------------------------------------------------------------------------

using Editor::ConformSurfaceReadiness;
using Editor::DeferFirstBuild;
using Editor::kFirstBuildDeferBudgetSeconds;

namespace
{

// The call the controllers make, with the per-recipe defer state owned here.
struct DeferState
{
    float32 Seconds = 0.0f;
    bool Warned = false;

    bool Defer(ConformSurfaceReadiness readiness, bool readsSurface, bool appliedOnce,
               bool surfaceSettled, float32 deltaSeconds = 1.0f / 60.0f)
    {
        return DeferFirstBuild(readiness, readsSurface, appliedOnce, surfaceSettled, deltaSeconds,
                               /*entityId*/ 7u, Seconds, Warned);
    }
};

} // namespace

// THE case: a ground-reading recipe whose terrain is declared but not yet
// provisioned must not commit a first build. This is the whole defect -- on
// scene load every entity exists while terrain data arrives from the
// asset-resolve pump tens of seconds later, and a build taken in that window
// measures the props instead.
TEST(SplineSurfaceReadiness, DefersAFirstBuildWhileTheTerrainIsStillProvisioning)
{
    DeferState d;
    EXPECT_TRUE(d.Defer(ConformSurfaceReadiness::Provisioning, true, false, /*settled*/ true));
    EXPECT_GT(d.Seconds, 0.0f) << "the clock must run while it defers, or the budget below can "
                                  "never be spent";
}

// The second reason, and the one a readiness check alone would miss: the ground
// is here but still moving. A terrain fills its base heights on one frame and
// composes its modifiers on another, so Ready is not by itself a safe moment to
// take a first measurement.
TEST(SplineSurfaceReadiness, DefersAFirstBuildWhileAReadySurfaceIsStillMoving)
{
    DeferState d;
    EXPECT_TRUE(d.Defer(ConformSurfaceReadiness::Ready, true, false, /*settled*/ false));
}

// A first build is deferred on a Ready-but-moving surface WITHOUT having to have
// observed Provisioning first. This is not a preference — a latch on "saw
// Provisioning" was tried and falsified on a large island scene: the terrain's handles
// resolve before the recipes become eligible, so the recipes this gate exists
// for never observe that state, and the latch restored the whole defect.
TEST(SplineSurfaceReadiness, DefersOnAMovingSurfaceEvenIfProvisioningWasNeverObserved)
{
    DeferState d;
    // First call of this recipe's life, and the world is already Ready.
    EXPECT_TRUE(d.Defer(ConformSurfaceReadiness::Ready, true, false, /*settled*/ false))
        << "the recipes this gate exists for never see Provisioning; requiring it "
           "reinstates the defect";
}

// THE STARVATION GUARD. A tiled terrain bumps the surface digest for as long as
// tiles keep arriving, so "wait for the digest to hold still" can wait forever.
// An unbounded settle would trade paving-on-rooftops for no paving at all --
// a quieter defect, not a fixed one.
TEST(SplineSurfaceReadiness, StopsDeferringAReadySurfaceThatNeverSettles)
{
    DeferState d;
    ASSERT_TRUE(d.Defer(ConformSurfaceReadiness::Ready, true, false, false,
                        kFirstBuildDeferBudgetSeconds - 1.0f));
    EXPECT_FALSE(d.Warned);

    EXPECT_FALSE(d.Defer(ConformSurfaceReadiness::Ready, true, false, false, 2.0f))
        << "a surface that never stops moving must not strand the first build forever";
    EXPECT_TRUE(d.Warned);
}

// The opposite failure, and the constraint that rules out "just wait for a
// stable digest": a scene with no terrain at all has nothing coming, so its
// conforming recipes must place on sight rather than wait out the budget --
// and they must not be held for settling either.
TEST(SplineSurfaceReadiness, NeverDefersWhenTheSceneHasNoTerrain)
{
    DeferState d;
    EXPECT_FALSE(d.Defer(ConformSurfaceReadiness::NoSurface, true, false, /*settled*/ false));
    EXPECT_EQ(d.Seconds, 0.0f);
}

// A recipe that never probes the ground has nothing to wait for; deferring it
// would delay every non-conforming placement in a scene behind terrain load.
TEST(SplineSurfaceReadiness, NeverDefersARecipeThatDoesNotReadTheGround)
{
    DeferState d;
    EXPECT_FALSE(d.Defer(ConformSurfaceReadiness::Provisioning, false, false, false));
    EXPECT_EQ(d.Seconds, 0.0f);
}

// Only the FIRST build is gated. Later builds already re-place through the
// settle window, and deferring them would starve edits made while a terrain is
// mid-provision.
TEST(SplineSurfaceReadiness, NeverDefersOnceTheRecipeHasBuiltBefore)
{
    DeferState d;
    EXPECT_FALSE(d.Defer(ConformSurfaceReadiness::Provisioning, true, /*appliedOnce*/ true, false));
}

// Ready AND settled is the release edge -- the only state a first measurement
// can be trusted against.
TEST(SplineSurfaceReadiness, ReleasesTheBuildOnceTheSurfaceIsReadyAndSettled)
{
    DeferState d;
    ASSERT_TRUE(d.Defer(ConformSurfaceReadiness::Provisioning, true, false, true));
    EXPECT_FALSE(d.Defer(ConformSurfaceReadiness::Ready, true, false, /*settled*/ true));
    EXPECT_EQ(d.Seconds, 0.0f) << "the clock must reset when the ground is final, so a later "
                                  "provision gets the full budget again";
}

// Giving up is once, not every frame: re-warning would flood the log.
TEST(SplineSurfaceReadiness, WarnsOnlyOnceAfterGivingUp)
{
    DeferState d;
    ASSERT_FALSE(d.Defer(ConformSurfaceReadiness::Provisioning, true, false, true,
                         kFirstBuildDeferBudgetSeconds + 1.0f));
    ASSERT_TRUE(d.Warned);
    const bool warnedBefore = d.Warned;
    EXPECT_FALSE(d.Defer(ConformSurfaceReadiness::Provisioning, true, false, true, 1.0f));
    EXPECT_EQ(d.Warned, warnedBefore);
}
