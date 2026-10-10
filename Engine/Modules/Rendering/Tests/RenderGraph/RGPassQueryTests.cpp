#include "RGPassQuery.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// Matcher-predicate tests for the shared pass-name query seam. Every predicate
// must produce a NEGATIVE as well as a positive: an instrument that cannot say
// "no" is how a dead needle sat green for four weeks. The graph-walking
// wrappers (CountDeclared/ScheduledIndex/…) are exercised by the five suites
// that assert through them; these tests pin the matching rules themselves.

namespace
{
using namespace GameEngine::Testing;

TEST(RGPassQueryTests, ExactMatchesIdentityOnly)
{
    EXPECT_TRUE(RGQuery::Matches("GPUDrawStream.Scatter.World",
                                RGQuery::Exact{"GPUDrawStream.Scatter.World"}));
    // The defect class under repair: a sibling extending the name must NOT
    // satisfy a single-pass pin, in either direction.
    EXPECT_FALSE(RGQuery::Matches("GPUDrawStream.Scatter.World.B",
                                 RGQuery::Exact{"GPUDrawStream.Scatter.World"}));
    EXPECT_FALSE(RGQuery::Matches("GPUDrawStream.Scatter.World",
                                 RGQuery::Exact{"GPUDrawStream.Scatter.World.B"}));
    EXPECT_FALSE(RGQuery::Matches("", RGQuery::Exact{"X"}));
    EXPECT_TRUE(RGQuery::Matches("", RGQuery::Exact{""}));
}

TEST(RGPassQueryTests, SubtreeMatchesRootAndSeparatedDescendants)
{
    const RGQuery::Subtree q{"GPUCulling"};
    EXPECT_TRUE(RGQuery::Matches("GPUCulling", q));
    EXPECT_TRUE(RGQuery::Matches("GPUCulling.View7.C252", q));
    EXPECT_TRUE(RGQuery::Matches("GPUCulling.VisibilityUnion", q));
    // '_' and '[' are separators too (MsmCascade_Compute_0, RenderEntities[…]).
    EXPECT_TRUE(RGQuery::Matches("RenderEntities[GameView#4]", RGQuery::Subtree{"RenderEntities"}));
    EXPECT_TRUE(RGQuery::Matches("VolumetricFog[3].Media", RGQuery::Subtree{"VolumetricFog"}));
    EXPECT_TRUE(RGQuery::Matches("A_0", RGQuery::Subtree{"A"}));

    // NOT substring: a non-separator continuation is a DIFFERENT name.
    EXPECT_FALSE(RGQuery::Matches("GPUCullingX.View7", q));
    EXPECT_FALSE(RGQuery::Matches("GPUCulling.View10", RGQuery::Subtree{"GPUCulling.View1"}));
    // NOT contains: the root anchors at the START of the name.
    EXPECT_FALSE(RGQuery::Matches("Nested.GPUCulling.View7", q));
    // A truncated root names nothing.
    EXPECT_FALSE(RGQuery::Matches("GPUCulling.View7", RGQuery::Subtree{"GPUCullin"}));
}

TEST(RGPassQueryTests, FamilyRequiresSegmentBoundariesOnBothSides)
{
    // Mid-name families under a Pipeline.<bp>.<node>.View<N> prefix the test
    // does not control — the shapes this predicate was factored from.
    EXPECT_TRUE(RGQuery::Matches("Pipeline.MsmTest.CSM.View0.MsmCascade_Compute_2",
                                RGQuery::Family{"MsmCascade_Compute"}));
    EXPECT_TRUE(RGQuery::Matches("Pipeline.CsmTest.CSM.View3.Cascade1", RGQuery::Family{"Cascade"}));
    EXPECT_TRUE(RGQuery::Matches("Pipeline.HzbTwoView.HZB.View0.HZBBuild.Mip3",
                                RGQuery::Family{"HZBBuild"}));
    EXPECT_TRUE(RGQuery::Matches("Pipeline.GrassTest.Grass.View2.Compact",
                                RGQuery::Family{"Compact"}));
    // A name that IS the base alone (raw AddPass, no pipeline prefix).
    EXPECT_TRUE(RGQuery::Matches("TerrainHeightmapUpload", RGQuery::Family{"TerrainHeightmapUpload"}));

    // The alias closures substring matching could not make:
    // a base embedded in a LONGER segment is a different pass.
    EXPECT_FALSE(RGQuery::Matches("Pipeline.FP.ClusterDebugOverlay.View0",
                                 RGQuery::Family{"DebugOverlay"}));
    EXPECT_FALSE(RGQuery::Matches("Pipeline.MsmTest.CSM.View0.MsmCascade_Compute_2",
                                 RGQuery::Family{"Cascade"}));
    EXPECT_FALSE(RGQuery::Matches("Pipeline.GrassTest.Grass.View2.CompactExtra",
                                 RGQuery::Family{"Compact"}));
    // Non-numeric continuation after the base is not a parameter.
    EXPECT_FALSE(RGQuery::Matches("Pipeline.X.Y.View0.CascadeAll", RGQuery::Family{"Cascade"}));
    // An empty base names nothing (a rotted constant must read as absent,
    // not as everything).
    EXPECT_FALSE(RGQuery::Matches("AnyName", RGQuery::Family{""}));
}

TEST(RGPassQueryTests, SnapshotHelpersCountAndLocate)
{
    const std::vector<std::string> names = {
        "GPUCulling.View0.C1",
        "RenderEntities[HeadlessView#4]",
        "HeadlessView.Finalized",
        "GPUCulling.VisibilityUnion",
    };
    EXPECT_EQ(RGQuery::CountIn(names, RGQuery::Subtree{"GPUCulling"}), 2u);
    EXPECT_EQ(RGQuery::CountIn(names, RGQuery::Exact{"HeadlessView.Finalized"}), 1u);
    // Exact refuses the shorter sibling; the union of the two arms is the
    // caller's explicit two-query job, not a fuzzy prefix.
    EXPECT_EQ(RGQuery::CountIn(names, RGQuery::Exact{"HeadlessView.Finalize"}), 0u);
    EXPECT_EQ(RGQuery::CountIn(names, RGQuery::Subtree{"NoSuchRoot"}), 0u);

    const auto idx = RGQuery::IndexIn(names, RGQuery::Subtree{"RenderEntities"});
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1u);
    EXPECT_FALSE(RGQuery::IndexIn(names, RGQuery::Exact{"GPUCulling"}).has_value());
}

} // namespace
