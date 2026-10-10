// Slice 2.7: the GPU-driven core on RGFrame, over the REAL headless device
// with REAL shaders (rendering_test_shader_paths). T1 pins the exact failure
// the slice-2 review caught as critical: the RenderGraph arm silently dropping every
// culling submission. Numbering follows the rg2 slice-2 plan §5 (retired 2026-07-29, in git history); T7 (AllocUpload
// offsets) is covered by RGUploadRingTests semantics and not repeated here.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/HzbCullingStrategy.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "RGPassQuery.h"
#include "Tests/RenderGraph/RGTestDevice.h"
#include "Tests/ScopedEnvVar.h"
#include "Tests/TestUtils.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;
using GameEngine::Rendering::RenderGraph::Test::MakeHeadlessDevice;
namespace RGQuery = GameEngine::Testing::RGQuery;

namespace
{
std::vector<uint8_t> TestShaderLoader(const char* name)
{
    using GameEngine::Rendering::Utils::ReadFile;
    if (!name || !name[0])
        return {};
    // Requested names arrive pre-prefixed "Shaders/..."; compiled .shaderpkg
    // artifacts live under CMAKE_BINARY_DIR (the shader output dir's parent).
#ifdef RENDERING_SHADER_OUTPUT_DIR
    const auto built = std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / name;
    std::error_code ec;
    if (std::filesystem::exists(built, ec))
        return ReadFile(built.string());
#endif
    return ReadFile((GameEngine::Rendering::Tests::GetRenderingShadersDir() / name).string());
}

// The first package that cannot be loaded, or nullptr when all are available.
// Uses the same load the production code performs, so the answer matches what
// the pipeline itself will conclude.
const char* FirstUnloadablePkg(std::initializer_list<const char*> pkgs)
{
    std::string err;
    for (const char* pkg : pkgs)
        if (LoadComputeStageBytes(pkg, ShaderSourceKind::SpirV, &TestShaderLoader, &err).empty())
            return pkg;
    return nullptr;
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

ViewCullingInput MakeView(uint32_t viewId, uint32_t instanceCount)
{
    ViewCullingInput v{};
    v.viewId = viewId;
    v.cascadeIndex = kCullingCascadeIndexNone;
    v.instanceCount = instanceCount;
    v.firstInstance = 0;
    v.nearPlane = 0.1f;
    v.farPlane = 100.0f;
    return v;
}

// First SCHEDULED index of the query, or -1. Query is Exact/Subtree/Family —
// never a substring: test-authored names here ("Skinning", "World") are full
// names, and production ones ("GPUCulling.View9.C252") collide by extension.
template <class Query>
int PosOf(const RGGraph& g, const Query& q)
{
    const auto idx = RGQuery::ScheduledIndex(g, q);
    return idx.has_value() ? static_cast<int>(*idx) : -1;
}
} // namespace

#define RG_REQUIRE_DEVICE(dev)                                                                        \
    auto dev = MakeHeadlessDevice();                                                                   \
    if (!dev)                                                                                          \
    GTEST_SKIP() << "no headless device available"

// T1 — THE critical pin: BeginFrame(RGFrame*) + SubmitView/SubmitCascadeGroup
// + EndFrame must actually schedule culling passes and publish a valid
// frame-local visibility value (the guards used to drop every submission).
TEST(RGGpuDriven, CullingSubmissionsScheduleUnderTheRGArm)
{
    RG_REQUIRE_DEVICE(dev);
    // Skip rather than fail when the package is unavailable: without it the
    // pipeline declares no culling pass, so the schedule assertions below would
    // report a staging gap as a broken scheduling contract.
    if (const char* missing = FirstUnloadablePkg({"Shaders/frustum_culling.shaderpkg"}))
        GTEST_SKIP() << missing << " unavailable";
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    ASSERT_NE(scene, nullptr);
    auto pipeline = GPUCullingFactory::CreateBalanced(dev.get());
    ASSERT_NE(pipeline, nullptr);

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    pipeline->BeginFrame(&frame, scene.get());
    pipeline->SubmitView(MakeView(/*viewId=*/7, /*instances=*/8));
    CascadeCullingGroup group{};
    group.viewId = 7;
    group.cascadeCount = 2;
    group.instanceCount = 8;
    pipeline->SubmitCascadeGroup(group);
    pipeline->EndFrame();

    ASSERT_TRUE(pipeline->GetVisibilityRG().IsValid())
        << "submissions were dropped — the RenderGraph arm is a no-op again";

    // Anchor a consumer so the chain survives cull, then execute for real.
    // EARLIER phase than the culling passes (kEarlySetup=0): at equal phase
    // insertion order alone would produce the expected order and the
    // culling-before-consumer assert below would be vacuous — only the RAW
    // edge can hold the consumer back.
    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    RGBuffer out = frame.CreateBuffer("Out", bd);
    frame.AddComputePass("Consumer", -100,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(pipeline->GetVisibilityRG());
                             p.Write(out);
                         },
                         [](RGContext&) {});
    frame.MarkOutput(out);
    frame.Execute();
    dev->WaitForIdle();

    const int cullPos = PosOf(frame.Graph(), RGQuery::Subtree{"GPUCulling.View7"});
    const int consumerPos = PosOf(frame.Graph(), RGQuery::Exact{"Consumer"});
    EXPECT_GE(cullPos, 0) << "per-view culling pass missing from the schedule";
    ASSERT_GE(consumerPos, 0);
    EXPECT_LT(cullPos, consumerPos) << "culling must precede its consumer (RAW edge)";
    // Per-view ranges published exactly like the old arm (1 view + 2 cascades).
    EXPECT_EQ(pipeline->GetViewVisibilityRanges().size(), 3u);
}

// T2 — the skinning-atlas coupling: two writers + one reader through THREE
// separate imports of one physical must collapse to one resource id, order
// writer→writer→reader, and give the reader a real RAW barrier.
TEST(RGGpuDriven, AtlasImportsCollapseAndOrderWriters)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    BufferDesc atlasDesc;
    atlasDesc.size = 4096;
    atlasDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle atlasPhys = dev->CreateBuffer(atlasDesc);
    ASSERT_TRUE(atlasPhys.IsValid());

    frame.BeginFrame(0);
    RGBuffer a1 = frame.ImportExternalBuffer("SkinPaletteAtlas", atlasPhys);
    RGBuffer a2 = frame.ImportExternalBuffer("SkinPaletteAtlas.Retarget", atlasPhys);
    RGBuffer a3 = frame.ImportExternalBuffer("SkinPaletteAtlas.WorldRead", atlasPhys);
    EXPECT_EQ(a1.Id, a2.Id);
    EXPECT_EQ(a1.Id, a3.Id);

    frame.AddPass("Skinning", 0, [&](RGPassBuilder& p) { p.Write(a1); }, [](RGContext&) {});
    frame.AddPass("Retarget", 0, [&](RGPassBuilder& p) { p.Write(a2); }, [](RGContext&) {});
    RGTexture color = frame.CreateTexture("C", [] {
        TextureDesc d;
        d.width = 8; d.height = 8;
        d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        return d;
    }());
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(a3); // the slice-3 contract read
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    const int skin = PosOf(frame.Graph(), RGQuery::Exact{"Skinning"});
    const int retarget = PosOf(frame.Graph(), RGQuery::Exact{"Retarget"});
    const int world = PosOf(frame.Graph(), RGQuery::Exact{"World"});
    ASSERT_GE(skin, 0);
    ASSERT_GE(retarget, 0);
    ASSERT_GE(world, 0);
    EXPECT_LT(skin, retarget) << "writer order = recording order (WAW)";
    EXPECT_LT(retarget, world);

    bool worldRaw = false;
    bool vsInScope = false;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.Resource == a1.Id && (b.SrcAccess & RGAccessMask::ShaderWrite) != 0 &&
            (b.DstAccess & RGAccessMask::ShaderRead) != 0)
        {
            worldRaw = true;
            if ((b.DstStage & RGStage::VertexShader) != 0)
                vsInScope = true;
        }
    EXPECT_TRUE(worldRaw) << "the world read must chain from the atlas writers (one hazard state)";
    EXPECT_TRUE(vsInScope)
        << "the atlas consumer is the VERTEX shader — FS-only visibility is the slice-2 C gap";

    dev->DestroyBuffer(atlasPhys);
}

// T4 (shape) — the stream-ready barrier the cutover must never lose: covers
// transfer zero-fill AND compute writes, visible to indirect + vertex reads.
TEST(RGGpuDriven, StreamReadyBarrierShapeSurvives)
{
    const ResourceBarrier b = GPUDrawStreamBuilder::CreateStreamReadyBarrier();
    EXPECT_TRUE(b.srcStageMask & static_cast<uint64_t>(PipelineStageMask::ComputeShader));
    EXPECT_TRUE(b.srcStageMask & static_cast<uint64_t>(PipelineStageMask::Transfer));
    EXPECT_TRUE(b.srcAccessMask & static_cast<uint64_t>(ResourceAccessMask::ShaderWrite));
    EXPECT_TRUE(b.srcAccessMask & static_cast<uint64_t>(ResourceAccessMask::TransferWrite));
    EXPECT_TRUE(b.dstStageMask & static_cast<uint64_t>(PipelineStageMask::DrawIndirect));
    EXPECT_TRUE(b.dstAccessMask & static_cast<uint64_t>(ResourceAccessMask::IndirectCommandRead));
}

// T5 — the RuntimeVisible WAR pin: skinning's Read recorded BEFORE the
// aggregate's Write keeps skinning first; the reversed recording flips it.
TEST(RGGpuDriven, RuntimeVisibleWarOrderFollowsRecording)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle rv = dev->CreateBuffer(bd);
    const BufferHandle atlas = dev->CreateBuffer(bd);

    frame.BeginFrame(0);
    RGBuffer rvRG = frame.ImportExternalBuffer("RuntimeVisible", rv);
    RGBuffer atlasRG = frame.ImportExternalBuffer("Atlas", atlas);
    frame.AddPass("SkinningLike", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(rvRG); // recorded FIRST — WAR
                      p.Write(atlasRG);
                  },
                  [](RGContext&) {});
    // EARLIER phase on the writer: at equal phase, insertion order alone
    // would already emit skin first and the assert below would be vacuous.
    // With phase pulling the aggregate forward, only the WAR edge can keep
    // it behind the reader — the assert certifies the edge, not the tiebreak.
    frame.AddPass("AggregateLike", -100, [&](RGPassBuilder& p) { p.Write(rvRG); },
                  [](RGContext&) {});
    frame.Execute();
    dev->WaitForIdle();

    const int skin = PosOf(frame.Graph(), RGQuery::Exact{"SkinningLike"});
    const int agg = PosOf(frame.Graph(), RGQuery::Exact{"AggregateLike"});
    ASSERT_GE(skin, 0);
    ASSERT_GE(agg, 0);
    EXPECT_LT(skin, agg)
        << "read-before-write recording = WAR; the write must not hoist past it (even with "
           "an earlier phase — the edge is hard, phase is a tiebreak)";

    dev->DestroyBuffer(rv);
    dev->DestroyBuffer(atlas);
}

// T6 — the D5 decision gate: ONE visibility buffer across frames is only safe
// if the re-import's first touch carries a cross-frame execution dependency.
TEST(RGGpuDriven, SingleVisibilityBufferGetsCrossFrameFirstTouchDependency)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto pipeline = GPUCullingFactory::CreateBalanced(dev.get());
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    RGResourceId visId = kInvalidId;
    for (uint64_t n = 0; n < 2; ++n)
    {
        frame.BeginFrame(n);
        pipeline->BeginFrame(&frame, scene.get());
        pipeline->SubmitView(MakeView(1, 8));
        pipeline->EndFrame();
        visId = pipeline->GetVisibilityRG().Id;
        BufferDesc bd;
        bd.size = 256;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        RGBuffer out = frame.CreateBuffer("Out", bd);
        frame.AddComputePass("Consumer", 0,
                             [&](RGPassBuilder& p)
                             {
                                 p.Read(pipeline->GetVisibilityRG());
                                 p.Write(out);
                             },
                             [](RGContext&) {});
        frame.MarkOutput(out);
        frame.Execute();
        dev->WaitForIdle();
    }

    // Frame 1's first touch of the re-imported buffer must carry the
    // BottomOfPipe-sourced execution dependency (PendingImportSync) — without
    // it, frame N+1's culling write races frame N's in-flight reads and D5
    // (single buffer) would have to revert to per-slot buffers.
    bool sawImportSync = false;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.Resource == visId && (b.SrcStage & RGStage::BottomOfPipe) != 0)
            sawImportSync = true;
    EXPECT_TRUE(sawImportSync) << "D5 single-visibility-buffer is NOT safe — revert to per-slot";
}

// E3 — the R2.1 deferred-dispatch seam: reserveOcclusionSlice publishes a
// phase-B range WITHOUT a dispatch; ScheduleOcclusionCullPass dispatches
// into it mid-declaration and lands AFTER a phase-A visibility reader (the
// WAR edge the whole cull→raster→cull seam hangs on); repeat calls no-op.
TEST(RGGpuDriven, OcclusionSliceReservationDefersDispatchBehindVisibilityWAR)
{
    RG_REQUIRE_DEVICE(dev);
    if (const char* missing = FirstUnloadablePkg({"Shaders/frustum_culling.shaderpkg",
                                                  "Shaders/hzb_culling.shaderpkg"}))
        GTEST_SKIP() << missing << " unavailable";
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUCullingPipeline::SetHzbCullingShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto pipeline = GPUCullingFactory::CreateBalanced(dev.get());
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    // A stand-in HZB pyramid for the phase-B (mode 2) dispatch to sample.
    TextureDesc htd{};
    htd.width = htd.height = htd.depth = 1u;
    htd.mipLevels = htd.arrayLayers = htd.sampleCount = 1u;
    htd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    htd.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    htd.debugName = "Test.HZB";
    TextureHandle hzbTex = dev->CreateTexture(htd);
    const RGTexture hzb =
        frame.ImportExternalTexture("Test.HZB", hzbTex, ResourceState::Common,
                                    TextureFormat::R32_FLOAT, 1u, 1u);

    pipeline->BeginFrame(&frame, scene.get());
    ViewCullingInput v = MakeView(/*viewId=*/9, /*instances=*/8);
    v.reserveOcclusionSlice = true;
    pipeline->SubmitView(v);
    pipeline->EndFrame();
    ASSERT_TRUE(pipeline->GetVisibilityRG().IsValid());

    // Ranges: phase-A slice + the reserved phase-B slice, disjoint offsets.
    const auto& ranges = pipeline->GetViewVisibilityRanges();
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].slicePhase, 0u);
    EXPECT_EQ(ranges[1].slicePhase, 1u);
    EXPECT_EQ(ranges[1].viewId, 9u);
    EXPECT_GE(ranges[1].visibilityOffset, ranges[0].visibilityOffset + ranges[0].visibilityCount)
        << "the reserved slice must not alias the phase-A slice";

    // A phase-A consumer of visibility (stands in for the scatter+raster).
    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    RGBuffer out = frame.CreateBuffer("Out", bd);
    frame.AddComputePass("PhaseAReader", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(pipeline->GetVisibilityRG());
                             p.Write(out);
                         },
                         [](RGContext&) {});

    // The deferred phase-B (mode 2) dispatch — recorded AFTER the reader
    // declaration, and its visibility WRITE must not hoist past the reader's
    // READ (WAR).
    pipeline->ScheduleOcclusionCullPass(frame, scene.get(), 9, hzb, /*hzbMipCount=*/1u);
    // Repeat call: the reservation is consumed — no second dispatch.
    pipeline->ScheduleOcclusionCullPass(frame, scene.get(), 9, hzb, /*hzbMipCount=*/1u);

    frame.MarkOutput(out);
    frame.Execute();
    dev->WaitForIdle();

    // Phase A is now the mode-1 HZB dispatch (frustum ∧ prevVisible), named with
    // the phase-A code C252; phase B is the mode-2 dispatch, C253.
    const int phaseA = PosOf(frame.Graph(), RGQuery::Exact{"GPUCulling.View9.C252"});
    const int reader = PosOf(frame.Graph(), RGQuery::Exact{"PhaseAReader"});
    const int phaseB = PosOf(frame.Graph(), RGQuery::Exact{"GPUCulling.View9.C253"});
    ASSERT_GE(phaseA, 0) << "phase-A (mode 1) dispatch missing";
    ASSERT_GE(reader, 0);
    ASSERT_GE(phaseB, 0) << "deferred phase-B dispatch missing";
    EXPECT_LT(phaseA, reader);
    EXPECT_LT(reader, phaseB)
        << "the phase-B write must stay behind the phase-A visibility reader (WAR)";

    EXPECT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Exact{"GPUCulling.View9.C253"}), 1u)
        << "repeat ScheduleOcclusionCullPass must no-op (consumed guard)";

    dev->DestroyTexture(hzbTex);
}

// R3 — the skinning-gate aggregate ordering under two-phase HZB (PR #258).
// Replicates the owner-frame declaration order exactly: skinning reads
// RuntimeVisible FIRST (the deliberate 1-frame WAR lag), then phase A (mode-1
// C252) writes the phase-A slice, a scatter/raster stand-in reads visibility,
// the deferred phase B (mode-2 C253) rewrites the reserved slice + prevVisible,
// and finally the union+aggregate epilogue runs. Pins that the visibility union
// is scheduled AFTER the phase-B rewrite so its RAW read of the shared
// visibility buffer observes the POST-B recovered slice (frustum ∧ HZB), not the
// stale phase-A (frustum ∧ prevVisible) generation. Because the union sits at
// kEarlySetup and phase B at kWorldRender, ONLY the recording-order RAW edge
// (phase-B write declared before the union read) can hold the union behind
// phase B — a phase tiebreak alone would hoist the union in front of it. If the
// union hoisted, the skinning gate would consume an aggregate that drops a
// just-revealed character for an extra frame, snapping its frozen palette.
TEST(RGGpuDriven, VisibilityUnionReadsPostPhaseBSliceForHzbView)
{
    RG_REQUIRE_DEVICE(dev);
    if (const char* missing =
            FirstUnloadablePkg({"Shaders/frustum_culling.shaderpkg",
                                "Shaders/hzb_culling.shaderpkg",
                                "Shaders/visibility_union.shaderpkg",
                                "Shaders/runtime_visibility_aggregate.shaderpkg"}))
        GTEST_SKIP() << missing << " unavailable";
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUCullingPipeline::SetHzbCullingShaderLoader(&TestShaderLoader);
    GPUCullingPipeline::SetVisibilityUnionShaderLoader(&TestShaderLoader);
    GPUCullingPipeline::SetRuntimeVisibilityShaderLoader(&TestShaderLoader);

    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    // A handful of skinned instances so GetInstanceCount() > 0 (the union and
    // aggregate both bail on an empty scene) and every published range spans the
    // full instance set (the union's `visibilityCount >= instanceCount` filter).
    constexpr uint32_t kInstances = 8;
    for (uint32_t i = 0; i < kInstances; ++i)
    {
        GPUInstance inst{};
        inst.runtimeId = i + 1u; // non-zero → a gated runtime
        scene->AddInstance(inst);
    }
    ASSERT_EQ(scene->GetInstanceCount(), kInstances);

    auto pipeline = GPUCullingFactory::CreateBalanced(dev.get());
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    // Stand-in HZB pyramid for the phase-B (mode 2) dispatch to sample.
    TextureDesc htd{};
    htd.width = htd.height = htd.depth = 1u;
    htd.mipLevels = htd.arrayLayers = htd.sampleCount = 1u;
    htd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    htd.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    htd.debugName = "Test.HZB";
    TextureHandle hzbTex = dev->CreateTexture(htd);
    const RGTexture hzb =
        frame.ImportExternalTexture("Test.HZB", hzbTex, ResourceState::Common,
                                    TextureFormat::R32_FLOAT, 1u, 1u);

    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);

    // 1. Skinning-like consumer: reads RuntimeVisible FIRST (recorded before any
    //    culling pass — the WAR pin that lags the aggregate a frame).
    const RGBuffer rv = pipeline->ImportRuntimeVisible(frame);
    ASSERT_TRUE(rv.IsValid());
    RGBuffer skinOut = frame.CreateBuffer("SkinOut", bd);
    frame.AddComputePass("SkinningLike", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(rv);
                             p.Write(skinOut);
                         },
                         [](RGContext&) {});

    // 2. Phase A + phase-B reservation (mode-1 C252 writes the phase-A slice).
    pipeline->BeginFrame(&frame, scene.get());
    ViewCullingInput v = MakeView(/*viewId=*/9, kInstances);
    v.reserveOcclusionSlice = true;
    pipeline->SubmitView(v);
    pipeline->EndFrame();
    ASSERT_TRUE(pipeline->GetVisibilityRG().IsValid());

    // 3. Phase-A scatter/raster stand-in: reads the visibility buffer (the read
    //    the deferred phase-B write must land behind — WAR).
    RGBuffer scatterOut = frame.CreateBuffer("ScatterOut", bd);
    frame.AddComputePass("PhaseAReader", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(pipeline->GetVisibilityRG());
                             p.Write(scatterOut);
                         },
                         [](RGContext&) {});

    // 4. Deferred phase B (mode-2 C253): rewrites the reserved slice + prevVisible.
    pipeline->ScheduleOcclusionCullPass(frame, scene.get(), 9, hzb, /*hzbMipCount=*/1u);

    // 5-6. Union then aggregate (owner-frame epilogue — the real spine tail).
    pipeline->ScheduleVisibilityUnion(frame, scene.get());
    pipeline->ScheduleRuntimeVisibilityAggregate(frame, scene.get());

    frame.MarkOutput(skinOut);
    frame.MarkOutput(scatterOut);
    frame.Execute();
    dev->WaitForIdle();

    const int skinning = PosOf(frame.Graph(), RGQuery::Exact{"SkinningLike"});
    const int reader = PosOf(frame.Graph(), RGQuery::Exact{"PhaseAReader"});
    const int phaseB = PosOf(frame.Graph(), RGQuery::Exact{"GPUCulling.View9.C253"});
    const int unionPos = PosOf(frame.Graph(), RGQuery::Exact{"GPUCulling.VisibilityUnion"});
    const int aggregate = PosOf(frame.Graph(), RGQuery::Exact{"GPUCulling.RuntimeVisibilityAggregate"});

    ASSERT_GE(phaseB, 0) << "phase-B (mode 2) dispatch missing";
    ASSERT_GE(unionPos, 0) << "visibility union pass missing";
    ASSERT_GE(aggregate, 0) << "runtime-visibility aggregate pass missing";
    ASSERT_GE(skinning, 0);
    ASSERT_GE(reader, 0);

    EXPECT_LT(reader, phaseB) << "phase-B write must stay behind the phase-A visibility reader (WAR)";
    EXPECT_LT(phaseB, unionPos)
        << "the union must read the shared visibility buffer AFTER the phase-B rewrite (RAW) so the "
           "aggregate reflects the post-B (frustum ∧ HZB) set, not the phase-A (frustum ∧ prevVisible) "
           "generation";
    EXPECT_LT(unionPos, aggregate) << "the aggregate consumes the union's AnyViewVisible output (RAW)";
    EXPECT_LT(skinning, aggregate)
        << "skinning reads RuntimeVisible before the aggregate rewrites it (deliberate 1-frame WAR lag)";

    dev->DestroyTexture(hzbTex);
}

// E4 — the strategy seam end-to-end: HzbCullingStrategy reserves the
// phase-B slice for the view's own generation and NOT for cascade slices
// (cascades stay frustum-only in v1 — the main-view HZB doesn't apply to
// light frusta).
TEST(RGGpuDriven, HzbStrategyReservesPhaseBForOwnSliceOnly)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto pipeline = GPUCullingFactory::CreateBalanced(dev.get());
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    pipeline->BeginFrame(&frame, scene.get());
    HzbCullingStrategy strategy;
    ViewCullingContext ctx{};
    ctx.Id = 4;
    ctx.CascadeIndex = kCullingCascadeIndexNone;
    ctx.InstanceCount = 8;
    ctx.CullingPipeline = pipeline.get();
    ctx.Scene = scene.get();
    strategy.ScheduleCulling(ctx);
    // A per-view cascade slice through the same strategy must NOT reserve.
    ctx.CascadeIndex = 1;
    strategy.ScheduleCulling(ctx);
    pipeline->EndFrame();

    const auto& ranges = pipeline->GetViewVisibilityRanges();
    ASSERT_EQ(ranges.size(), 3u) << "own slice + cascade slice + ONE phase-B reservation";
    uint32_t phaseB = 0;
    for (const auto& r : ranges)
        if (r.slicePhase == 1u)
        {
            ++phaseB;
            EXPECT_EQ(r.cascadeIndex, kCullingCascadeIndexNone)
                << "only the view's own (non-cascade) generation reserves";
        }
    EXPECT_EQ(phaseB, 1u);

    frame.Execute();
    dev->WaitForIdle();
}

// Introspection purity: the debug server's get_render_stats reports the scatter
// pipeline's readiness, and reading it must not be what makes it ready. Before
// IntrospectScatter existed the handler called GetOrCreateScatterPipeline —
// which loads SPIR-V, creates a Vulkan pipeline and latches the compact/stats
// variant axes — so a stats poll on a cold builder reported (and caused) its own
// side effect, and IsScatterStatsActive could flip between two consecutive polls
// with the reader as the only cause. This fails if any introspection accessor
// ever regains a creating path.
TEST(RGGpuDriven, IntrospectScatterNeverCreatesThePipeline)
{
    RG_REQUIRE_DEVICE(dev);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());

    // Cold builder: the render path has not scheduled a scatter pass yet.
    const auto cold = builder->IntrospectScatter();
    EXPECT_FALSE(cold.PipelineReady) << "a freshly initialized builder has no scatter pipeline";

    // Poll it the way a stats reader does. Repeated reads must be identical,
    // and none of them may warm anything.
    for (int poll = 0; poll < 4; ++poll)
        EXPECT_EQ(builder->IntrospectScatter(), cold) << "read " << poll << " moved the state";

    // Only the render path's explicit create changes the answer. Skip rather
    // than fail when the package is unavailable — that arm proves nothing about
    // purity, which the assertions above already established.
    if (!builder->GetOrCreateScatterPipeline().IsValid())
    {
        builder->Shutdown();
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";
    }

    const auto warm = builder->IntrospectScatter();
    EXPECT_TRUE(warm.PipelineReady) << "the snapshot must report what the render path built";
    EXPECT_EQ(builder->IntrospectScatter(), warm) << "reads stay pure once warm too";
    builder->Shutdown();
}

// E2 — ScheduleUnifiedScatter(RGFrame&): publishes the ordering proxy,
// consumes the registered slices, publishes consumer ranges, and the pass's
// first-touch barrier carries Transfer in its dst scope — the declared
// CopyDst write union that puts the cursor zero-fills INSIDE the barrier
// scope (cross-frame WAR rides this pass's global execution dependency; the
// fill bytes themselves are exec-recorded and covered by
// StreamReadyBarrierShapeSurvives + gate 1).
TEST(RGGpuDriven, ScheduleUnifiedScatterPublishesOrderingWithTransferInDstScope)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    BufferDesc vd;
    vd.size = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());
    RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

    BatchRegistry registry;
    registry.OnInstanceAdded(/*materialIndex=*/0u, /*meshIndex=*/0u, /*mirrored=*/false);

    builder->BeginArenaFrame();
    GPUDrawStreamBuilder::SliceRegistration slice{};
    slice.viewId                 = 1u;
    slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
    slice.disableVisibilityCheck = true;
    builder->RegisterSlice(slice);

    // Color-only slice (cascadeIndex none) — no shadow table needed, so the
    // depth-class span is unused (empty); empty color-class span = merge off.
    RGBuffer ordering = builder->ScheduleUnifiedScatter(
        frame, "E2", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot, sceneRG.Meshes, registry,
        /*materialDepthClass=*/{}, /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));
    ASSERT_TRUE(ordering.IsValid());
    EXPECT_EQ(builder->GetPendingSliceCount(), 0u) << "slices consumed by the schedule";
    const auto range = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
        GPUDrawStreamBuilder::SlicePhase::A);
    EXPECT_TRUE(range.IsValid()) << "the schedule publishes a consumer range per slice x batch";
    EXPECT_EQ(range.even.maxDrawCount, 1u) << "range bound = the batch's snapshot capacity";

    // HZB two-generation seam: a SECOND schedule call in the same app frame
    // registering the SAME (view, cascade) under SlicePhase::B must publish
    // its own range at a DISJOINT arena offset — and must not disturb the
    // phase-A lookup (the R2.1 recovery-generation contract).
    GPUDrawStreamBuilder::SliceRegistration sliceB = slice;
    sliceB.phase = GPUDrawStreamBuilder::SlicePhase::B;
    builder->RegisterSlice(sliceB);
    RGBuffer orderingB = builder->ScheduleUnifiedScatter(
        frame, "E2.B", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot, sceneRG.Meshes, registry,
        /*materialDepthClass=*/{}, /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));
    ASSERT_TRUE(orderingB.IsValid());
    const auto rangeB = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
        GPUDrawStreamBuilder::SlicePhase::B);
    ASSERT_TRUE(rangeB.IsValid()) << "phase-B generation publishes its own range";
    EXPECT_NE(rangeB.even.cmdByteOffset, range.even.cmdByteOffset)
        << "phase generations claim disjoint arena record ranges";
    EXPECT_NE(rangeB.even.countByteOffset, range.even.countByteOffset);
    const auto rangeA2 = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
        GPUDrawStreamBuilder::SlicePhase::A);
    EXPECT_EQ(rangeA2.even.cmdByteOffset, range.even.cmdByteOffset)
        << "phase-B registration must not disturb the phase-A range";

    frame.MarkOutput(ordering);
    frame.Execute();
    dev->WaitForIdle();

    EXPECT_GE(PosOf(frame.Graph(), RGQuery::Subtree{"GPUDrawStream.Scatter"}), 0);
    bool transferInDst = false;
    for (const RGBarrier& bar : frame.Graph().Barriers())
        if (bar.Resource == ordering.Id && (bar.DstStage & RGStage::Transfer) != 0 &&
            (bar.DstAccess & RGAccessMask::TransferWrite) != 0)
            transferInDst = true;
    EXPECT_TRUE(transferInDst)
        << "the exec OPENS with Transfer-stage slot fills — the declared CopyDst write "
           "must put Transfer in the pass's barrier dst scope";

    dev->DestroyBuffer(visPhys);
}

// A cascade group's slices publish at cascadeIndexBase + c. Default base 0
// keeps the directional cascades at 0..3; a color fan-out (reflection probe
// faces) bases its slices in its own block so they never alias the view's
// own cascade-None slice or the shadow families.
TEST(RGGpuDriven, CascadeGroupPublishesRangesAtCascadeIndexBase)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto pipeline = GPUCullingFactory::CreateBalanced(dev.get());
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    constexpr uint8_t kFaceBase = 0x80u;
    pipeline->BeginFrame(&frame, scene.get());
    pipeline->SubmitView(MakeView(/*viewId=*/9, /*instances=*/8));
    CascadeCullingGroup cascades{};
    cascades.viewId = 9;
    cascades.cascadeCount = 2;
    cascades.instanceCount = 8;
    pipeline->SubmitCascadeGroup(cascades);
    CascadeCullingGroup faces{};
    faces.viewId = 9;
    faces.cascadeCount = 3;
    faces.cascadeIndexBase = kFaceBase;
    faces.shadowCasterDispatch = false;
    faces.instanceCount = 8;
    pipeline->SubmitCascadeGroup(faces);
    pipeline->EndFrame();

    std::vector<uint8_t> published;
    for (const auto& r : pipeline->GetViewVisibilityRanges())
        if (r.viewId == 9 && r.slicePhase == 0u)
            published.push_back(r.cascadeIndex);
    const std::vector<uint8_t> expected = {kCullingCascadeIndexNone, 0u, 1u,
                                           kFaceBase, kFaceBase + 1u, kFaceBase + 2u};
    EXPECT_EQ(published, expected)
        << "own slice, cascades at the default base, faces at their own base";

    frame.Execute();
    dev->WaitForIdle();
}

// The batch table a slice scatters from is its `table` field, not its cascade
// byte: a Color-table slice at a non-None cascade index (a probe face)
// publishes under the color key, and a Shadow-table slice publishes under the
// R1.5 shared-depth sentinel — with the SAME registry and depth-class span.
TEST(RGGpuDriven, SliceTableSelectsPublishedKeyIndependentOfCascadeIndex)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    BufferDesc vd;
    vd.size = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());
    RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

    BatchRegistry registry;
    registry.OnInstanceAdded(/*materialIndex=*/0u, /*meshIndex=*/0u, /*mirrored=*/false);
    // Material 0 is shared-depth eligible: a Shadow-table slice keys it on the
    // single-sided sentinel, a Color-table slice on the raw material.
    const uint8_t depthClass[1] = {static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided)};

    constexpr uint8_t kFaceIndex = 0x80u;
    builder->BeginArenaFrame();
    GPUDrawStreamBuilder::SliceRegistration face{};
    face.viewId                 = 1u;
    face.cascadeIndex           = kFaceIndex;
    face.table                  = GPUDrawStreamBuilder::SliceTable::Color;
    face.disableVisibilityCheck = true;
    builder->RegisterSlice(face);
    GPUDrawStreamBuilder::SliceRegistration cascade{};
    cascade.viewId                 = 1u;
    cascade.cascadeIndex           = 0u;
    cascade.table                  = GPUDrawStreamBuilder::SliceTable::Shadow;
    cascade.disableVisibilityCheck = true;
    builder->RegisterSlice(cascade);

    RGBuffer ordering = builder->ScheduleUnifiedScatter(
        frame, "Tables", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
        sceneRG.Meshes, registry, depthClass, /*materialColorClass=*/{},
        /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));
    ASSERT_TRUE(ordering.IsValid());

    using Phase = GPUDrawStreamBuilder::SlicePhase;
    EXPECT_TRUE(builder->FindBatchDrawRange(1u, kFaceIndex, 0u, 0u, Phase::A).IsValid())
        << "a Color-table slice keys on the material whatever its cascade index";
    EXPECT_FALSE(builder->FindBatchDrawRange(
                     1u, kFaceIndex, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 0u,
                     Phase::A).IsValid())
        << "a Color-table slice never publishes shadow sentinels";
    EXPECT_TRUE(builder->FindBatchDrawRange(
                    1u, 0u, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 0u, Phase::A)
                    .IsValid())
        << "a Shadow-table slice keys eligible casters on the sentinel";
    EXPECT_FALSE(builder->FindBatchDrawRange(1u, 0u, 0u, 0u, Phase::A).IsValid())
        << "a Shadow-table slice does not publish the eligible material's raw key";

    frame.MarkOutput(ordering);
    frame.Execute();
    dev->WaitForIdle();
    dev->DestroyBuffer(visPhys);
}

// Two Color-table slices of ONE view, at distinct cascade indices, scattering
// in ONE call from DISJOINT visibility slices (the reflection-probe full-bake
// shape: six faces, one view, one scatter). Each slice must draw exactly its
// own visible instances — no leakage of the other slice's set, no double
// counting. Meshes carry power-of-two triangle counts so the per-slice
// triangle sum names the exact instance set that survived.
TEST(RGGpuDriven, ColorSlicesOfOneViewScatterTheirOwnVisibilitySets)
{
    GameEngine::Rendering::Tests::ScopedEnvVar statsEnv("GE_SCATTER_STATS", "1");

    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);

    auto scene = std::make_unique<GPUScene>(dev.get());
    ASSERT_TRUE(scene->Initialize(256u, 32u));
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter shaderpkg unavailable";
    if (!builder->IsScatterStatsActive())
        GTEST_SKIP() << "stats scatter variant unavailable";

    constexpr uint32_t kInstances = 4u;
    // Triangle counts 1/2/4/8: a slice's drawnTriangles is the bitmask of the
    // instances it drew.
    for (uint32_t i = 0; i < kInstances; ++i)
    {
        GPUMesh m{};
        m.indexCount        = 3u * (1u << i);
        m.vertexCount       = 3u;
        m.lodCount          = 1u;
        m.boundingRadius    = 1.0f;
        m.lodIndexCount[0]  = m.indexCount;
        m.lodIndexOffset[0] = 0u;
        m.lodThreshold[0]   = 0.0f;
        const uint32_t mesh = scene->AddMesh(m);
        GPUInstance inst{};
        inst.materialIndex  = 0u;
        inst.meshIndex      = mesh;
        inst.boundingRadius = 1.0f;
        scene->AddInstance(inst);
    }
    scene->FlushGPUBuffers();

    // Two disjoint visibility slices in one buffer, laid out exactly as
    // GPUCullingPipeline lays out a fused group's slices.
    const size_t alignBytes =
        std::max<size_t>(dev->GetCapabilities().minStorageBufferOffsetAlignment, sizeof(uint32_t));
    const uint32_t sliceAOffsetBytes = 0u;
    const uint32_t sliceBOffsetBytes = static_cast<uint32_t>(alignBytes);
    BufferDesc vd;
    vd.size        = sliceBOffsetBytes + kInstances * sizeof(uint32_t);
    vd.usage       = static_cast<uint32_t>(BufferUsage::Storage);
    vd.memoryUsage = BufferMemoryUsage::Upload;
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());
    {
        std::vector<uint32_t> bits(vd.size / sizeof(uint32_t), 0u);
        const uint32_t aBase = sliceAOffsetBytes / sizeof(uint32_t);
        const uint32_t bBase = sliceBOffsetBytes / sizeof(uint32_t);
        bits[aBase + 0] = 1u; // slice A sees instances 0,1 -> 1 + 2 = 3 triangles
        bits[aBase + 1] = 1u;
        bits[bBase + 2] = 1u; // slice B sees instances 2,3 -> 4 + 8 = 12 triangles
        bits[bBase + 3] = 1u;
        dev->UpdateBuffer(visPhys, 0, vd.size, bits.data());
    }

    constexpr uint32_t kViewId   = 1u;
    constexpr uint8_t  kFaceOne  = 0x80u;
    constexpr uint8_t  kFaceTwo  = 0x81u;

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    auto runFrame = [&](uint64_t frameIndex)
    {
        builder->BeginArenaFrame();
        scene->FlushGPUBuffers();
        frame.BeginFrame(frameIndex);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        for (const uint8_t face : {kFaceOne, kFaceTwo})
        {
            GPUDrawStreamBuilder::SliceRegistration s{};
            s.viewId                = kViewId;
            s.cascadeIndex          = face;
            s.table                 = GPUDrawStreamBuilder::SliceTable::Color;
            s.visibilityOffsetBytes = face == kFaceOne ? sliceAOffsetBytes : sliceBOffsetBytes;
            builder->RegisterSlice(s);
        }

        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "ProbeFaces", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, scene->GetBatchRegistry(), /*materialDepthClass=*/{},
            /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, kInstances,
            static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
    };

    runFrame(0);
    runFrame(1); // the reduce in this BeginArenaFrame publishes frame 0's stats

    // Each slice publishes its own range per (material, mesh), and the two
    // slices' record regions must not alias.
    using Phase = GPUDrawStreamBuilder::SlicePhase;
    for (uint32_t mesh = 0; mesh < kInstances; ++mesh)
    {
        const auto ra = builder->FindBatchDrawRange(kViewId, kFaceOne, 0u, mesh, Phase::A);
        const auto rb = builder->FindBatchDrawRange(kViewId, kFaceTwo, 0u, mesh, Phase::A);
        ASSERT_TRUE(ra.IsValid() && rb.IsValid()) << "mesh " << mesh;
        EXPECT_NE(ra.even.cmdByteOffset, rb.even.cmdByteOffset)
            << "mesh " << mesh << ": slices must own disjoint record regions";
        EXPECT_NE(ra.even.countByteOffset, rb.even.countByteOffset)
            << "mesh " << mesh << ": slices must own disjoint count slots";
    }

    struct Sample
    {
        uint32_t passedCasters  = 0;
        uint64_t drawnTriangles = 0;
        bool     present        = false;
    };
    auto sampleOf = [&](uint8_t face)
    {
        Sample out{};
        for (const auto& s : builder->GetShadowArcStats())
            if (s.viewId == kViewId && s.cascadeIndex == face && s.phase == 0u)
            {
                out.passedCasters  = s.passedCasters;
                out.drawnTriangles = s.drawnTriangles;
                out.present        = true;
            }
        return out;
    };
    // The reduce runs in BeginArenaFrame, so frame 1's counters land here.
    builder->BeginArenaFrame();
    const Sample a = sampleOf(kFaceOne);
    const Sample b = sampleOf(kFaceTwo);
    ASSERT_TRUE(a.present && b.present) << "per-slice stats missing — instrument broken";

    EXPECT_EQ(a.passedCasters, 2u) << "slice A must emit one record per instance it sees";
    EXPECT_EQ(b.passedCasters, 2u) << "slice B must emit one record per instance it sees";
    EXPECT_EQ(a.drawnTriangles, 3u) << "slice A must draw instances {0,1} only";
    EXPECT_EQ(b.drawnTriangles, 12u) << "slice B must draw instances {2,3} only";

    dev->DestroyBuffer(visPhys);
}

// Crossfade head/tail publication. Two properties the depth prepass depends on:
// the tail is a segment of its own that the depth consumer can decline to issue,
// and turning the feature on does NOT move the head region — same arena base,
// same byte offset, so the prepass draws exactly the records it drew before.
// A layout that folded the tail into the row (the pre-split shape) fails the
// second half: every row after the first shifts.
//
// Nothing fades in this fixture, so the enabled arm is also the AT-REST shape:
// publication keys on the duration, never on live fades. That is what puts the
// tail variant's compile request on the first enabled frame, and equally what
// makes the at-rest residual one count-0 indirect draw per present tail.
TEST(RGGpuDriven, CrossfadeTailIsASeparateSegmentAndLeavesTheHeadRegionInPlace)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    BufferDesc vd;
    vd.size = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    // Two rows, so a shifted record-offset prefix shows up as a moved second
    // row rather than only a moved block base. THREE instances per row,
    // deliberately odd: a tail region holds PAIRS, so its bound must round down
    // to whole pairs — a row of capacity 3 publishes a tail bound of 2.
    BatchRegistry registry;
    for (uint32_t mesh = 0; mesh < 2u; ++mesh)
        for (uint32_t i = 0; i < 3u; ++i)
            registry.OnInstanceAdded(/*materialIndex=*/0u, mesh, /*mirrored=*/false);

    // A camera slice crossfades only with a real perspective term and no forced
    // level — the same gate the scheduler applies.
    GPUDrawStreamBuilder::SliceRegistration slice{};
    slice.viewId                 = 1u;
    slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
    slice.disableVisibilityCheck = true;
    slice.lod.projScaleY         = 1.0f;

    auto scheduleOnce = [&](float crossfadeDuration)
    {
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        builder->BeginArenaFrame();
        GPUDrawStreamBuilder::SliceRegistration s2 = slice;
        s2.lod.crossfadeDuration = crossfadeDuration;
        builder->RegisterSlice(s2);
        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "XF", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, registry, /*materialDepthClass=*/{}, /*materialColorClass=*/{},
            /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
            static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        std::array<GPUDrawStreamBuilder::BatchDrawRange, 2> out{};
        for (uint32_t mesh = 0; mesh < 2u; ++mesh)
            out[mesh] = builder->FindBatchDrawRange(
                1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, mesh,
                GPUDrawStreamBuilder::SlicePhase::A);
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
        return out;
    };

    const auto off = scheduleOnce(0.0f);
    const auto on  = scheduleOnce(0.25f);

    for (uint32_t mesh = 0; mesh < 2u; ++mesh)
    {
        ASSERT_TRUE(off[mesh].IsValid()) << "mesh " << mesh;
        ASSERT_TRUE(on[mesh].IsValid()) << "mesh " << mesh;

        EXPECT_FALSE(off[mesh].evenTail.IsPresent())
            << "mesh " << mesh << ": with the feature off the tail segment must be ABSENT, "
               "so consumers issue exactly the draws they issued before it existed";

        EXPECT_TRUE(on[mesh].evenTail.IsPresent()) << "mesh " << mesh;
        EXPECT_EQ(on[mesh].even.maxDrawCount, 3u) << "mesh " << mesh;
        EXPECT_EQ(on[mesh].evenTail.maxDrawCount, 2u)
            << "mesh " << mesh << ": the tail bound must round the odd capacity down to a "
               "whole pair, or the consumer draws a slot the scatter refused to write";
        EXPECT_NE(on[mesh].evenTail.cmdByteOffset, on[mesh].even.cmdByteOffset)
            << "mesh " << mesh << ": the tail must live in its own record block";
        EXPECT_NE(on[mesh].evenTail.countByteOffset, on[mesh].even.countByteOffset)
            << "mesh " << mesh << ": the tail must have its own cursor";

        // The load-bearing one: the head region does not move.
        EXPECT_EQ(on[mesh].even.cmdByteOffset, off[mesh].even.cmdByteOffset)
            << "mesh " << mesh << ": enabling the crossfade moved the HEAD record region";
        EXPECT_EQ(on[mesh].even.countByteOffset, off[mesh].even.countByteOffset)
            << "mesh " << mesh << ": enabling the crossfade moved the HEAD cursor";
        EXPECT_EQ(on[mesh].even.maxDrawCount, off[mesh].even.maxDrawCount)
            << "mesh " << mesh << ": capacity is the snapshot live count either way";

        // The consumer-rule API against the REAL published range: one segment
        // set, head then tail, issued by the depth prepass and the colour pass
        // alike. Their equality is what makes early-Z admit exactly the
        // fragments the colour dither keeps.
        const auto segs = on[mesh].Segments();
        ASSERT_EQ(segs.count, 2u) << "mesh " << mesh;
        EXPECT_EQ(segs.items[0].segment.cmdByteOffset, on[mesh].even.cmdByteOffset)
            << "mesh " << mesh;
        EXPECT_FALSE(segs.items[0].crossfading) << "mesh " << mesh;
        EXPECT_EQ(segs.items[1].segment.cmdByteOffset, on[mesh].evenTail.cmdByteOffset)
            << "mesh " << mesh << ": both passes issue the tail";
        EXPECT_TRUE(segs.items[1].crossfading) << "mesh " << mesh;
    }

    dev->DestroyBuffer(visPhys);
}

// The consumer rule as API, all parities: Segments() yields heads AND crossfade
// tails, with the parity each segment draws under and `crossfading` set on the
// tails alone. There is ONE accessor because the depth prepass and the colour
// pass must issue the SAME set — a tail drawn in only one of them is exactly the
// asymmetry that either z-kills the dither or leaves the fading instance out of
// prepass depth. Pure struct; no device.
TEST(RGGpuDriven, SegmentsYieldHeadsAndTailsPerParityForEveryRasterPass)
{
    GPUDrawStreamBuilder::BatchDrawRange range{};
    range.even         = {/*cmd=*/100u, /*count=*/10u, /*max=*/3u};
    range.evenTail     = {/*cmd=*/200u, /*count=*/20u, /*max=*/2u};
    range.mirrored     = {/*cmd=*/300u, /*count=*/30u, /*max=*/3u};
    range.mirroredTail = {/*cmd=*/400u, /*count=*/40u, /*max=*/2u};

    const auto segs = range.Segments();
    ASSERT_EQ(segs.count, 4u);
    EXPECT_EQ(segs.items[0].segment.cmdByteOffset, 100u);
    EXPECT_FALSE(segs.items[0].mirrored);
    EXPECT_FALSE(segs.items[0].crossfading) << "the parity-0 HEAD must not take the dither variant";
    EXPECT_EQ(segs.items[1].segment.cmdByteOffset, 200u);
    EXPECT_FALSE(segs.items[1].mirrored);
    EXPECT_TRUE(segs.items[1].crossfading) << "the parity-0 TAIL must take the dither variant";
    EXPECT_EQ(segs.items[2].segment.cmdByteOffset, 300u);
    EXPECT_TRUE(segs.items[2].mirrored);
    EXPECT_FALSE(segs.items[2].crossfading) << "the parity-1 HEAD must not take the dither variant";
    EXPECT_EQ(segs.items[3].segment.cmdByteOffset, 400u);
    EXPECT_TRUE(segs.items[3].mirrored);
    EXPECT_TRUE(segs.items[3].crossfading) << "the parity-1 TAIL must take the dither variant";

    // An all-mirrored batch (only the parity-1 row exists): the absent parity-0
    // segments are skipped and the flip flag survives.
    GPUDrawStreamBuilder::BatchDrawRange mirroredOnly{};
    mirroredOnly.mirrored     = {/*cmd=*/300u, /*count=*/30u, /*max=*/3u};
    mirroredOnly.mirroredTail = {/*cmd=*/400u, /*count=*/40u, /*max=*/2u};
    const auto segsM = mirroredOnly.Segments();
    ASSERT_EQ(segsM.count, 2u);
    EXPECT_EQ(segsM.items[0].segment.cmdByteOffset, 300u);
    EXPECT_TRUE(segsM.items[0].mirrored);
    EXPECT_FALSE(segsM.items[0].crossfading);
    EXPECT_EQ(segsM.items[1].segment.cmdByteOffset, 400u);
    EXPECT_TRUE(segsM.items[1].mirrored);
    EXPECT_TRUE(segsM.items[1].crossfading);
}

// A tail-less range is BOTH the feature-OFF shape and the shape every shadow /
// cascade slice publishes (only a cascade=None slice reserves a tail block).
// There, no consumer may ask for the dither variant at all — that is what makes
// the default byte-identical and what keeps a dithered caster, whose holes PCF
// would average into a washed-out shadow, structurally impossible.
TEST(RGGpuDriven, ATailLessRangeAsksNoPassForTheDitherVariant)
{
    GPUDrawStreamBuilder::BatchDrawRange tailLess{};
    tailLess.even     = {/*cmd=*/100u, /*count=*/10u, /*max=*/3u};
    tailLess.mirrored = {/*cmd=*/300u, /*count=*/30u, /*max=*/3u};

    const auto segs = tailLess.Segments();
    ASSERT_EQ(segs.count, 2u);
    EXPECT_EQ(segs.items[0].segment.cmdByteOffset, 100u);
    EXPECT_EQ(segs.items[1].segment.cmdByteOffset, 300u);
    for (const auto& sd : segs)
        EXPECT_FALSE(sd.crossfading)
            << "a tail-less range asked a raster pass for the dither variant";
}

// P2 — the shared cascade=None slice: with a non-empty color-class map, the
// color/depth-prepass slice publishes ONE range per (colorClassId, mesh). The
// color pass AND the depth prepass both look up by colorClassId, so they
// resolve the SAME merged range by construction — the shared-slice desync the
// review flagged (color-only A/B misses it) cannot happen. A stale lookup by a
// merged member's raw materialIndex must find NOTHING, proving the re-key is
// mandatory and lockstep across both consumers.
TEST(RGGpuDriven, ColorClassMergePublishesOneSharedRangePerClass)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    BufferDesc vd;
    vd.size = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());
    RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

    // mat 0 and mat 1 on mesh 0 share color class C0; the map merges them.
    BatchRegistry registry;
    registry.OnInstanceAdded(/*materialIndex=*/0u, /*meshIndex=*/0u, /*mirrored=*/false);
    registry.OnInstanceAdded(/*materialIndex=*/1u, /*meshIndex=*/0u, /*mirrored=*/false);
    const uint32_t C0 = GPUDrawStreamBuilder::kColorClassBase;
    const std::vector<uint32_t> colorClass{C0, C0};

    builder->BeginArenaFrame();
    GPUDrawStreamBuilder::SliceRegistration slice{};
    slice.viewId                 = 1u;
    slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
    slice.disableVisibilityCheck = true;
    builder->RegisterSlice(slice);

    RGBuffer ordering = builder->ScheduleUnifiedScatter(
        frame, "ColorMerge", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot, sceneRG.Meshes, registry,
        /*materialDepthClass=*/{}, colorClass, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));
    ASSERT_TRUE(ordering.IsValid());

    // One published range keyed on the CLASS id: both the color pass and the
    // main-view depth prepass look this up by colorClassId, so they land on the
    // identical merged range. maxDrawCount == the merged capacity (mat0 + mat1).
    const auto merged = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, C0, 0u,
        GPUDrawStreamBuilder::SlicePhase::A);
    ASSERT_TRUE(merged.IsValid());
    EXPECT_EQ(merged.even.maxDrawCount, 2u) << "merged range must bound both members' instances";

    // A lookup by a merged member's raw materialIndex (the pre-P2 key) must miss
    // — the table is keyed by the class id, so a non-lockstep consumer would
    // silently draw nothing rather than double-draw.
    EXPECT_FALSE(builder->FindBatchDrawRange(1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
                                             GPUDrawStreamBuilder::SlicePhase::A)
                     .IsValid());
    EXPECT_FALSE(builder->FindBatchDrawRange(1u, GPUDrawStreamBuilder::kCascadeIndexNone, 1u, 0u,
                                             GPUDrawStreamBuilder::SlicePhase::A)
                     .IsValid());

    frame.MarkOutput(ordering);
    frame.Execute();
    dev->WaitForIdle();

    dev->DestroyBuffer(visPhys);
}

// Draw consolidation: non-empty ordered/group spans make the schedule build
// ordered-axis tables and publish ONE range per (classKey, group) RUN keyed by
// pool-group id — one DrawIndexedIndirectCount per (classKey, group) instead
// of per (classKey, mesh), spanning the run's mesh-major-compacted draw block.
// Consumers look up by group on the same signal; a non-lockstep per-mesh
// lookup for a mesh whose id differs from its group must find nothing
// (draw-nothing, never double-draw — the P2 re-key precedent).
TEST(RGGpuDriven, GroupedSchedulePublishesOneRangePerPoolGroup)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";
    if (!builder->GetOrCreateGroupCompactPipeline().IsValid())
        GTEST_SKIP() << "draw_stream_group_compact.shaderpkg unavailable";

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    BufferDesc vd;
    vd.size = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());
    RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

    // (mat 0, mesh 0) and (mat 0, mesh 1) share pool group 3: ordered ranks
    // {0, 1}, both mapping to group 3 (the plan's paired spans).
    BatchRegistry registry;
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/false);
    registry.OnInstanceAdded(0u, 1u, /*mirrored=*/false);
    const std::vector<uint32_t> meshOrdered{0u, 1u};
    const std::vector<uint32_t> orderedToGroup{3u, 3u};

    builder->BeginArenaFrame();
    GPUDrawStreamBuilder::SliceRegistration slice{};
    slice.viewId                 = 1u;
    slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
    slice.disableVisibilityCheck = true;
    builder->RegisterSlice(slice);

    RGBuffer ordering = builder->ScheduleUnifiedScatter(
        frame, "Grouped", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
        sceneRG.Meshes, registry,
        /*materialDepthClass=*/{}, /*materialColorClass=*/{}, meshOrdered, orderedToGroup, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));
    ASSERT_TRUE(ordering.IsValid());

    // One merged range under (mat 0, group 3), bounding both meshes' instances.
    const auto merged = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 3u,
        GPUDrawStreamBuilder::SlicePhase::A);
    ASSERT_TRUE(merged.IsValid());
    EXPECT_EQ(merged.even.maxDrawCount, 2u);

    // The per-mesh keys are gone: mesh 0's id (0) and mesh 1's id (1) find
    // nothing — a stale per-mesh consumer draws nothing rather than
    // double-consuming the merged region.
    EXPECT_FALSE(builder->FindBatchDrawRange(1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
                                             GPUDrawStreamBuilder::SlicePhase::A)
                     .IsValid());
    EXPECT_FALSE(builder->FindBatchDrawRange(1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 1u,
                                             GPUDrawStreamBuilder::SlicePhase::A)
                     .IsValid());

    frame.MarkOutput(ordering);
    frame.Execute();
    dev->WaitForIdle();

    dev->DestroyBuffer(visPhys);
}

// Winding parity: MakeStreamKey masks the mesh field to 24 bits, so a batch's
// parity-0 and parity-1 table rows collapse to the SAME range-map key. The
// publish step MUST merge them into ONE BatchDrawRange with two segments (even
// + mirrored) rather than overwrite (must-fix #3). A mirror-free batch keeps a
// single-segment range (byte-identical to pre-parity).
TEST(RGGpuDriven, ScheduleUnifiedScatterMergesParitySiblingsIntoTwoSegments)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    BufferDesc vd;
    vd.size = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());
    RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

    // (mat 0, mesh 0): 2 non-mirrored + 3 mirrored — one batch, both parities.
    // (mat 1, mesh 0): 1 non-mirrored only — a mirror-free single-segment batch.
    BatchRegistry registry;
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/false);
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/false);
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/true);
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/true);
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/true);
    registry.OnInstanceAdded(1u, 0u, /*mirrored=*/false);

    builder->BeginArenaFrame();
    GPUDrawStreamBuilder::SliceRegistration slice{};
    slice.viewId                 = 1u;
    slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
    slice.disableVisibilityCheck = true;
    builder->RegisterSlice(slice);

    RGBuffer ordering = builder->ScheduleUnifiedScatter(
        frame, "Parity", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot, sceneRG.Meshes, registry,
        /*materialDepthClass=*/{}, /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));
    ASSERT_TRUE(ordering.IsValid());

    // (mat 0, mesh 0): ONE range with both segments, disjoint offsets, capacities split.
    const auto mixed = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u, GPUDrawStreamBuilder::SlicePhase::A);
    ASSERT_TRUE(mixed.IsValid());
    EXPECT_TRUE(mixed.even.IsPresent());
    EXPECT_TRUE(mixed.mirrored.IsPresent());
    EXPECT_EQ(mixed.even.maxDrawCount, 2u);         // even (non-mirrored)
    EXPECT_EQ(mixed.mirrored.maxDrawCount, 3u); // odd (mirrored)
    EXPECT_NE(mixed.even.cmdByteOffset, mixed.mirrored.cmdByteOffset)
        << "the mirrored segment must NOT overwrite the even segment (must-fix #3)";
    EXPECT_NE(mixed.even.countByteOffset, mixed.mirrored.countByteOffset);

    // (mat 1, mesh 0): mirror-free → single even segment, no mirrored sibling.
    const auto pure = builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 1u, 0u, GPUDrawStreamBuilder::SlicePhase::A);
    ASSERT_TRUE(pure.IsValid());
    EXPECT_TRUE(pure.even.IsPresent());
    EXPECT_FALSE(pure.mirrored.IsPresent()) << "a mirror-free batch keeps a single-segment range";
    EXPECT_EQ(pure.even.maxDrawCount, 1u);

    frame.MarkOutput(ordering);
    frame.Execute();
    dev->WaitForIdle();

    dev->DestroyBuffer(visPhys);
}

// P3 recycle reset — a recycled GPUScene slot must not hand the prior tenant's
// dwell-band history to its new tenant. The band rule is idempotent, so an
// inherited level inside the band is a PERMANENT fixed point at a still camera
// (in BatchScatterComputeTests, ConservativeBoundsPreserveReferenceHysteresis holds
// a previous level the stateless pick would drop, and
// LodBandConvergesToAFixedPointInOneStep shows the held level does not move again);
// the only exit is the OnInstanceSlotsRecycled → EnsurePrevLodBuffer sentinel
// refill, driven here end-to-end through ScheduleUnifiedScatter + a real Execute
// per app frame. Instrument: the per-slice drawnTriangles stats channel (a LEVEL
// readout — the two meshes' distinct per-level triangle counts make the slice sum
// identify WHICH slot a refill touched, so a wrong-slot or whole-buffer fill cannot
// alias the pass).
namespace
{
using GameEngine::Rendering::Tests::ScopedEnvVar;
} // namespace

TEST(RGGpuDriven, RecycledSlotDwellHistoryResetsToTheStatelessPick)
{
    // The stats scatter variant compiles in drawnTriangles/lodChanges; the env
    // request is latched at this builder's first pipeline build.
    ScopedEnvVar statsEnv("GE_SCATTER_STATS", "1");

    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);

    auto scene = std::make_unique<GPUScene>(dev.get());
    ASSERT_TRUE(scene->Initialize(256u, 32u));
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter shaderpkg unavailable";
    if (!builder->IsScatterStatsActive())
        GTEST_SKIP() << "stats scatter variant unavailable";

    // Two 2-level meshes with DISTINCT per-level triangle counts and one switch
    // threshold at coverage 0.1 (radius 1, projScaleY 1 => coverage = 1/dist).
    auto makeMesh = [](uint32_t lod0Indices, uint32_t lod1Indices)
    {
        GPUMesh m{};
        m.indexCount        = lod0Indices;
        m.vertexCount       = 3u;
        m.lodCount          = 2u;
        m.boundingRadius    = 1.0f;
        m.lodIndexCount[0]  = lod0Indices;
        m.lodIndexOffset[0] = 0u;
        m.lodIndexCount[1]  = lod1Indices;
        m.lodIndexOffset[1] = lod0Indices;
        m.lodThreshold[0]   = 0.1f;
        m.lodThreshold[1]   = 0.0f; // coarsest level always clears
        return m;
    };
    const uint32_t meshA = scene->AddMesh(makeMesh(30u, 12u)); // LOD0 10 tri / LOD1 4 tri
    const uint32_t meshB = scene->AddMesh(makeMesh(60u, 6u));  // LOD0 20 tri / LOD1 2 tri

    auto makeInstance = [](uint32_t materialIndex, uint32_t meshIndex)
    {
        GPUInstance inst{};
        inst.materialIndex  = materialIndex;
        inst.meshIndex      = meshIndex;
        inst.boundingRadius = 1.0f;
        return inst;
    };
    scene->AddInstance(makeInstance(1u, meshA)); // slot 0
    scene->AddInstance(makeInstance(2u, meshB)); // slot 1 — recycled below
    scene->AddInstance(makeInstance(1u, meshA)); // slot 2

    constexpr uint32_t kViewId    = 1u;
    constexpr float    kBand      = 0.25f;
    constexpr float    kFarDist   = 11.0f; // coverage 0.0909 < 0.1: coarse everywhere
    constexpr float    kStillDist = 9.0f;  // coverage 0.1111: the stateless pick is
                                           // LOD0, the banded gain gate (0.125) holds
    // Camera-slice triangle sums (slots A + B + A):
    constexpr uint64_t kAllHeldCoarse = 4u + 2u + 4u;  // every slot held at LOD1
    constexpr uint64_t kSlot1Reset    = 4u + 20u + 4u; // ONLY slot 1 re-picked LOD0

    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    // One app frame at `dist` (a still camera = the same dist every call).
    // Returns the PREVIOUS frame's camera-slice stats, reduced by this frame's
    // BeginArenaFrame (fence-gated; complete by the prior WaitForIdle).
    struct SliceSample
    {
        uint64_t drawnTriangles = 0;
        uint32_t lodChanges     = 0;
        bool     present        = false;
    };
    uint64_t frameIndex = 0;
    auto runFrame = [&](float dist, std::vector<uint32_t> recycled)
    {
        builder->BeginArenaFrame();
        SliceSample prev{};
        for (const auto& s : builder->GetShadowArcStats())
            if (s.viewId == kViewId && s.cascadeIndex == GPUDrawStreamBuilder::kCascadeIndexNone)
            {
                prev.drawnTriangles = s.drawnTriangles;
                prev.lodChanges     = s.lodChanges;
                prev.present        = true;
            }
        if (!recycled.empty())
            builder->OnInstanceSlotsRecycled(std::move(recycled));

        scene->FlushGPUBuffers();
        frame.BeginFrame(frameIndex++);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        GPUDrawStreamBuilder::SliceRegistration slice{};
        slice.viewId                 = kViewId;
        slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        slice.disableVisibilityCheck = true;
        slice.lod.projScaleY         = 1.0f;
        slice.lod.cameraPos[0]       = dist;
        slice.lod.lodHysteresisBand  = kBand;
        builder->RegisterSlice(slice);

        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "Recycle", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, scene->GetBatchRegistry(), /*materialDepthClass=*/{},
            /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{},
            scene->GetInstanceCount(), static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
        return prev;
    };

    runFrame(kFarDist, {}); // F1: empty history -> stateless coarse; commits prev=LOD1

    const SliceSample f1 = runFrame(kStillDist, {});
    ASSERT_TRUE(f1.present) << "stats readback did not surface — instrument broken";
    EXPECT_EQ(f1.drawnTriangles, kAllHeldCoarse);
    EXPECT_EQ(f1.lodChanges, 3u) << "first banded frame counts every instance (empty history)";

    const SliceSample f2 = runFrame(kStillDist, {});
    ASSERT_TRUE(f2.present);
    EXPECT_EQ(f2.drawnTriangles, kAllHeldCoarse)
        << "the dwell band must hold the coarse level at a still camera";
    EXPECT_EQ(f2.lodChanges, 0u);

    // GPUScene recycled slot 1 to a new tenant. The camera has NOT moved.
    const SliceSample f3 = runFrame(kStillDist, {1u});
    ASSERT_TRUE(f3.present);
    EXPECT_EQ(f3.drawnTriangles, kAllHeldCoarse)
        << "F3 still holds — the permanent fixed point the reset exists to break";
    EXPECT_EQ(f3.lodChanges, 0u);

    const SliceSample f4 = runFrame(kStillDist, {});
    ASSERT_TRUE(f4.present);
    EXPECT_EQ(f4.drawnTriangles, kSlot1Reset)
        << "the recycled slot's new tenant must take the STATELESS pick: its history "
           "was refilled with kLodNoHistory while slots 0/2 kept theirs";
    EXPECT_EQ(f4.lodChanges, 1u) << "exactly the recycled instance changed level";

    const SliceSample f5 = runFrame(kStillDist, {});
    ASSERT_TRUE(f5.present);
    EXPECT_EQ(f5.drawnTriangles, kSlot1Reset)
        << "the new tenant's own history persists — a reset must not repeat";
    EXPECT_EQ(f5.lodChanges, 0u);

    dev->DestroyBuffer(visPhys);
}

// The crossfade half of the same reset contract, driven end-to-end through the
// PRODUCTION path (EnsureLodFadeBuffer's clearAll -> the scatter pass's fill ->
// the dispatch's first read) rather than a hand-seeded state buffer. A fresh
// view's fade slots must arrive as kLodNoHistory, so an instance whose FIRST pick
// is a coarser level draws one opaque head record instead of dissolving in from
// LOD0 as a tail pair — a pair carries no head record, so a frame that cannot
// issue the tail draw (cold LodCrossfade variant, compiled async) loses the
// instance from colour AND depth entirely.
//
// Instrument: the per-slice drawnTriangles stats channel, which is a LEVEL
// readout — a fading instance contributes BOTH of its levels, so a pair is
// arithmetically distinguishable from a head. F3 is the positive control that
// keeps this falsifiable: it forces a REAL transition out of the adopted state
// and must show a pair, so a green F1/F2 cannot be explained by "the sentinel
// disabled crossfading".
TEST(RGGpuDriven, FirstTouchCrossfadeStateArrivesAsNoHistoryAndAdoptsItsPick)
{
    ScopedEnvVar statsEnv("GE_SCATTER_STATS", "1");

    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);

    auto scene = std::make_unique<GPUScene>(dev.get());
    ASSERT_TRUE(scene->Initialize(256u, 32u));
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter shaderpkg unavailable";
    if (!builder->IsScatterStatsActive())
        GTEST_SKIP() << "stats scatter variant unavailable";

    // One 2-level mesh, three instances sharing it — so they form ONE batch row
    // of capacity 3. A tail claim is a PAIR bounded by that same capacity
    // (t + 1 < capacity), so exactly one instance can pair and the other two fall
    // back to a head: the frame's triangle sum therefore differs from the
    // no-fade sum by exactly one outgoing level, deterministically and
    // independently of which instance wins the atomic.
    constexpr uint32_t kLod0Indices = 30u; // 10 tri
    constexpr uint32_t kLod1Indices = 12u; //  4 tri
    GPUMesh mesh{};
    mesh.indexCount        = kLod0Indices;
    mesh.vertexCount       = 3u;
    mesh.lodCount          = 2u;
    mesh.boundingRadius    = 1.0f;
    mesh.lodIndexCount[0]  = kLod0Indices;
    mesh.lodIndexOffset[0] = 0u;
    mesh.lodIndexCount[1]  = kLod1Indices;
    mesh.lodIndexOffset[1] = kLod0Indices;
    mesh.lodThreshold[0]   = 0.1f; // radius 1, projScaleY 1 => coverage == 1/dist
    mesh.lodThreshold[1]   = 0.0f;
    const uint32_t meshIdx = scene->AddMesh(mesh);

    for (uint32_t i = 0; i < 3u; ++i)
    {
        GPUInstance inst{};
        inst.materialIndex  = 1u; // one material + one mesh => a single row
        inst.meshIndex      = meshIdx;
        inst.boundingRadius = 1.0f;
        scene->AddInstance(inst);
    }

    constexpr uint32_t kViewId   = 1u;
    constexpr float    kFarDist  = 11.0f; // coverage 0.0909 < 0.1 => LOD1
    constexpr float    kNearDist = 5.0f;  // coverage 0.2         => LOD0
    // Triangle sums over the three instances.
    constexpr uint64_t kAllCoarseHeads = 4u * 3u;             // no fade, all LOD1
    constexpr uint64_t kAllFineHeads   = 10u * 3u;            // no fade, all LOD0
    constexpr uint64_t kOnePairToFine  = (10u + 4u) + 10u * 2u; // one pair + two heads

    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    struct SliceSample
    {
        uint64_t drawnTriangles = 0;
        bool     present        = false;
    };
    uint64_t frameIndex = 0;
    // A held clock: every frame stamps and reads the same nowSeconds, so a fade
    // that starts can never age out, and each frame's shape is decided purely by
    // the fade STATE rather than by elapsed time.
    constexpr float kNow = 1.0f;
    auto runFrame = [&](float dist)
    {
        builder->BeginArenaFrame();
        SliceSample prev{};
        for (const auto& s : builder->GetShadowArcStats())
            if (s.viewId == kViewId && s.cascadeIndex == GPUDrawStreamBuilder::kCascadeIndexNone)
            {
                prev.drawnTriangles = s.drawnTriangles;
                prev.present        = true;
            }

        scene->FlushGPUBuffers();
        builder->SetFrameTimeSeconds(kNow);
        frame.BeginFrame(frameIndex++);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        GPUDrawStreamBuilder::SliceRegistration slice{};
        slice.viewId                 = kViewId;
        slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        slice.disableVisibilityCheck = true;
        slice.lod.projScaleY         = 1.0f;
        slice.lod.cameraPos[0]       = dist;
        slice.lod.crossfadeDuration  = 0.25f; // engages the tail for this slice
        builder->RegisterSlice(slice);

        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "FadeFirstTouch", /*rgPassPhase=*/0, vis, sceneRG.Instances,
            sceneRG.ScatterHot, sceneRG.Meshes, scene->GetBatchRegistry(),
            /*materialDepthClass=*/{}, /*materialColorClass=*/{}, /*meshPoolOrdered=*/{},
            /*orderedToGroup=*/{}, scene->GetInstanceCount(),
            static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
        return prev;
    };

    runFrame(kFarDist); // F1: the view's first frame — clearAll fills the sentinel

    const SliceSample f1 = runFrame(kFarDist);
    ASSERT_TRUE(f1.present) << "stats readback did not surface — instrument broken";
    EXPECT_EQ(f1.drawnTriangles, kAllCoarseHeads)
        << "a first-touch pick above LOD0 must adopt that pick as its settled state: "
           "the fade slots arrived as kLodNoHistory, not as a settled LOD0 pair";

    const SliceSample f2 = runFrame(kNearDist);
    ASSERT_TRUE(f2.present);
    EXPECT_EQ(f2.drawnTriangles, kAllCoarseHeads)
        << "F2 re-reads the same held state at the same distance";

    // F3 sampled the kNearDist frame: a REAL transition LOD1 -> LOD0 out of the
    // state F1 adopted. This must pair — otherwise the sentinel has not fixed
    // first touch, it has disabled crossfading.
    const SliceSample f3 = runFrame(kNearDist);
    ASSERT_TRUE(f3.present);
    EXPECT_EQ(f3.drawnTriangles, kOnePairToFine)
        << "a settled instance that changes level MUST still fade (one pair fits a "
           "capacity-3 row); got " << f3.drawnTriangles << ", all-heads would be "
        << kAllFineHeads;

    dev->DestroyBuffer(visPhys);
}

// ── Scatter arena reservation ────────────────────────────────────────────────
// Two pins on what a crossfading call may claim, and on what it must never do
// when that claim does not fit. Both are boundary tests: the arena is one shared
// per-app-frame allocation, and a call that gets its reservation wrong either
// wastes it or drops a whole pass of world geometry.

// A crossfading call reserves tail blocks for the slices that can CLAIM a tail,
// not for every slice. A slice with no perspective term never crossfades
// (sliceCrossfades), so adding one must cost ONE head block — not a head block
// plus a dead tail block. Instrument: a second call's record base in the same
// app frame IS the first call's claim, so the delta between the two setups
// measures the reservation directly.
TEST(RGGpuDriven, TailBlocksAreReservedOnlyForTailCapableSlices)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    constexpr uint32_t kInstancesPerRow = 4u;
    BatchRegistry registry;
    for (uint32_t i = 0; i < kInstancesPerRow; ++i)
        registry.OnInstanceAdded(/*materialIndex=*/0u, /*meshIndex=*/0u, /*mirrored=*/false);

    // Schedule `extraOrthoSlices` non-crossfading colour slices beside one
    // crossfading camera slice, then schedule a SECOND call in the same app frame
    // and report where its head records landed — i.e. how much the first claimed.
    // Distinct viewIds so no two slices share a range-map key.
    auto probeClaimBytes = [&](uint32_t extraOrthoSlices) -> uint64_t
    {
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);
        builder->BeginArenaFrame();

        GPUDrawStreamBuilder::SliceRegistration camera{};
        camera.viewId                 = 1u;
        camera.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        camera.disableVisibilityCheck = true;
        camera.lod.projScaleY         = 1.0f;  // perspective => tail-capable
        camera.lod.crossfadeDuration  = 0.25f;
        builder->RegisterSlice(camera);
        for (uint32_t i = 0; i < extraOrthoSlices; ++i)
        {
            GPUDrawStreamBuilder::SliceRegistration ortho = camera;
            ortho.viewId         = 2u + i;
            ortho.lod.projScaleY = 0.0f;       // ortho/thumb => never crossfades
            builder->RegisterSlice(ortho);
        }
        const RGBuffer first = builder->ScheduleUnifiedScatter(
            frame, "ArenaA", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, registry, /*materialDepthClass=*/{}, /*materialColorClass=*/{},
            /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
            static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(first.IsValid());

        GPUDrawStreamBuilder::SliceRegistration probe = camera;
        probe.viewId                = 900u;
        probe.lod.crossfadeDuration = 0.0f;    // a plain call, so its own claim is minimal
        builder->RegisterSlice(probe);
        const RGBuffer second = builder->ScheduleUnifiedScatter(
            frame, "ArenaB", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, registry, /*materialDepthClass=*/{}, /*materialColorClass=*/{},
            /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
            static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(second.IsValid());
        const auto probeRange = builder->FindBatchDrawRange(
            900u, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
            GPUDrawStreamBuilder::SlicePhase::A);
        EXPECT_TRUE(probeRange.IsValid());
        return probeRange.even.cmdByteOffset;
    };

    const uint64_t oneSlice  = probeClaimBytes(0u);
    const uint64_t twoSlices = probeClaimBytes(1u);
    ASSERT_GT(oneSlice, 0u) << "the probe call must land after the first call's claim";

    // One crossfading camera slice claims head + tail, so half that claim is one
    // record block. Adding a tail-incapable slice must add exactly one more.
    // Under uniform tail striding the delta was two blocks: the added slice
    // reserved a tail region it can never write.
    const uint64_t oneBlockBytes = oneSlice / 2u;
    EXPECT_EQ(twoSlices - oneSlice, oneBlockBytes)
        << "a slice that cannot claim a tail must add ONE record block, not two: "
           "one-slice claim " << oneSlice << " B, two-slice claim " << twoSlices << " B";

    dev->DestroyBuffer(visPhys);
}

// A schedule call whose claim does not fit the arena must RESIZE and proceed,
// never publish nothing. An invalid ordering drops every draw of that pass for
// the frame — the frame presents with its world geometry missing — and the
// crossfade's extra tail block is exactly what pushes a large scene past the
// initial capacity, so this is the reachable case rather than a hypothetical.
TEST(RGGpuDriven, AnArenaClaimTooLargeForTheInitialCapacityGrowsInsteadOfSkipping)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    FramePools pools(dev.get());
    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    // Sized to straddle the initial record capacity the way the shipped scene
    // does: it FITS as head blocks alone and does not once every slice also owns
    // a tail block. Registry counts are the row capacities, so this costs one
    // hash-map increment per instance and no GPU memory of its own — the
    // dispatch still runs over the 8 instances passed below.
    constexpr uint32_t kRecordsPerSlice = 200000u;
    constexpr uint32_t kSlices          = 3u;
    static_assert(kSlices * kRecordsPerSlice < (1u << 20),
                  "the head-only claim must fit, or the test proves nothing about the tail");
    static_assert(2u * kSlices * kRecordsPerSlice > (1u << 20),
                  "the head+tail claim must NOT fit, or the growth path never engages");
    BatchRegistry registry;
    for (uint32_t i = 0; i < kRecordsPerSlice; ++i)
        registry.OnInstanceAdded(/*materialIndex=*/0u, /*meshIndex=*/0u, /*mirrored=*/false);

    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);
    RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);
    builder->BeginArenaFrame();
    for (uint32_t i = 0; i < kSlices; ++i)
    {
        GPUDrawStreamBuilder::SliceRegistration s{};
        s.viewId                 = 1u + i;
        s.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        s.disableVisibilityCheck = true;
        s.lod.projScaleY         = 1.0f;
        s.lod.crossfadeDuration  = 0.25f;
        builder->RegisterSlice(s);
    }
    const RGBuffer ordering = builder->ScheduleUnifiedScatter(
        frame, "ArenaGrow", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
        sceneRG.Meshes, registry, /*materialDepthClass=*/{}, /*materialColorClass=*/{},
        /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{}, 8u,
        static_cast<uint32_t>(scene->GetMeshes().size()));

    ASSERT_TRUE(ordering.IsValid())
        << "an over-capacity claim skipped the pass instead of resizing the arena";
    for (uint32_t i = 0; i < kSlices; ++i)
    {
        const auto range = builder->FindBatchDrawRange(
            1u + i, GPUDrawStreamBuilder::kCascadeIndexNone, 0u, 0u,
            GPUDrawStreamBuilder::SlicePhase::A);
        ASSERT_TRUE(range.IsValid()) << "slice " << i << " published no consumer range";
        EXPECT_TRUE(range.evenTail.IsPresent())
            << "slice " << i << ": the resized arena must still carry the crossfade tail";
    }

    // The new buffers must be bindable and dispatchable, not merely allocated.
    frame.MarkOutput(ordering);
    frame.Execute();
    dev->WaitForIdle();

    dev->DestroyBuffer(visPhys);
}

// The crossfade tail census (GetLiveTailObservation) is what elision suppression
// lifts on, and a FALSE zero there freezes a record pair permanently. A slice
// whose stats row falls past kStatsSliceCapacity gets no binding, so the reduce
// cannot read its tail cursors at all — for a TAIL-CAPABLE slice that must
// surface as an invalid reading, never as a zero count.
//
// Two arms in one test on purpose. The small arm is the control that proves the
// reduce produces a reading here at all; it also arms the over-capacity arm,
// because a census SURVIVES a reduce that merely could not run (see
// ReduceShadowArcStats). So the second arm only reads invalid if the reduce
// actively invalidated it — an unobservable tail must not ride a stale valid
// reading forward.
TEST(RGGpuDriven, ATailCapableSliceWithNoStatsBindingInvalidatesTheCensus)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);
    auto scene = GPUSceneFactory::CreateSmallScene(dev.get());
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

    // One mesh + two instances on one material, so the batch registry carries a
    // real row: an empty registry publishes no ordering and the call would prove
    // nothing about the census.
    GPUMesh mesh{};
    mesh.indexCount        = 3u;
    mesh.vertexCount       = 3u;
    mesh.lodCount          = 1u;
    mesh.boundingRadius    = 1.0f;
    mesh.lodIndexCount[0]  = 3u;
    mesh.lodIndexOffset[0] = 0u;
    const uint32_t meshIdx = scene->AddMesh(mesh);
    for (uint32_t i = 0; i < 2u; ++i)
    {
        GPUInstance inst{};
        inst.materialIndex  = 1u;
        inst.meshIndex      = meshIdx;
        inst.boundingRadius = 1.0f;
        scene->AddInstance(inst);
    }

    FramePools pools(dev.get());
    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    uint64_t frameIndex = 0;

    // One app frame of `sliceCount` crossfading CAMERA slices, then the next
    // frame's BeginArenaFrame reduces it. `ok` reports whether the frame actually
    // dispatched — an invalid ordering must never reach MarkOutput.
    bool ok = false;
    auto censusAfterAFrameOf = [&](uint32_t sliceCount)
    {
        builder->BeginArenaFrame();
        scene->FlushGPUBuffers();
        frame.BeginFrame(frameIndex++);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);
        for (uint32_t i = 0; i < sliceCount; ++i)
        {
            GPUDrawStreamBuilder::SliceRegistration s{};
            s.viewId                 = 1u + i;
            s.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
            s.disableVisibilityCheck = true;
            s.lod.projScaleY         = 1.0f; // perspective + auto LOD == tail-capable
            s.lod.crossfadeDuration  = 0.25f;
            builder->RegisterSlice(s);
        }
        const RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "Census", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, scene->GetBatchRegistry(), /*materialDepthClass=*/{},
            /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{},
            scene->GetInstanceCount(), static_cast<uint32_t>(scene->GetMeshes().size()));
        ok = ordering.IsValid();
        if (ok)
        {
            frame.MarkOutput(ordering);
            frame.Execute();
            dev->WaitForIdle(); // the reduce below is fence-gated on this
        }
        builder->BeginArenaFrame();
        return builder->GetLiveTailObservation();
    };

    // CONTROL: every slice fits the stats array, so the census is readable.
    const auto small = censusAfterAFrameOf(4u);
    ASSERT_TRUE(ok) << "control frame published no ordering";
    ASSERT_TRUE(small.valid)
        << "control arm produced no reading — the over-capacity arm below would then "
           "pass for the wrong reason; this environment cannot run this test";
    EXPECT_EQ(small.recomputeCount, builder->GetScatterRecomputeCount())
        << "a census must be stamped with the recompute count it covers";

    // The real case: more tail-capable slices than the stats array holds.
    const uint32_t overCapacity = GPUDrawStreamBuilder::kStatsSliceCapacity + 1u;
    const auto over = censusAfterAFrameOf(overCapacity);
    ASSERT_TRUE(ok) << "over-capacity frame published no ordering — it must still "
                       "dispatch; only the ATTRIBUTION is lost";
    EXPECT_FALSE(over.valid)
        << "a tail-capable slice the reduce cannot read must invalidate the whole "
           "census; reporting its tail as 0 would elide the scatter with a live "
           "pair retained and freeze it mid-dissolve";

    dev->DestroyBuffer(visPhys);
}

// ---- Rendered level and phase history (S4) --------------------------------
//
// The scatter keeps a per-(view, instance) record of the level and crossfade
// phase it last RENDERED, for a consumer that pairs this frame's surface with
// the previous frame's. Two properties are builder-side rather than shader-side
// and are driven here end to end: nothing is allocated until a view asks, and
// the two halves swap roles only after a frame that was actually recorded.
namespace
{
GPUMesh MakeRenderedHistoryMesh()
{
    GPUMesh m{};
    m.indexCount        = 30u;
    m.vertexCount       = 3u;
    m.lodCount          = 1u;
    m.boundingRadius    = 1.0f;
    m.lodIndexCount[0]  = 30u;
    m.lodIndexOffset[0] = 0u;
    return m;
}

GPUInstance MakeRenderedHistoryInstance(uint32_t materialIndex, uint32_t meshIndex)
{
    GPUInstance inst{};
    inst.materialIndex  = materialIndex;
    inst.meshIndex      = meshIndex;
    inst.boundingRadius = 1.0f;
    return inst;
}
} // namespace

// The no-consumer promise, measured rather than argued: these buffers are
// created straight on the device, so the render graph's resource list cannot
// report them and the builder's own count is the instrument. A view that does
// not ask holds none, and a view that stops asking gives its pair back.
TEST(RGGpuDriven, RenderedHistoryStaysUnallocatedUntilAViewAsksForIt)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);

    auto scene = std::make_unique<GPUScene>(dev.get());
    ASSERT_TRUE(scene->Initialize(256u, 32u));
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter shaderpkg unavailable";

    const uint32_t mesh = scene->AddMesh(MakeRenderedHistoryMesh());
    scene->AddInstance(MakeRenderedHistoryInstance(1u, mesh));

    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    uint64_t frameIndex = 0;
    auto runFrame = [&](bool requestHistory, bool execute)
    {
        builder->BeginArenaFrame();
        scene->FlushGPUBuffers();
        frame.BeginFrame(frameIndex++);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        GPUDrawStreamBuilder::SliceRegistration slice{};
        slice.viewId                 = 1u;
        slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        slice.disableVisibilityCheck = true;
        slice.lod.projScaleY         = 1.0f;
        slice.lod.cameraPos[0]       = 5.0f;
        slice.lod.renderedLevelHistoryRequested = requestHistory;
        builder->RegisterSlice(slice);

        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "History", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, scene->GetBatchRegistry(), /*materialDepthClass=*/{},
            /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{},
            scene->GetInstanceCount(), static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        if (!execute)
            return; // declared, then abandoned: nothing is recorded or submitted
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
    };

    runFrame(/*requestHistory=*/false, /*execute=*/true);
    runFrame(/*requestHistory=*/false, /*execute=*/true);
    EXPECT_EQ(builder->IntrospectScatter().RenderedHistoryViews, 0u)
        << "a view that does not ask must allocate no history at all";
    EXPECT_EQ(builder->IntrospectScatter().RenderedHistoryRotations, 0u);

    runFrame(/*requestHistory=*/true, /*execute=*/true);
    EXPECT_EQ(builder->IntrospectScatter().RenderedHistoryViews, 1u)
        << "asking allocates exactly one pair, for the asking view";

    runFrame(/*requestHistory=*/false, /*execute=*/true);
    EXPECT_EQ(builder->IntrospectScatter().RenderedHistoryViews, 0u)
        << "a view that stops asking releases its pair rather than holding it";

    builder->Shutdown();
    dev->DestroyBuffer(visPhys);
}

// A sample may only become a later frame's PREVIOUS if the frame that produced
// it was actually submitted. The rotation is what decides that here, and it is
// keyed to the scatter pass's exec closure — which runs from RGFrame::Execute
// and from nowhere else — rather than to the declare that schedules it.
//
// The sequence is its own control: the count advances on the frames around the
// abandoned one, so a flat reading cannot be explained by a dead counter.
TEST(RGGpuDriven, DeclaredButUnsubmittedFrameNeverBecomesTheNextFramesPrevious)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);

    auto scene = std::make_unique<GPUScene>(dev.get());
    ASSERT_TRUE(scene->Initialize(256u, 32u));
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter shaderpkg unavailable";

    const uint32_t mesh = scene->AddMesh(MakeRenderedHistoryMesh());
    scene->AddInstance(MakeRenderedHistoryInstance(1u, mesh));

    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    uint64_t frameIndex = 0;
    auto runFrame = [&](bool execute)
    {
        builder->BeginArenaFrame();
        scene->FlushGPUBuffers();
        frame.BeginFrame(frameIndex++);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        GPUDrawStreamBuilder::SliceRegistration slice{};
        slice.viewId                 = 1u;
        slice.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        slice.disableVisibilityCheck = true;
        slice.lod.projScaleY         = 1.0f;
        slice.lod.cameraPos[0]       = 5.0f;
        slice.lod.renderedLevelHistoryRequested = true;
        builder->RegisterSlice(slice);

        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "History", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, scene->GetBatchRegistry(), /*materialDepthClass=*/{},
            /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{},
            scene->GetInstanceCount(), static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        if (!execute)
            return;
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
    };
    auto rotations = [&] { return builder->IntrospectScatter().RenderedHistoryRotations; };

    runFrame(/*execute=*/true); // first frame: the pair is created, nothing to rotate
    EXPECT_EQ(rotations(), 0u);

    runFrame(/*execute=*/true);
    EXPECT_EQ(rotations(), 1u) << "a recorded frame's buffer becomes the next frame's previous";

    // Declared and abandoned. Its own declare rotates on the RECORDED frame
    // before it, and then it writes nothing.
    runFrame(/*execute=*/false);
    const uint64_t afterAbandoned = rotations();
    EXPECT_EQ(afterAbandoned, 2u);

    runFrame(/*execute=*/true);
    EXPECT_EQ(rotations(), afterAbandoned)
        << "the abandoned frame recorded nothing, so its buffer must not become "
           "this frame's previous — the last SUBMITTED frame's stays";

    runFrame(/*execute=*/true);
    EXPECT_EQ(rotations(), afterAbandoned + 1u)
        << "and the rotation resumes on the frame after a recorded one, so the "
           "flat step above is the gate rather than a dead counter";

    builder->Shutdown();
    dev->DestroyBuffer(visPhys);
}

// The request travels on ViewLODParams, which is per slice, while the pair is
// per view — and one view registers several camera slices in a frame (the two
// culling phases). A view keeps its pair while ANY camera slice of the frame
// asks for it: a sibling slice that does not ask must neither retire the pair
// the asking slice just bound nor force it to be recreated every frame.
TEST(RGGpuDriven, RenderedHistorySurvivesASiblingSliceThatDoesNotAsk)
{
    RG_REQUIRE_DEVICE(dev);
    GPUScene::SetCullingShaderLoader(&TestShaderLoader);
    GPUDrawStreamBuilder::SetShaderLoader(&TestShaderLoader);

    auto scene = std::make_unique<GPUScene>(dev.get());
    ASSERT_TRUE(scene->Initialize(256u, 32u));
    auto builder = std::make_unique<GPUDrawStreamBuilder>(dev.get());
    ASSERT_TRUE(builder->Initialize());
    if (!builder->GetOrCreateScatterPipeline().IsValid())
        GTEST_SKIP() << "draw_command_scatter shaderpkg unavailable";

    const uint32_t mesh = scene->AddMesh(MakeRenderedHistoryMesh());
    scene->AddInstance(MakeRenderedHistoryInstance(1u, mesh));

    BufferDesc vd;
    vd.size  = 4096;
    vd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle visPhys = dev->CreateBuffer(vd);
    ASSERT_TRUE(visPhys.IsValid());

    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    uint64_t frameIndex = 0;
    // Two camera slices of one view per frame. The non-asking sibling is
    // registered FIRST so a release-on-first-sight implementation would either
    // retire the pair before the asking slice binds it or recreate it fresh.
    auto runFrame = [&](bool siblingAsks)
    {
        builder->BeginArenaFrame();
        scene->FlushGPUBuffers();
        frame.BeginFrame(frameIndex++);
        RGBuffer vis = frame.ImportExternalBuffer("Vis", visPhys);
        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame);

        GPUDrawStreamBuilder::SliceRegistration sibling{};
        sibling.viewId                 = 1u;
        sibling.cascadeIndex           = GPUDrawStreamBuilder::kCascadeIndexNone;
        sibling.disableVisibilityCheck = true;
        sibling.lod.projScaleY         = 1.0f;
        sibling.lod.cameraPos[0]       = 5.0f;
        sibling.lod.renderedLevelHistoryRequested = siblingAsks;
        builder->RegisterSlice(sibling);
        GPUDrawStreamBuilder::SliceRegistration asking = sibling;
        asking.lod.renderedLevelHistoryRequested = true;
        builder->RegisterSlice(asking);

        RGBuffer ordering = builder->ScheduleUnifiedScatter(
            frame, "History", /*rgPassPhase=*/0, vis, sceneRG.Instances, sceneRG.ScatterHot,
            sceneRG.Meshes, scene->GetBatchRegistry(), /*materialDepthClass=*/{},
            /*materialColorClass=*/{}, /*meshPoolOrdered=*/{}, /*orderedToGroup=*/{},
            scene->GetInstanceCount(), static_cast<uint32_t>(scene->GetMeshes().size()));
        EXPECT_TRUE(ordering.IsValid());
        frame.MarkOutput(ordering);
        frame.Execute();
        dev->WaitForIdle();
    };

    runFrame(/*siblingAsks=*/true);
    ASSERT_EQ(builder->IntrospectScatter().RenderedHistoryViews, 1u);
    ASSERT_EQ(builder->IntrospectScatter().RenderedHistoryRotations, 0u);

    runFrame(/*siblingAsks=*/false);
    EXPECT_EQ(builder->IntrospectScatter().RenderedHistoryViews, 1u)
        << "a sibling slice that does not ask must not retire the view's pair";
    EXPECT_EQ(builder->IntrospectScatter().RenderedHistoryRotations, 1u)
        << "the pair the asking slice bound is the one created last frame, rotated once — "
           "a recreated pair would have nothing to rotate";

    builder->Shutdown();
    dev->DestroyBuffer(visPhys);
}
