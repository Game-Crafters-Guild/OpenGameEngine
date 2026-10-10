// Stage 2b isolation tests for subresource-range barrier generation: per-mip
// state tracking (the Hi-Z pattern: read mip N-1 / write mip N of ONE texture),
// per-layer tracking + rectangle merging (the CSM cascade pattern), and
// whole-range reads over mixed subresource states. Pure logic, no GPU.

#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <gtest/gtest.h>

using namespace GameEngine::Rendering::RenderGraph;

namespace
{
RGResourceId Tex(RGGraph& g, const char* name, uint32_t mips = 1, uint32_t layers = 1)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Texture;
    d.MipLevels = mips;
    d.ArrayLayers = layers;
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
RGRange Mip(uint32_t m)
{
    return RGRange{m, 1, 0, kRemaining};
}
RGRange Layer(uint32_t l)
{
    return RGRange{0, kRemaining, l, 1};
}
void Bake(RGGraph& g)
{
    g.Compile();
    g.Schedule();
    g.GenerateBarriers(RGGraph::kIdentityQueueMap);
}
// Barriers belonging to pass p's batch.
std::vector<RGBarrier> BatchOf(const RGGraph& g, RGPassId p)
{
    std::vector<RGBarrier> out;
    for (const RGBarrierBatch& b : g.BarrierBatches())
        if (b.Pass == p)
            for (uint32_t i = 0; i < b.Count; ++i)
                out.push_back(g.Barriers()[b.First + i]);
    return out;
}
} // namespace

TEST(RGSubresource, HiZMipChainTracksPerMipNoSelfHazard)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId hiz = Tex(g, "HiZ", /*mips=*/4);
    g.MarkExternal(hiz);

    RGPassId p0 = g.AddPass({"WriteMip0"});
    g.Write(p0, hiz, RGAccess::StorageWrite, Mip(0));
    RGPassId chain[4] = {p0, kInvalidId, kInvalidId, kInvalidId};
    for (uint32_t i = 1; i < 4; ++i)
    {
        chain[i] = g.AddPass({"Downsample"});
        g.Read(chain[i], hiz, RGAccess::StorageRead, Mip(i - 1)); // read prev mip
        g.Write(chain[i], hiz, RGAccess::StorageWrite, Mip(i));   // write own mip — same texture!
    }
    Bake(g);

    ASSERT_EQ(g.LivePassCount(), 4u);

    // p0: only mip0's first-use init transition (Undefined -> General).
    auto b0 = BatchOf(g, p0);
    ASSERT_EQ(b0.size(), 1u);
    EXPECT_EQ(b0[0].Range.BaseMip, 0u);
    EXPECT_EQ(b0[0].Range.MipCount, 1u);
    EXPECT_EQ(b0[0].OldLayout, RGImageLayout::Undefined);
    EXPECT_EQ(b0[0].NewLayout, RGImageLayout::General);

    // Each downsample step: RAW on the previous mip + first-use init on its own
    // mip — exactly two single-mip barriers; no whole-resource barrier, ever.
    for (uint32_t i = 1; i < 4; ++i)
    {
        auto bi = BatchOf(g, chain[i]);
        ASSERT_EQ(bi.size(), 2u) << "step " << i;
        const RGBarrier* raw = nullptr;
        const RGBarrier* init = nullptr;
        for (const RGBarrier& b : bi)
        {
            if (b.Range.BaseMip == i - 1)
                raw = &b;
            if (b.Range.BaseMip == i)
                init = &b;
        }
        ASSERT_NE(raw, nullptr) << "step " << i;
        ASSERT_NE(init, nullptr) << "step " << i;
        EXPECT_EQ(raw->Range.MipCount, 1u);
        EXPECT_TRUE((raw->SrcAccess & RGAccessMask::ShaderWrite) != 0) << "RAW src is the prior write";
        EXPECT_TRUE((raw->DstAccess & RGAccessMask::ShaderRead) != 0);
        EXPECT_EQ(init->Range.MipCount, 1u);
        EXPECT_EQ(init->OldLayout, RGImageLayout::Undefined) << "own mip is first-use";
    }
    for (const RGBarrier& b : g.Barriers())
        EXPECT_LT(b.Range.MipCount, 4u) << "no whole-resource transition in a per-mip chain";
}

TEST(RGSubresource, CascadeLayersAreIndependentAndMergeOnSample)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId shadow = Tex(g, "ShadowArray", /*mips=*/1, /*layers=*/4);
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(color);

    RGPassId cascade[4];
    for (uint32_t i = 0; i < 4; ++i)
    {
        cascade[i] = g.AddPass({"Cascade"});
        g.Write(cascade[i], shadow, RGAccess::DepthWrite, Layer(i));
    }
    RGPassId world = g.AddPass({"World"});
    g.Read(world, shadow, RGAccess::Sampled); // whole array
    g.Write(world, color, RGAccess::ColorAttachment);
    Bake(g);

    // Each cascade transitions ONLY its own layer, from Undefined (no WAW
    // against sibling layers — distinct cells don't hazard).
    for (uint32_t i = 0; i < 4; ++i)
    {
        auto bi = BatchOf(g, cascade[i]);
        ASSERT_EQ(bi.size(), 1u) << "cascade " << i;
        EXPECT_EQ(bi[0].Range.BaseLayer, i);
        EXPECT_EQ(bi[0].Range.LayerCount, 1u);
        EXPECT_EQ(bi[0].OldLayout, RGImageLayout::Undefined) << "init, not a cross-layer WAW";
        EXPECT_EQ(bi[0].NewLayout, RGImageLayout::DepthAttachment);
    }

    // World samples all four layers: identical per-layer states merge into ONE
    // ranged barrier (layers 0..4) + the color attachment's own init barrier.
    auto bw = BatchOf(g, world);
    ASSERT_EQ(bw.size(), 2u);
    const RGBarrier* shadowBar = nullptr;
    for (const RGBarrier& b : bw)
        if (b.Resource == shadow)
            shadowBar = &b;
    ASSERT_NE(shadowBar, nullptr);
    EXPECT_EQ(shadowBar->Range.BaseLayer, 0u);
    EXPECT_EQ(shadowBar->Range.LayerCount, 4u) << "identical layer states must merge to one barrier";
    EXPECT_EQ(shadowBar->OldLayout, RGImageLayout::DepthAttachment);
    EXPECT_EQ(shadowBar->NewLayout, RGImageLayout::ShaderReadOnly);
}

TEST(RGSubresource, WholeRangeReadOverMixedStatesEmitsPerStateGroups)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId t = Tex(g, "T", /*mips=*/2);
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(out);

    RGPassId w0 = g.AddPass({"WriteMip0"});
    g.Write(w0, t, RGAccess::StorageWrite, Mip(0)); // mip0 -> General; mip1 untouched
    RGPassId rd = g.AddPass({"SampleAll"});
    g.Read(rd, t, RGAccess::Sampled); // whole texture
    g.Write(rd, out, RGAccess::StorageWrite);
    Bake(g);

    // mip0: General -> ShaderReadOnly (RAW); mip1: Undefined -> ShaderReadOnly
    // (first use); + the pass's own output buffer first-write exec dep.
    auto br = BatchOf(g, rd);
    ASSERT_EQ(br.size(), 3u);
    const RGBarrier* m0 = nullptr;
    const RGBarrier* m1 = nullptr;
    for (const RGBarrier& b : br)
    {
        if (b.Resource == t && b.Range.BaseMip == 0)
            m0 = &b;
        if (b.Resource == t && b.Range.BaseMip == 1)
            m1 = &b;
    }
    ASSERT_NE(m0, nullptr);
    ASSERT_NE(m1, nullptr);
    EXPECT_EQ(m0->OldLayout, RGImageLayout::General);
    EXPECT_EQ(m1->OldLayout, RGImageLayout::Undefined);
    EXPECT_EQ(m0->NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(m1->NewLayout, RGImageLayout::ShaderReadOnly);
}

TEST(RGSubresource, WholeResourceAccessesStillProduceSingleBarriers)
{
    // Default-range (whole resource) usage keeps the old behavior: one barrier
    // covering all subresources, with resolved counts.
    RGGraph g;
    g.BeginFrame();
    RGResourceId t = Tex(g, "T", /*mips=*/3, /*layers=*/2);
    RGResourceId out = Tex(g, "Out");
    g.MarkExternal(out);

    RGPassId w = g.AddPass({"WriteAll"});
    g.Write(w, t, RGAccess::StorageWrite);
    RGPassId rd = g.AddPass({"SampleAll"});
    g.Read(rd, t, RGAccess::Sampled);
    g.Write(rd, out, RGAccess::ColorAttachment);
    Bake(g);

    auto bw = BatchOf(g, w);
    ASSERT_EQ(bw.size(), 1u);
    EXPECT_EQ(bw[0].Range.MipCount, 3u);
    EXPECT_EQ(bw[0].Range.LayerCount, 2u);

    auto br = BatchOf(g, rd);
    ASSERT_EQ(br.size(), 2u); // T transition + Out init
    for (const RGBarrier& b : br)
    {
        if (b.Resource == t)
        {
            EXPECT_EQ(b.Range.MipCount, 3u);
            EXPECT_EQ(b.Range.LayerCount, 2u);
        }
    }
}
