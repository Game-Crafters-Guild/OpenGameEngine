// M1 steady-state allocation test: after two warm-up frames, building +
// compiling an identical frame must perform only a small bounded number of heap
// allocations (member scratch retains capacity; the arena retains its blocks).
// This pins the immediate-mode design's core perf claim — and fails loudly if
// someone reintroduces per-frame vector churn.
//
// Counted by the allocation counter in a process-wide window around the
// steady-state frame.

#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <gtest/gtest.h>

#include "Memory/AllocationCountScope.h"

#include <cstdio>

using namespace GameEngine::Rendering::RenderGraph;

namespace
{
// A representative editor-shaped frame: depth prepass, 4 CSM cascade layers,
// compute cull, world, a 3-pass post chain, an imported history, and an
// independent preview chain (second component).
void BuildRepresentativeFrame(RGGraph& g)
{
    g.BeginFrame();

    RGResourceDesc texDesc;
    texDesc.Kind = RGResourceKind::Texture;
    auto tex = [&](const char* name, uint32_t mips = 1, uint32_t layers = 1)
    {
        RGResourceDesc d = texDesc;
        d.Name = name;
        d.MipLevels = mips;
        d.ArrayLayers = layers;
        return g.CreateResource(d);
    };
    auto buf = [&](const char* name)
    {
        RGResourceDesc d;
        d.Kind = RGResourceKind::Buffer;
        d.SizeBytes = 4096;
        d.Name = name;
        return g.CreateResource(d);
    };

    RGResourceId depth = tex("Depth");
    RGResourceId shadow = tex("ShadowArray", 1, 4);
    RGResourceId lights = buf("LightLists");
    RGResourceId color = tex("SceneColor");
    RGResourceId bloomA = tex("BloomA");
    RGResourceId bloomB = tex("BloomB");
    RGResourceId ldr = tex("LDR");
    // History is a rotating PAIR (read frame N-1, write frame N) — sampling
    // the texture a pass also attaches is the feedback-loop shape the barrier
    // combiner asserts against.
    RGResourceId histPrev = tex("HistoryPrev");
    RGResourceId hist = tex("History");
    RGResourceId pvColor = tex("PreviewColor");
    RGResourceId pvOut = tex("PreviewOut");
    g.MarkImported(histPrev, RGImageLayout::ShaderReadOnly);
    g.MarkImported(hist, RGImageLayout::ShaderReadOnly);
    g.MarkExternal(ldr);
    g.MarkExternal(pvOut);

    RGPassId pre = g.AddPass({"DepthPrepass"});
    g.Write(pre, depth, RGAccess::DepthWrite);
    for (uint32_t i = 0; i < 4; ++i)
    {
        RGPassId cascade = g.AddPass({"Cascade"});
        g.Write(cascade, shadow, RGAccess::DepthWrite, RGRange{0, kRemaining, i, 1});
    }
    RGPassId cull = g.AddPass({"ClusterCull", 0, RGQueue::Compute});
    g.Read(cull, depth, RGAccess::Sampled);
    g.Write(cull, lights, RGAccess::StorageWrite);
    RGPassId world = g.AddPass({"World"});
    g.Read(world, depth, RGAccess::DepthRead);
    g.Read(world, shadow, RGAccess::Sampled);
    g.Read(world, lights, RGAccess::StorageRead);
    g.Read(world, histPrev, RGAccess::Sampled);
    g.Write(world, color, RGAccess::ColorAttachment);
    g.Write(world, hist, RGAccess::ColorAttachment); // history update (rotated pair)
    RGPassId blurH = g.AddPass({"BloomH"});
    g.Read(blurH, color, RGAccess::Sampled);
    g.Write(blurH, bloomA, RGAccess::ColorAttachment);
    RGPassId blurV = g.AddPass({"BloomV"});
    g.Read(blurV, bloomA, RGAccess::Sampled);
    g.Write(blurV, bloomB, RGAccess::ColorAttachment);
    RGPassId tonemap = g.AddPass({"Tonemap"});
    g.Read(tonemap, color, RGAccess::Sampled);
    g.Read(tonemap, bloomB, RGAccess::Sampled);
    g.Write(tonemap, ldr, RGAccess::ColorAttachment);
    RGPassId pvWorld = g.AddPass({"PreviewWorld"});
    g.Write(pvWorld, pvColor, RGAccess::ColorAttachment);
    RGPassId pvEncode = g.AddPass({"PreviewEncode"});
    g.Read(pvEncode, pvColor, RGAccess::Sampled);
    g.Write(pvEncode, pvOut, RGAccess::ColorAttachment);

    g.Compile();
    g.Schedule();
    g.GenerateBarriers(RGGraph::kIdentityQueueMap);
    g.BuildSubmissionPlan();
}
} // namespace

TEST(RGAlloc, SteadyStateFrameHeapAllocationsAreBounded)
{
    RGGraph g;
    BuildRepresentativeFrame(g); // frame 0: capacities grow
    BuildRepresentativeFrame(g); // frame 1: settle

    uint64_t allocs = 0;
    {
        const GameEngine::Memory::AllocationCountScope frame(GameEngine::Memory::CountWindow::Process);
        BuildRepresentativeFrame(g); // frame 2: steady state
        allocs = frame.Count();
    }
    std::printf("[RenderGraph] steady-state frame heap allocations: %llu\n",
                static_cast<unsigned long long>(allocs));
    RecordProperty("steadyStateAllocs", static_cast<int>(allocs));
    // ZERO. After the flat-CSR conversion (barrier combos/needy-masks/mip-runs,
    // cluster keys/in-degrees, bounded submission Waits) every byte of frame
    // scratch is a capacity-retained member or arena block — a steady-state
    // frame performs no heap allocation at all (pre-hygiene baseline: ~376).
    // If this fails, something reintroduced per-frame churn (the old design's
    // failure mode); fix it with member scratch, don't raise the bound.
    EXPECT_EQ(allocs, 0u) << "per-frame heap churn crept back into the RenderGraph hot path";
}
