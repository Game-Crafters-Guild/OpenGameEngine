// Isolation tests for the RenderGraph structural scheduler: dependency levels,
// component-contiguous emission (the Mac M1 TBDR interleave fix), working-set
// clusters with the dependency-forced split rule, and determinism. Pure graph
// algorithm, no GPU.

#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <initializer_list>
#include <vector>

using namespace GameEngine::Rendering::RenderGraph;

namespace
{
RGResourceId Tex(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Texture;
    d.Name = name;
    return g.CreateResource(d);
}
RGResourceId Buf(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Buffer;
    d.SizeBytes = 256;
    d.Name = name;
    return g.CreateResource(d);
}

int PositionOf(const RGGraph& g, RGPassId p)
{
    const auto& ord = g.ScheduledOrder();
    for (int i = 0; i < static_cast<int>(ord.size()); ++i)
        if (ord[i] == p)
            return i;
    return -1;
}

bool Contiguous(const RGGraph& g, std::initializer_list<RGPassId> passes)
{
    int mn = INT_MAX, mx = INT_MIN, found = 0;
    for (RGPassId p : passes)
    {
        const int pos = PositionOf(g, p);
        if (pos < 0)
            continue;
        mn = std::min(mn, pos);
        mx = std::max(mx, pos);
        ++found;
    }
    return found == static_cast<int>(passes.size()) &&
           (mx - mn) == static_cast<int>(passes.size()) - 1;
}

bool AppearsAfter(const RGGraph& g, RGPassId earlier, RGPassId later)
{
    return PositionOf(g, earlier) < PositionOf(g, later);
}
} // namespace

TEST(RGSchedule, LinearChainLevelsAndOrder)
{
    RGGraph g;
    g.BeginFrame();
    constexpr int kN = 6;
    RGResourceId res[kN];
    for (int i = 0; i < kN; ++i)
        res[i] = Tex(g, "R");
    g.MarkExternal(res[kN - 1]);

    RGPassId pass[kN];
    for (int i = 0; i < kN; ++i)
    {
        pass[i] = g.AddPass({"Link"});
        if (i > 0)
            g.Read(pass[i], res[i - 1], RGAccess::Sampled);
        g.Write(pass[i], res[i], RGAccess::ColorAttachment);
    }
    g.Compile();
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), static_cast<size_t>(kN));
    for (int i = 0; i < kN; ++i)
    {
        EXPECT_EQ(g.Level(pass[i]), static_cast<uint32_t>(i)) << "level at " << i;
        EXPECT_EQ(g.ScheduledOrder()[i], pass[i]) << "chain must schedule in order at " << i;
    }
}

TEST(RGSchedule, IndependentSinkWritersAreLevelZero)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Tex(g, "A");
    RGResourceId b = Tex(g, "B");
    g.MarkExternal(a);
    g.MarkExternal(b);
    RGPassId pa = g.AddPass({"WriteA"});
    g.Write(pa, a, RGAccess::ColorAttachment);
    RGPassId pb = g.AddPass({"WriteB"});
    g.Write(pb, b, RGAccess::ColorAttachment);
    g.Compile();
    g.Schedule();

    EXPECT_EQ(g.Level(pa), 0u);
    EXPECT_EQ(g.Level(pb), 0u);
    EXPECT_EQ(g.ScheduledOrder().size(), 2u);
}

// The Mac M1 case: an independent secondary view (preview) declared interleaved
// with the main scene. The preview is its own hazard-graph component, so it is
// emitted as one contiguous block BY CONSTRUCTION — its tile working-set is
// established once, never mid-scene.
TEST(RGSchedule, SecondaryViewComponentSchedulesContiguously)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId sceneDepth = Tex(g, "SceneDepth");
    RGResourceId sceneColor = Tex(g, "SceneColor");
    RGResourceId pvColor = Tex(g, "PreviewColor");
    RGResourceId pvDepth = Tex(g, "PreviewDepth");
    RGResourceId pvTonemap = Tex(g, "PreviewTonemapped");
    RGResourceId finalImg = Tex(g, "Final");
    g.MarkExternal(sceneColor);
    g.MarkExternal(finalImg);

    // Declared INTERLEAVED (scene/preview/scene/preview...) — the thrash setup.
    RGPassId sceneDepthPass = g.AddPass({"SceneDepth"});
    g.Write(sceneDepthPass, sceneDepth, RGAccess::DepthWrite);

    RGPassId previewWorld = g.AddPass({"PreviewWorld"});
    g.Write(previewWorld, pvColor, RGAccess::ColorAttachment);
    g.Write(previewWorld, pvDepth, RGAccess::DepthWrite);

    RGPassId sceneWorld = g.AddPass({"SceneWorld"});
    g.Read(sceneWorld, sceneDepth, RGAccess::DepthRead);
    g.Write(sceneWorld, sceneColor, RGAccess::ColorAttachment);

    RGPassId previewTonemap = g.AddPass({"PreviewTonemap"});
    g.Read(previewTonemap, pvColor, RGAccess::Sampled);
    g.Write(previewTonemap, pvTonemap, RGAccess::ColorAttachment);

    RGPassId previewEncode = g.AddPass({"PreviewEncode"});
    g.Read(previewEncode, pvTonemap, RGAccess::Sampled);
    g.Write(previewEncode, finalImg, RGAccess::ColorAttachment);

    g.Compile();
    ASSERT_EQ(g.LivePassCount(), 5u);
    g.Schedule();

    EXPECT_TRUE(Contiguous(g, {previewWorld, previewTonemap, previewEncode}))
        << "the preview component must be one contiguous block";
    EXPECT_TRUE(Contiguous(g, {sceneDepthPass, sceneWorld}))
        << "the scene component must be one contiguous block";
    EXPECT_TRUE(AppearsAfter(g, sceneDepthPass, sceneWorld));
    EXPECT_TRUE(AppearsAfter(g, previewWorld, previewTonemap));
    EXPECT_TRUE(AppearsAfter(g, previewTonemap, previewEncode));
}

// CSM-style: 4 cascade passes share one shadow-array attachment (one working
// set) with a foreign pass declared in the middle; the cascades cluster into a
// contiguous block and the shadow working-set is established exactly once.
TEST(RGSchedule, SharedAttachmentClusterStaysContiguous)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId shadow = Tex(g, "ShadowArray");
    RGResourceId other = Tex(g, "OtherTarget");
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(other);
    g.MarkExternal(color);

    RGPassId c0 = g.AddPass({"Cascade0"});
    g.Write(c0, shadow, RGAccess::DepthWrite);
    RGPassId foreign = g.AddPass({"ForeignPass"}); // declared between cascades
    g.Write(foreign, other, RGAccess::ColorAttachment);
    RGPassId c1 = g.AddPass({"Cascade1"});
    g.Write(c1, shadow, RGAccess::DepthWrite);
    RGPassId c2 = g.AddPass({"Cascade2"});
    g.Write(c2, shadow, RGAccess::DepthWrite);
    RGPassId c3 = g.AddPass({"Cascade3"});
    g.Write(c3, shadow, RGAccess::DepthWrite);
    RGPassId world = g.AddPass({"World"});
    g.Read(world, shadow, RGAccess::Sampled);
    g.Write(world, color, RGAccess::ColorAttachment);

    g.Compile();
    ASSERT_EQ(g.LivePassCount(), 6u);
    g.Schedule();

    EXPECT_TRUE(Contiguous(g, {c0, c1, c2, c3}))
        << "the four cascades share one working-set and must stay contiguous";
    EXPECT_EQ(g.WorkingSetId(c0), g.WorkingSetId(c1));
    EXPECT_EQ(g.WorkingSetId(c1), g.WorkingSetId(c2));
    EXPECT_EQ(g.WorkingSetId(c2), g.WorkingSetId(c3));
    EXPECT_TRUE(AppearsAfter(g, c3, world));
    // shadow set -> color -> other: the minimum possible for three distinct sets.
    EXPECT_EQ(g.WorkingSetTransitions(), 2u);
}

// Regression vs the previous greedy scheduler: a cross-cluster dependency
// (b1 -> a2) used to force an interleave (a1, b1, a2) because a1 won the
// insertion tiebreak. The cluster DAG orders B before the whole {a1, a2}
// cluster, keeping the shared-working-set pair contiguous.
TEST(RGSchedule, DependencyIntoClusterDoesNotSplitIt)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId wsA = Tex(g, "TargetA");
    RGResourceId wsB = Tex(g, "TargetB");
    RGResourceId bufX = Buf(g, "X");
    g.MarkExternal(wsA);
    g.MarkExternal(wsB);

    RGPassId a1 = g.AddPass({"A1"});
    g.Write(a1, wsA, RGAccess::ColorAttachment);
    RGPassId b1 = g.AddPass({"B1"});
    g.Write(b1, wsB, RGAccess::ColorAttachment);
    g.Write(b1, bufX, RGAccess::StorageWrite);
    RGPassId a2 = g.AddPass({"A2"});
    g.Write(a2, wsA, RGAccess::ColorAttachment); // same working set as a1
    g.Read(a2, bufX, RGAccess::StorageRead);     // but depends on b1

    g.Compile();
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), 3u);
    EXPECT_TRUE(Contiguous(g, {a1, a2}))
        << "b1 must be hoisted before the {a1,a2} cluster, not wedged inside it";
    EXPECT_TRUE(AppearsAfter(g, b1, a2));
}

// When a data dependency genuinely forces an outside pass BETWEEN two
// same-working-set passes (a1 -> x -> a2), the cluster must split rather than
// deadlock or emit an invalid order.
TEST(RGSchedule, ClusterSplitsWhenOutsidePassForcedBetween)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId target = Tex(g, "Target");
    RGResourceId bufX = Buf(g, "X");
    RGResourceId bufY = Buf(g, "Y");
    g.MarkExternal(target);

    RGPassId a1 = g.AddPass({"A1"});
    g.Write(a1, target, RGAccess::ColorAttachment);
    g.Write(a1, bufX, RGAccess::StorageWrite);
    RGPassId x = g.AddPass({"Middle"}); // neutral compute, no attachments
    g.Read(x, bufX, RGAccess::StorageRead);
    g.Write(x, bufY, RGAccess::StorageWrite);
    RGPassId a2 = g.AddPass({"A2"});
    g.Write(a2, target, RGAccess::ColorAttachment); // same working set as a1
    g.Read(a2, bufY, RGAccess::StorageRead);        // forced after x

    g.Compile();
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), 3u) << "split must not deadlock the cluster DAG";
    EXPECT_EQ(PositionOf(g, a1), 0);
    EXPECT_EQ(PositionOf(g, x), 1);
    EXPECT_EQ(PositionOf(g, a2), 2);
}

// Re-review fix (3): symmetric cross dependencies (a1→b2, b1→a2) make the
// working-set contraction {a1,a2}⇄{b1,b2} a 2-cycle even though the PASS graph
// is acyclic. The scheduler must split the stalled clusters and run all four
// passes — not cull them as a false "cycle".
TEST(RGSchedule, SymmetricCrossDependenciesScheduleAllPasses)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId texA = Tex(g, "TargetA");
    RGResourceId texB = Tex(g, "TargetB");
    RGResourceId outA1 = Buf(g, "OutA1");
    RGResourceId outB1 = Buf(g, "OutB1");
    g.MarkExternal(texA);
    g.MarkExternal(texB);

    RGPassId a1 = g.AddPass({"A1"});
    g.Write(a1, texA, RGAccess::ColorAttachment);
    g.Write(a1, outA1, RGAccess::StorageWrite);
    RGPassId b1 = g.AddPass({"B1"});
    g.Write(b1, texB, RGAccess::ColorAttachment);
    g.Write(b1, outB1, RGAccess::StorageWrite);
    RGPassId a2 = g.AddPass({"A2"});
    g.Write(a2, texA, RGAccess::ColorAttachment); // same working set as a1
    g.Read(a2, outB1, RGAccess::StorageRead);     // ...but depends on b1
    RGPassId b2 = g.AddPass({"B2"});
    g.Write(b2, texB, RGAccess::ColorAttachment); // same working set as b1
    g.Read(b2, outA1, RGAccess::StorageRead);     // ...but depends on a1

    g.Compile();
    ASSERT_EQ(g.LivePassCount(), 4u);
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), 4u) << "all four passes must run";
    EXPECT_EQ(g.LivePassCount(), 4u) << "no false Cycle culls";
    for (RGPassId p : {a1, b1, a2, b2})
        EXPECT_EQ(g.CullReason(p), RGCullReason::NotCulled);
    EXPECT_TRUE(AppearsAfter(g, a1, b2));
    EXPECT_TRUE(AppearsAfter(g, b1, a2));
}

// Slice 0b: the hazard-edge successor accessor that drives the MCP dependency
// listing (valid post-Schedule; no parallel edge structure retained).
TEST(RGSchedule, PassSuccessorsExposeHazardEdges)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId t = Tex(g, "T");
    RGResourceId outA = Tex(g, "OutA");
    RGResourceId outB = Tex(g, "OutB");
    g.MarkExternal(outA);
    g.MarkExternal(outB);
    RGPassId producer = g.AddPass({"Producer"});
    g.Write(producer, t, RGAccess::ColorAttachment);
    RGPassId consumer = g.AddPass({"Consumer"});
    g.Read(consumer, t, RGAccess::Sampled);
    g.Write(consumer, outA, RGAccess::ColorAttachment);
    RGPassId loner = g.AddPass({"Loner"});
    g.Write(loner, outB, RGAccess::ColorAttachment);
    g.Compile();
    g.Schedule();

    const auto& succ = g.PassSuccessors(producer);
    ASSERT_EQ(succ.size(), 1u);
    EXPECT_EQ(succ[0], consumer);
    EXPECT_TRUE(g.PassSuccessors(consumer).empty());
    EXPECT_TRUE(g.PassSuccessors(loner).empty());
}

TEST(RGSchedule, DeterministicAcrossRebuilds)
{
    auto build = [](RGGraph& g) -> std::vector<RGPassId>
    {
        g.BeginFrame();
        RGResourceId sceneDepth = Tex(g, "SceneDepth");
        RGResourceId sceneColor = Tex(g, "SceneColor");
        RGResourceId pvColor = Tex(g, "PreviewColor");
        RGResourceId finalImg = Tex(g, "Final");
        g.MarkExternal(sceneColor);
        g.MarkExternal(finalImg);
        RGPassId d = g.AddPass({"SceneDepth"});
        g.Write(d, sceneDepth, RGAccess::DepthWrite);
        RGPassId pw = g.AddPass({"PreviewWorld"});
        g.Write(pw, pvColor, RGAccess::ColorAttachment);
        RGPassId w = g.AddPass({"SceneWorld"});
        g.Read(w, sceneDepth, RGAccess::DepthRead);
        g.Write(w, sceneColor, RGAccess::ColorAttachment);
        RGPassId pe = g.AddPass({"PreviewEncode"});
        g.Read(pe, pvColor, RGAccess::Sampled);
        g.Write(pe, finalImg, RGAccess::ColorAttachment);
        g.Compile();
        g.Schedule();
        return g.ScheduledOrder();
    };

    RGGraph g;
    const std::vector<RGPassId> first = build(g);
    const std::vector<RGPassId> second = build(g); // same shape, rebuilt from scratch
    EXPECT_EQ(first, second) << "same graph must produce the identical schedule";
}

// Audit batch-1 pin (1a): a live predecessor edge INTO a cycle-culled pass must
// not corrupt the cluster contraction (clusterOf[culled] == kInvalidId used to
// index the contraction arrays out of bounds).
TEST(RGSchedule, CycleCullLivePredecessorEdgeDoesNotCorruptContraction)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId y = Buf(g, "Y");
    RGResourceId z = Buf(g, "Z");
    g.MarkExternal(x);
    g.MarkExternal(y);
    g.MarkExternal(z);
    RGPassId a = g.AddPass({"A"}); // live producer feeding the cycle
    g.Write(a, x, RGAccess::StorageWrite);
    RGPassId b = g.AddPass({"B"});
    RGPassId c = g.AddPass({"C"});
    g.Read(b, x, RGAccess::StorageRead); // RAW: A -> B (live edge into the cycle)
    // Interleaved records build B <-> C: WAR C->B (C reads Y before B writes it),
    // WAR B->C (B reads Z before C writes it).
    g.Read(c, y, RGAccess::StorageRead);
    g.Write(b, y, RGAccess::StorageWrite);
    g.Read(b, z, RGAccess::StorageRead);
    g.Write(c, z, RGAccess::StorageWrite);
    g.Compile();
    g.Schedule();

    EXPECT_NE(PositionOf(g, a), -1) << "the live producer must still schedule";
    EXPECT_EQ(PositionOf(g, b), -1);
    EXPECT_EQ(PositionOf(g, c), -1);
    EXPECT_TRUE(g.IsCulled(b));
    EXPECT_TRUE(g.IsCulled(c));
}

// ── AddOrderingEdge: explicit execution-order edges (no barrier, no lifetime) ──

TEST(RGSchedule, OrderingEdgeForcesScheduleOrder)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Tex(g, "A");
    RGResourceId b = Tex(g, "B");
    g.MarkExternal(a);
    g.MarkExternal(b);
    RGPassId pa = g.AddPass({"WriteA"}); // insertion tiebreak would emit A first
    g.Write(pa, a, RGAccess::ColorAttachment);
    RGPassId pb = g.AddPass({"WriteB"});
    g.Write(pb, b, RGAccess::ColorAttachment);
    g.AddOrderingEdge(pb, pa); // force B before A
    g.Compile();
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), 2u);
    EXPECT_TRUE(AppearsAfter(g, pb, pa)) << "ordering edge must override the insertion tiebreak";
    EXPECT_EQ(g.Level(pb), 0u);
    EXPECT_EQ(g.Level(pa), 1u) << "ordering edges feed dependency levels like hazard edges";
}

TEST(RGSchedule, OrderingEdgeUnionsComponents)
{
    // Two independent two-pass chains are separate weakly-connected components;
    // an ordering edge between them must union them so they emit as ONE
    // contiguous block in edge order (the M1 contiguity guarantee holds).
    RGGraph g;
    g.BeginFrame();
    RGResourceId r1 = Tex(g, "R1");
    RGResourceId s1 = Tex(g, "S1");
    RGResourceId r2 = Tex(g, "R2");
    RGResourceId s2 = Tex(g, "S2");
    g.MarkExternal(s1);
    g.MarkExternal(s2);
    RGPassId a1 = g.AddPass({"A1"});
    g.Write(a1, r1, RGAccess::ColorAttachment);
    RGPassId a2 = g.AddPass({"A2"});
    g.Read(a2, r1, RGAccess::Sampled);
    g.Write(a2, s1, RGAccess::ColorAttachment);
    RGPassId b1 = g.AddPass({"B1"});
    g.Write(b1, r2, RGAccess::ColorAttachment);
    RGPassId b2 = g.AddPass({"B2"});
    g.Read(b2, r2, RGAccess::Sampled);
    g.Write(b2, s2, RGAccess::ColorAttachment);
    g.AddOrderingEdge(a2, b1); // chain A fully before chain B
    g.Compile();
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), 4u);
    EXPECT_TRUE(Contiguous(g, {a1, a2, b1, b2}));
    EXPECT_TRUE(AppearsAfter(g, a2, b1));
    EXPECT_TRUE(AppearsAfter(g, a1, a2));
    EXPECT_TRUE(AppearsAfter(g, b1, b2));
}

TEST(RGSchedule, OrderingEdgeCycleCullsLoudly)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    g.MarkExternal(x);
    RGPassId ok = g.AddPass({"Healthy"});
    g.Write(ok, x, RGAccess::ColorAttachment);
    RGPassId a = g.AddPass({"CycleA"});
    g.PreventCulling(a);
    RGPassId b = g.AddPass({"CycleB"});
    g.PreventCulling(b);
    g.AddOrderingEdge(a, b);
    g.AddOrderingEdge(b, a);
    g.Compile();
    g.Schedule();

    EXPECT_NE(PositionOf(g, ok), -1) << "acyclic remainder must survive";
    EXPECT_EQ(PositionOf(g, a), -1);
    EXPECT_EQ(PositionOf(g, b), -1);
    EXPECT_TRUE(g.IsCulled(a));
    EXPECT_TRUE(g.IsCulled(b));
}

TEST(RGSchedule, OrderingEdgeDoesNotKeepPassesAlive)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Tex(g, "A");
    RGResourceId dead = Tex(g, "Dead");
    g.MarkExternal(a);
    RGPassId live = g.AddPass({"Live"});
    g.Write(live, a, RGAccess::ColorAttachment);
    RGPassId culled = g.AddPass({"Culled"}); // no sink, no PreventCulling
    g.Write(culled, dead, RGAccess::ColorAttachment);
    g.AddOrderingEdge(culled, live); // edge into a live pass is NOT a lifetime
    g.Compile();
    g.Schedule();

    EXPECT_TRUE(g.IsCulled(culled)) << "an ordering edge must not anchor culling";
    EXPECT_NE(PositionOf(g, live), -1);
    EXPECT_EQ(g.Level(live), 0u) << "edges from culled passes are dropped entirely";
}

TEST(RGSchedule, OrderingEdgeDuplicatesAndSelfEdgesAreHarmless)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Tex(g, "A");
    RGResourceId b = Tex(g, "B");
    g.MarkExternal(a);
    g.MarkExternal(b);
    RGPassId pa = g.AddPass({"WriteA"});
    g.Write(pa, a, RGAccess::ColorAttachment);
    RGPassId pb = g.AddPass({"WriteB"});
    g.Write(pb, b, RGAccess::ColorAttachment);
    g.AddOrderingEdge(pb, pa);
    g.AddOrderingEdge(pb, pa); // duplicate: must dedup, not inflate in-degree
    g.AddOrderingEdge(pb, pa);
    g.AddOrderingEdge(pa, pa); // self edge: dropped
    g.Compile();
    g.Schedule();

    ASSERT_EQ(g.ScheduledOrder().size(), 2u) << "no phantom in-degree may strand a pass";
    EXPECT_TRUE(AppearsAfter(g, pb, pa));
}
