// Stage 1.1 isolation tests for the RenderGraph graph builder + ref-count cull.
// Pure graph algorithm over declared accesses — no GPU, no device.

#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGRealize.h"

#include <gtest/gtest.h>

#include <cstdio>

using namespace GameEngine::Rendering;     // TextureDesc/TextureFormat for the realize checks
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
} // namespace

TEST(RGCull, PassWithNoConsumedOutputIsCulled)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGPassId p = g.AddPass({"WritesNothingUseful"});
    g.Write(p, x, RGAccess::ColorAttachment); // X is never read, never external
    g.Compile();

    EXPECT_TRUE(g.IsCulled(p));
    EXPECT_EQ(g.CullReason(p), RGCullReason::NoConsumer);
    EXPECT_FALSE(g.IsResourceNeeded(x));
    EXPECT_EQ(g.LivePassCount(), 0u);
    EXPECT_EQ(g.CulledPassCount(), 1u);
}

TEST(RGCull, WriterOfExternalSinkSurvives)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(color); // presented
    RGPassId p = g.AddPass({"Present"});
    g.Write(p, color, RGAccess::ColorAttachment);
    g.Compile();

    EXPECT_FALSE(g.IsCulled(p));
    EXPECT_TRUE(g.IsResourceNeeded(color));
    EXPECT_EQ(g.LivePassCount(), 1u);
}

TEST(RGCull, TransitiveChainSurvivesAndDeadBranchIsCulled)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId shadow = Tex(g, "Shadow");
    RGResourceId depth = Tex(g, "Depth");
    RGResourceId color = Tex(g, "Color");
    RGResourceId unused = Tex(g, "Unused");
    g.MarkExternal(color);

    RGPassId shadowPass = g.AddPass({"ShadowDepth"});
    g.Write(shadowPass, shadow, RGAccess::DepthWrite);

    RGPassId depthPass = g.AddPass({"DepthPrepass"});
    g.Write(depthPass, depth, RGAccess::DepthWrite);

    RGPassId worldPass = g.AddPass({"World"});
    g.Read(worldPass, shadow, RGAccess::Sampled);
    g.Read(worldPass, depth, RGAccess::DepthRead);
    g.Write(worldPass, color, RGAccess::ColorAttachment);

    RGPassId deadPass = g.AddPass({"DeadOffscreen"});
    g.Write(deadPass, unused, RGAccess::ColorAttachment); // nobody reads Unused

    g.Compile();

    EXPECT_FALSE(g.IsCulled(shadowPass));
    EXPECT_FALSE(g.IsCulled(depthPass));
    EXPECT_FALSE(g.IsCulled(worldPass));
    EXPECT_TRUE(g.IsCulled(deadPass));
    EXPECT_EQ(g.LivePassCount(), 3u);
    EXPECT_TRUE(g.IsResourceNeeded(shadow));
    EXPECT_TRUE(g.IsResourceNeeded(depth));
    EXPECT_TRUE(g.IsResourceNeeded(color));
    EXPECT_FALSE(g.IsResourceNeeded(unused));
}

TEST(RGCull, ConsumingAnOtherwiseDeadResourceRevivesItsProducer)
{
    // Same as above but World now reads Unused -> DeadOffscreen must survive.
    RGGraph g;
    g.BeginFrame();
    RGResourceId color = Tex(g, "Color");
    RGResourceId aux = Tex(g, "Aux");
    g.MarkExternal(color);

    RGPassId auxPass = g.AddPass({"AuxProducer"});
    g.Write(auxPass, aux, RGAccess::ColorAttachment);

    RGPassId worldPass = g.AddPass({"World"});
    g.Read(worldPass, aux, RGAccess::Sampled);
    g.Write(worldPass, color, RGAccess::ColorAttachment);

    g.Compile();

    EXPECT_FALSE(g.IsCulled(auxPass)) << "producer of a consumed resource must survive";
    EXPECT_FALSE(g.IsCulled(worldPass));
    EXPECT_EQ(g.LivePassCount(), 2u);
}

TEST(RGCull, PreventCullingKeepsSideEffectPass)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "QueryScratch");
    RGPassId p = g.AddPass({"TimestampQuery"});
    g.Write(p, x, RGAccess::StorageWrite); // output consumed by nobody
    g.PreventCulling(p);
    g.Compile();

    EXPECT_FALSE(g.IsCulled(p));
    EXPECT_EQ(g.LivePassCount(), 1u);
}

TEST(RGCull, LongChainFullyTransitive)
{
    RGGraph g;
    g.BeginFrame();
    constexpr int kN = 8;
    RGResourceId res[kN];
    for (int i = 0; i < kN; ++i)
        res[i] = Tex(g, "R");
    g.MarkExternal(res[kN - 1]); // only the final output is a sink

    // Pass i writes res[i] and (for i>0) reads res[i-1]: a strict chain.
    for (int i = 0; i < kN; ++i)
    {
        RGPassId p = g.AddPass({"ChainLink"});
        if (i > 0)
            g.Read(p, res[i - 1], RGAccess::Sampled);
        g.Write(p, res[i], RGAccess::ColorAttachment);
    }
    g.Compile();

    EXPECT_EQ(g.LivePassCount(), static_cast<size_t>(kN)) << "entire chain feeds the sink";
    EXPECT_EQ(g.CulledPassCount(), 0u);
}

// Review minor: pool byte estimates must cover the whole mip chain (base-only
// undercounts a full chain by ~33%, skewing budgets/eviction).
TEST(RGRealize, EstimateBytesSumsMipChain)
{
    TextureDesc d;
    d.width = 256;
    d.height = 256;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM); // 4 bytes/texel
    d.mipLevels = 9;                                              // 256 -> 1
    // sum of (2^k)^2 for k=0..8 = (4^9 - 1) / 3 = 87381 texels
    EXPECT_EQ(EstimateBytes(d), 87381ull * 4ull);
    d.mipLevels = 1;
    EXPECT_EQ(EstimateBytes(d), 256ull * 256ull * 4ull);
}

// Review minor: a culled pass whose outputs DO have readers (all themselves
// culled) reports ProducerCulled, not NoConsumer — the MCP "why didn't this
// run" answer must distinguish the two.
TEST(RGCull, CullReasonDistinguishesProducerCulledFromNoConsumer)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId y = Tex(g, "Y");
    RGPassId producer = g.AddPass({"Producer"});
    g.Write(producer, x, RGAccess::ColorAttachment);
    RGPassId consumer = g.AddPass({"Consumer"});
    g.Read(consumer, x, RGAccess::Sampled);
    g.Write(consumer, y, RGAccess::ColorAttachment); // y reaches no sink
    g.Compile();

    EXPECT_TRUE(g.IsCulled(consumer));
    EXPECT_EQ(g.CullReason(consumer), RGCullReason::NoConsumer) << "nobody reads its output";
    EXPECT_TRUE(g.IsCulled(producer));
    EXPECT_EQ(g.CullReason(producer), RGCullReason::ProducerCulled)
        << "its output HAS a reader — which was culled";
}

// Review minor: a dependency cycle must never silently truncate the schedule
// (Release included) — cyclic passes are culled with an explicit reason and the
// acyclic remainder still schedules.
TEST(RGCull, DependencyCycleIsCulledWithReasonNotSilentlyTruncated)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId y = Tex(g, "Y");
    RGResourceId z = Tex(g, "Z");
    g.MarkExternal(x);
    g.MarkExternal(y);
    g.MarkExternal(z);
    // Interleaved recording produces RAW edges A->B (on X) and B->A (on Y): a cycle.
    RGPassId a = g.AddPass({"CycleA"});
    RGPassId b = g.AddPass({"CycleB"});
    RGPassId c = g.AddPass({"Independent"});
    g.Write(a, x, RGAccess::ColorAttachment);
    g.Write(b, y, RGAccess::ColorAttachment);
    g.Read(a, y, RGAccess::Sampled);
    g.Read(b, x, RGAccess::Sampled);
    g.Write(c, z, RGAccess::ColorAttachment); // acyclic bystander
    g.Compile();
    ASSERT_EQ(g.LivePassCount(), 3u);
    g.Schedule();

    EXPECT_TRUE(g.IsCulled(a));
    EXPECT_TRUE(g.IsCulled(b));
    EXPECT_EQ(g.CullReason(a), RGCullReason::Cycle);
    EXPECT_EQ(g.CullReason(b), RGCullReason::Cycle);
    EXPECT_FALSE(g.IsCulled(c)) << "the acyclic remainder must survive";
    ASSERT_EQ(g.ScheduledOrder().size(), 1u);
    EXPECT_EQ(g.ScheduledOrder()[0], c);
    EXPECT_EQ(g.LivePassCount(), 1u);
}

// Pin: names are arena-copied at declaration — a caller reusing its name buffer
// (per-view dynamic names) must not retroactively rename the pass.
TEST(RGCull, PassNameSurvivesCallerBufferReuse)
{
    RGGraph g;
    g.BeginFrame();
    char name[32];
    std::snprintf(name, sizeof(name), "First");
    RGResourceId x = Tex(g, "X");
    g.MarkExternal(x);
    RGPassId p = g.AddPass({name});
    g.Write(p, x, RGAccess::ColorAttachment);
    std::snprintf(name, sizeof(name), "Mutated");
    g.Compile();
    EXPECT_STREQ(g.PassName(p), "First");
}

TEST(RGCull, BeginFrameResetsForReuse)
{
    RGGraph g;

    // Frame 1: one live pass.
    g.BeginFrame();
    RGResourceId c1 = Tex(g, "Color");
    g.MarkExternal(c1);
    RGPassId p1 = g.AddPass({"P1"});
    g.Write(p1, c1, RGAccess::ColorAttachment);
    g.Compile();
    EXPECT_EQ(g.PassCount(), 1u);
    EXPECT_EQ(g.LivePassCount(), 1u);

    // Frame 2: declarations cleared; a fresh, different graph.
    g.BeginFrame();
    EXPECT_EQ(g.PassCount(), 0u);
    EXPECT_EQ(g.ResourceCount(), 0u);
    RGResourceId x = Tex(g, "X");
    RGPassId dead = g.AddPass({"Dead"});
    g.Write(dead, x, RGAccess::ColorAttachment); // not external -> culled
    g.Compile();
    EXPECT_EQ(g.PassCount(), 1u);
    EXPECT_EQ(g.LivePassCount(), 0u);
    EXPECT_TRUE(g.IsCulled(dead));
}
