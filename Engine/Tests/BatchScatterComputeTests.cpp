// GPU-executed tests for draw_command_scatter.comp (R1.4): scatter a known
// GPUScene through the real pipeline and verify per-batch cursors, record
// regions, indirection targets, and the arena-base push constants against
// CPU expectations. This is the P2 gate from the scatter-bucketer design doc
// (§5): the shader must route every live instance into exactly the region the
// CPU-side BuildBatchTable sized for it, with zero tripwire hits.

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshLODThresholds.h"
#include "Rendering/Common/MatrixUtils.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/NoneCullingStrategy.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

constexpr uint32_t kDrawRecordUints = 5u; // VkDrawIndexedIndirectCommand

// Binding 7 is a per-slice array now (P0 shadow-arc). These tests dispatch a
// single slice with statsBase = 0, so they use row 0. Layout MUST match
// draw_command_scatter.comp's ScatterSliceStats (16 B).
struct ScatterSliceStats
{
    uint32_t tableMisses;
    uint32_t overflows;
    uint32_t drawnTriangles;
    uint32_t reserved;
};

// Mirror of draw_command_scatter.comp's binding-11 uvec2 fade state. `.Levels`
// packs fromLod | (toLod << kLodFadeToLevelShift); `.StartSeconds` is the stamp
// the weight is derived from. Held as a float on the CPU side because the shader
// stores floatBitsToUint of exactly this value.
struct LodFadeState
{
    uint32_t Levels = 0u;
    float    StartSeconds = 0.0f;

    bool operator==(const LodFadeState& o) const
    {
        // Bit compare: the whole point of the idempotency test is that a re-run
        // rewrites the SAME bytes, which a float tolerance would hide.
        return Levels == o.Levels
               && std::memcmp(&StartSeconds, &o.StartSeconds, sizeof(float)) == 0;
    }
};
static_assert(sizeof(LodFadeState) == 8, "must match the shader's uvec2 lodFade[]");

// One rendered level and phase entry as draw_command_scatter.comp packs it
// (bindings 12/13): the selected level in the low byte, the fading bit set when
// the invocation emitted the crossfade PAIR, the continuous bit set when the
// previous frame's entry existed and named the same level and fading state.
uint32_t MakeRenderedEntry(uint32_t level, bool fading, bool continuous)
{
    return (level & GPUDrawStreamBuilder::kRenderedHistoryLevelMask)
           | (fading ? GPUDrawStreamBuilder::kRenderedHistoryFadingBit : 0u)
           | (continuous ? GPUDrawStreamBuilder::kRenderedHistoryContinuousBit : 0u);
}

// A scatter pipeline variant: the binding-1 fetch axis (compact 32 B mirror vs
// full-fat 240 B instance buffer) crossed with the instrumentation axis (the
// per-slice drawnTriangles subgroup atomic, GE_SCATTER_STATS). Field names
// match the GE_SCATTER_COMPACT / GE_SCATTER_STATS env knobs.
struct ScatterVariant
{
    bool compact;
    bool stats;
};

/// Inputs to ge_SelectLOD's auto-select scan. The default projScaleY of 0
/// makes the shader return LOD0 before the scan runs, which every test
/// that pins a level with forceLod relies on — only tests that opt in here
/// exercise the threshold comparison.
///
/// Namespace scope, not a member: a nested type whose members have default
/// initializers cannot be used as `= {}` on a default argument inside the
/// enclosing class definition.
struct LodDispatchParams
{
    float projScaleY = 0.0f;
    float cameraPos[3] = {0.0f, 0.0f, 0.0f};
    float sseThresholdToCoverage = 0.0f;
    float sseThresholdToCoverageTight = 0.0f;
    // Dithered crossfade. 0 (the default) leaves the shader's early return in
    // place, so every pre-existing expectation in this file is untouched.
    float crossfadeDuration = 0.0f;
    float nowSeconds = 0.0f;
    // Crossfade TAIL region bases, inside the same cursor/record buffers
    // this dispatch allocates: one extra cursor per row at tailCursorBase,
    // and a record block at tailRecordBase that reuses the head table's
    // per-row recordOffset/capacity. Read only when crossfadeDuration > 0.
    uint32_t tailCursorBase = 0u;
    uint32_t tailRecordBase = 0u;
    // Dwell band. > 0 engages the band against the fixture's persistent
    // prevLod buffer, so consecutive Dispatch calls form a history the way
    // frames do.
    float hysteresisBand = 0.0f;
    float smallCullCoverage = 0.0f;
    // Rendered level and phase history (bindings 12/13). false (the default)
    // leaves the shader's gate at 0, so it touches neither binding and every
    // pre-existing expectation in this file is unaffected by it.
    bool renderedHistory = false;
    // Per-instance visibility for this dispatch, one word per instance. Empty
    // (the default) keeps the pass-through the rest of this file relies on;
    // non-empty models a real culling GENERATION — the world scatter's phase A
    // and phase B run over disjoint visibility slices of one frame.
    std::vector<uint32_t> visibility;
};

// Parametrized over the scatter package: {fetch} × {stats}. Every body runs on
// the shipping package; the bodies that can tell the other three apart also run
// on them (the parity suites at the end of this file), asserting the same
// record/cursor/indirection/tripwire expectations, so the stats axis is shown to
// add only the drawnTriangles accumulation and never to perturb a record. The
// stats-on cases additionally assert drawnTriangles accumulates; the stats-off
// cases assert it stays 0.
class BatchScatterComputeTest : public ::testing::TestWithParam<ScatterVariant>
{
  protected:
    // true = compact 32 B mirror at binding 1; false = full-fat 240 B buffer.
    bool UseCompact() const { return GetParam().compact; }
    // true = the drawnTriangles subgroup atomic is compiled in (GE_SCATTER_STATS).
    bool UseStats() const { return GetParam().stats; }

    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
        {
            GTEST_SKIP() << "No Vulkan device available";
        }
        m_Scene = std::make_unique<GPUScene>(m_Device.get());
        ASSERT_TRUE(m_Scene->Initialize(256u, 32u));

        m_Builder = std::make_unique<GPUDrawStreamBuilder>(m_Device.get());
        ASSERT_TRUE(m_Builder->Initialize());

        // The compact variant binds GPUScene's 32 B mirror, which only exists
        // when GE_SCATTER_COMPACT is on (the default). If the process forced it
        // off, skip the compact param; the full-fat param still runs.
        if (UseCompact() && !m_Scene->IsScatterHotEnabled())
        {
            GTEST_SKIP() << "compact variant needs the scatter-hot mirror "
                            "(GE_SCATTER_COMPACT=0 disables it)";
        }

        // Build the requested variant explicitly (not via GetOrCreateScatterPipeline,
        // which is gated by the env-cached selection) so all four run in one process.
        m_Pipeline = m_Builder->CreateScatterPipelineForVariant(UseCompact(), UseStats());
        if (!m_Pipeline.IsValid())
        {
            FAIL() << "scatter shaderpkg not available for variant (compact="
                         << UseCompact() << " stats=" << UseStats() << ")";
        }
    }

    void TearDown() override
    {
        m_Culling.reset();
        m_CullFrame.reset();
        m_CullRing.reset();
        m_CullTransient.reset();
        m_CullPersistent.reset();
        for (BufferHandle& b : m_OwnedBuffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        m_OwnedBuffers.clear();
        m_LodFade      = BufferHandle{};
        m_LodFadeCount = 0;
        m_PrevLod      = BufferHandle{};
        m_PrevLodCount = 0;
        m_Rendered[0]  = BufferHandle{};
        m_Rendered[1]  = BufferHandle{};
        m_RenderedCount = 0;
        m_RenderedCurrentIndex = 0;
        if (m_Builder)
            m_Builder->Shutdown();
        m_Builder.reset();
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    uint32_t AddMeshRow(uint32_t indexCount)
    {
        GPUMesh mesh{};
        mesh.indexCount     = indexCount;
        mesh.indexOffset    = 0u;
        mesh.vertexOffset   = 0u;
        mesh.vertexCount    = 3u;
        mesh.lodCount       = 1u;
        mesh.lodIndexCount[0]  = indexCount;
        mesh.lodIndexOffset[0] = 0u;
        mesh.boundingRadius = 1.0f;
        return m_Scene->AddMesh(mesh);
    }

    GPUInstance MakeInstance(uint32_t materialIndex, uint32_t meshIndex, uint32_t flags = 0u)
    {
        GPUInstance instance{};
        instance.materialIndex  = materialIndex;
        instance.meshIndex      = meshIndex;
        instance.flags          = flags;
        instance.boundingRadius = 1.0f;
        return instance;
    }

    BufferHandle MakeStorageBuffer(size_t bytes, const char* name, bool indirect = false)
    {
        BufferDesc desc{};
        desc.size  = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage)
                   | static_cast<uint32_t>(BufferUsage::TransferDst)
                   | (indirect ? static_cast<uint32_t>(BufferUsage::Indirect) : 0u);
        // Readback, not Upload: the test reads these back on the CPU, and
        // Readback is the class that asks for a host-cached type.
        desc.memoryUsage = BufferMemoryUsage::Readback;
        desc.debugName   = name;
        BufferHandle h = m_Device->CreateBuffer(desc);
        m_OwnedBuffers.push_back(h);
        return h;
    }

    void WriteBuffer(BufferHandle h, const void* data, size_t bytes)
    {
        void* mapped = m_Device->MapBuffer(h);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, data, bytes);
        m_Device->UnmapBuffer(h);
    }

    template <typename T>
    std::vector<T> ReadBuffer(BufferHandle h, size_t count)
    {
        std::vector<T> out(count);
        void* mapped = m_Device->MapBuffer(h);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(out.data(), mapped, count * sizeof(T));
        m_Device->UnmapBuffer(h);
        return out;
    }

    /// Create (or resize) the persistent binding-10 history buffer and seed
    /// every entry to ONE value. Tests call this to establish a known previous
    /// frame; the buffer survives across Dispatch calls until the next
    /// SeedPrevLod. A uniform seed cannot observe WHICH index the shader reads:
    /// prevLod[instanceIndex], [slot], [batchId], [localSlot] and
    /// [gl_LocalInvocationID.x] all return the same value. Use the per-index
    /// overload for anything that depends on the index expression.
    void SeedPrevLod(uint32_t instanceCount, uint32_t value)
    {
        SeedPrevLod(std::vector<uint32_t>(std::max(instanceCount, 1u), value));
    }

    /// Seed a DISTINCT history value per instance — the only way to pin the
    /// per-(instance, view) property the dwell band exists for.
    void SeedPrevLod(const std::vector<uint32_t>& values)
    {
        m_PrevLodCount = static_cast<uint32_t>(std::max<size_t>(values.size(), 1u));
        m_PrevLod = MakeStorageBuffer(m_PrevLodCount * sizeof(uint32_t), "Test.PrevLod");
        std::vector<uint32_t> seed = values;
        seed.resize(m_PrevLodCount, GPUDrawStreamBuilder::kLodNoHistory);
        WriteBuffer(m_PrevLod, seed.data(), m_PrevLodCount * sizeof(uint32_t));
    }

    // Dispatch the scatter over the scene's current GPU buffers with the
    // given table + arena bases; returns cursors/records/indirection/stats.
    struct DispatchResult
    {
        BufferHandle cursorBuffer{}, recordBuffer{}, indirectionBuffer{};
        std::vector<uint32_t> cursors;
        std::vector<uint32_t> records;     // raw uints, 5 per draw record
        std::vector<uint32_t> indirection;
        ScatterSliceStats stats{}; // row 0 (this dispatch uses statsBase = 0)
        std::vector<LodFadeState> lodFade; // binding 11 AFTER the dispatch
        std::vector<uint32_t> prevLod;     // binding 10 AFTER the dispatch
        // The rendered level and phase pair AFTER the dispatch. renderedPrevious
        // is what a later dispatch of the same frame reads, so a test asserting
        // it unchanged is asserting the immutability the pair exists for.
        std::vector<uint32_t> renderedPrevious; // binding 12
        std::vector<uint32_t> renderedCurrent;  // binding 13
    };

    /// Create (or resize) the persistent binding-11 fade-state buffer and seed
    /// every entry to ONE state. The buffer survives across Dispatch calls, so
    /// consecutive calls form a transition history the way frames do. All-zero
    /// is the engine's first-touch state: fromLod == toLod == 0, not fading.
    void SeedLodFade(uint32_t instanceCount, uint32_t packedLevels, float startSeconds)
    {
        SeedLodFade(std::vector<LodFadeState>(std::max(instanceCount, 1u),
                                              {packedLevels, startSeconds}));
    }

    /// Seed a DISTINCT state per instance — the only way to pin the
    /// per-(instance, view) property the fade buffer exists for.
    void SeedLodFade(const std::vector<LodFadeState>& states)
    {
        m_LodFadeCount = static_cast<uint32_t>(std::max<size_t>(states.size(), 1u));
        m_LodFade = MakeStorageBuffer(m_LodFadeCount * sizeof(LodFadeState), "Test.LodFade");
        std::vector<LodFadeState> seed = states;
        seed.resize(m_LodFadeCount, LodFadeState{});
        WriteBuffer(m_LodFade, seed.data(), m_LodFadeCount * sizeof(LodFadeState));
    }

    /// Create the persistent rendered level and phase PAIR and seed the
    /// previous half per instance; the current half starts at no-history, the
    /// state the builder's per-frame clear leaves. The pair survives across
    /// Dispatch calls, so several dispatches form ONE frame the way the world
    /// scatter's two phases and a later consumer dispatch do.
    void SeedRenderedHistory(const std::vector<uint32_t>& previous)
    {
        m_RenderedCount = static_cast<uint32_t>(std::max<size_t>(previous.size(), 1u));
        for (uint32_t i = 0; i < 2u; ++i)
            m_Rendered[i] = MakeStorageBuffer(m_RenderedCount * sizeof(uint32_t),
                                              i == 0 ? "Test.RenderedA" : "Test.RenderedB");
        m_RenderedCurrentIndex = 0u;
        std::vector<uint32_t> seed = previous;
        seed.resize(m_RenderedCount, GPUDrawStreamBuilder::kLodNoHistory);
        WriteBuffer(m_Rendered[1u - m_RenderedCurrentIndex], seed.data(),
                    m_RenderedCount * sizeof(uint32_t));
        ClearRenderedCurrent();
    }

    /// What the builder does once per RECORDED frame: the buffer this frame
    /// wrote becomes the next frame's previous, and the new current is cleared
    /// to no-history so an instance that draws nothing leaves that value.
    void RotateRenderedHistory()
    {
        m_RenderedCurrentIndex = 1u - m_RenderedCurrentIndex;
        ClearRenderedCurrent();
    }

    void ClearRenderedCurrent()
    {
        const std::vector<uint32_t> clear(m_RenderedCount, GPUDrawStreamBuilder::kLodNoHistory);
        WriteBuffer(m_Rendered[m_RenderedCurrentIndex], clear.data(),
                    m_RenderedCount * sizeof(uint32_t));
    }

    DispatchResult Dispatch(const std::vector<GPUDrawStreamBuilder::BatchTableEntry>& table,
                            uint32_t totalRecords, uint32_t cursorBase, uint32_t recordBase,
                            uint32_t cursorSlots, uint32_t recordSlots,
                            uint32_t classMode = 0u,
                            const std::vector<uint32_t>& materialColorClass = {},
                            uint32_t forceLod = 0xFFFFFFFFu,
                            const std::vector<uint32_t>& meshPoolGroup = {},
                            uint32_t meshGroupMode = 0u,
                            const LodDispatchParams& lod = {},
                            bool cull = false, uint32_t layerMask = 0xFFFFFFFFu,
                            bool shadowCull = false, bool fusedCull = false)
    {
        m_Scene->FlushGPUBuffers();

        const uint32_t instanceCount = m_Scene->GetInstanceCount();
        const uint32_t frameIndex    = m_Scene->GetFrameIndex();

        BufferHandle visibility = MakeStorageBuffer(
            std::max<size_t>(instanceCount, 1u) * sizeof(uint32_t), "Test.Visibility");
        if (!lod.visibility.empty())
        {
            std::vector<uint32_t> visible = lod.visibility;
            visible.resize(std::max<size_t>(instanceCount, 1u), 0u);
            WriteBuffer(visibility, visible.data(), visible.size() * sizeof(uint32_t));
        }
        BufferHandle tableBuf = MakeStorageBuffer(
            std::max<size_t>(table.size(), 1u) * sizeof(GPUDrawStreamBuilder::BatchTableEntry),
            "Test.BatchTable");
        if (!table.empty())
            WriteBuffer(tableBuf, table.data(),
                        table.size() * sizeof(GPUDrawStreamBuilder::BatchTableEntry));

        BufferHandle cursors = MakeStorageBuffer(cursorSlots * sizeof(uint32_t),
                                                 "Test.Cursors", /*indirect=*/true);
        const std::vector<uint32_t> zeroCursors(cursorSlots, 0u);
        WriteBuffer(cursors, zeroCursors.data(), cursorSlots * sizeof(uint32_t));

        BufferHandle records = MakeStorageBuffer(
            recordSlots * kDrawRecordUints * sizeof(uint32_t), "Test.Records", true);
        const std::vector<uint32_t> zeroRecords(recordSlots * kDrawRecordUints, 0u);
        WriteBuffer(records, zeroRecords.data(), zeroRecords.size() * sizeof(uint32_t));

        BufferHandle indirection = MakeStorageBuffer(recordSlots * sizeof(uint32_t),
                                                     "Test.Indirection");
        const std::vector<uint32_t> zeroIndirection(recordSlots, 0xDEADBEEFu);
        WriteBuffer(indirection, zeroIndirection.data(), recordSlots * sizeof(uint32_t));

        BufferHandle stats = MakeStorageBuffer(sizeof(ScatterSliceStats), "Test.Stats");
        const ScatterSliceStats zeroStats{};
        WriteBuffer(stats, &zeroStats, sizeof(zeroStats));

        // materialColorClass (binding 8, P2). Always bound so the layout is
        // valid; contents matter only for classMode == kClassModeColor.
        const uint32_t classCount = std::max<uint32_t>(
            static_cast<uint32_t>(materialColorClass.size()), 1u);
        BufferHandle colorClass = MakeStorageBuffer(classCount * sizeof(uint32_t),
                                                    "Test.MaterialColorClass");
        std::vector<uint32_t> classData = materialColorClass;
        classData.resize(classCount, 0u);
        WriteBuffer(colorClass, classData.data(), classCount * sizeof(uint32_t));

        // meshPoolGroup (binding 9, draw consolidation). Always bound so the
        // 10-binding layout is valid; contents matter only when
        // meshGroupMode != 0.
        const uint32_t groupCount = std::max<uint32_t>(
            static_cast<uint32_t>(meshPoolGroup.size()), 1u);
        BufferHandle poolGroup = MakeStorageBuffer(groupCount * sizeof(uint32_t),
                                                   "Test.MeshPoolGroup");
        std::vector<uint32_t> groupData = meshPoolGroup;
        groupData.resize(groupCount, 0u);
        WriteBuffer(poolGroup, groupData.data(), groupCount * sizeof(uint32_t));

        if (cull)
            CullInto(visibility, layerMask, shadowCull, fusedCull);

        PipelineHandle pipeline = m_Pipeline;

        DescriptorSetDesc setDesc{};
        setDesc.layout    = GPUDrawStreamBuilder::MakeScatterDescriptorSetLayout();
        setDesc.transient = true;
        setDesc.debugName = "Test.ScatterDS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(setDesc);

        m_Device->UpdateStorageBufferBinding(ds, 0, visibility, 0,
                                             std::max<size_t>(instanceCount, 1u) * sizeof(uint32_t));
        // Binding 1 matches the pipeline variant under test: the coalesced 32 B
        // mirror for the compact param, else the full 240 B instance buffer.
        // GPUScene maintains + uploads the mirror on the same mutators, so the
        // FlushGPUBuffers above populated it.
        if (UseCompact())
            m_Device->UpdateStorageBufferBinding(
                ds, 1, m_Scene->GetScatterHotBufferForFrame(frameIndex), 0,
                std::max<size_t>(instanceCount, 1u) * sizeof(GPUInstanceScatterHot));
        else
            m_Device->UpdateStorageBufferBinding(
                ds, 1, m_Scene->GetInstanceBufferForFrame(frameIndex), 0,
                std::max<size_t>(instanceCount, 1u) * sizeof(GPUInstance));
        m_Device->UpdateStorageBufferBinding(ds, 2, m_Scene->GetMeshBuffer(), 0,
                                             std::max<size_t>(m_Scene->GetMeshes().size(), size_t(1)) * sizeof(GPUMesh));
        m_Device->UpdateStorageBufferBinding(ds, 3, tableBuf, 0,
                                             std::max<size_t>(table.size(), 1u) * sizeof(GPUDrawStreamBuilder::BatchTableEntry));
        m_Device->UpdateStorageBufferBinding(ds, 4, records, 0,
                                             recordSlots * kDrawRecordUints * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(ds, 5, cursors, 0, cursorSlots * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(ds, 6, indirection, 0, recordSlots * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(ds, 7, stats, 0, sizeof(ScatterSliceStats));
        m_Device->UpdateStorageBufferBinding(ds, 8, colorClass, 0,
                                             classCount * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(ds, 9, poolGroup, 0,
                                             groupCount * sizeof(uint32_t));
        // Binding 10 (dwell-band history). Bound on every dispatch so the
        // 12-binding layout is valid; only read/written when the band is > 0.
        // Tests that want history call SeedPrevLod first, so the buffer persists
        // across dispatches exactly as the per-view buffer persists across frames.
        const uint32_t prevLodCount = std::max(instanceCount, 1u);
        // Re-seed when absent OR too small for this dispatch, so a test that
        // seeded for one instance cannot bind past the end after adding more.
        if (!m_PrevLod.IsValid() || m_PrevLodCount < prevLodCount)
            SeedPrevLod(prevLodCount, GPUDrawStreamBuilder::kLodNoHistory);
        m_Device->UpdateStorageBufferBinding(ds, 10, m_PrevLod, 0,
                                             prevLodCount * sizeof(uint32_t));
        // Binding 11 (crossfade state). Bound on every dispatch so the layout is
        // valid; only read/written when crossfadeDuration > 0. Tests that want a
        // history call SeedLodFade first, so the buffer persists across
        // dispatches exactly as the per-view buffer persists across frames.
        const uint32_t lodFadeCount = std::max(instanceCount, 1u);
        // Re-seed when absent OR too small, so a test that seeded for one
        // instance cannot bind past the end after adding more.
        if (!m_LodFade.IsValid() || m_LodFadeCount < lodFadeCount)
            SeedLodFade(lodFadeCount, 0u, 0.0f);
        m_Device->UpdateStorageBufferBinding(ds, 11, m_LodFade, 0,
                                             lodFadeCount * sizeof(LodFadeState));
        // Bindings 12/13 (rendered level and phase). Bound on every dispatch so
        // the 14-binding layout is valid; read and written only when the gate
        // below is on. Tests that want a history call SeedRenderedHistory first.
        const uint32_t renderedCount = std::max(instanceCount, 1u);
        if (!m_Rendered[0].IsValid() || m_RenderedCount < renderedCount)
            SeedRenderedHistory(
                std::vector<uint32_t>(renderedCount, GPUDrawStreamBuilder::kLodNoHistory));
        m_Device->UpdateStorageBufferBinding(ds, 12, m_Rendered[1u - m_RenderedCurrentIndex], 0,
                                             renderedCount * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(ds, 13, m_Rendered[m_RenderedCurrentIndex], 0,
                                             renderedCount * sizeof(uint32_t));

        GPUDrawStreamBuilder::ScatterPushConstants pc{};
        pc.instanceCount   = instanceCount;
        pc.batchCount      = static_cast<uint32_t>(table.size());
        pc.cursorBase      = cursorBase;
        pc.recordBase      = recordBase;
        // Pass-through unless the case seeded a visibility slice of its own,
        // either from the CPU or from the production cull.
        pc.disableVisCheck = (cull || !lod.visibility.empty()) ? 0u : 1u;
        pc.viewWorldKey    = 0u;
        pc.forceLod        = forceLod;
        pc.classMode       = classMode;
        pc.statsBase       = 0u; // single-slice dispatch → row 0
        pc.meshGroupMode   = meshGroupMode;
        pc.projScaleY      = lod.projScaleY;
        pc.cameraPosX      = lod.cameraPos[0];
        pc.cameraPosY      = lod.cameraPos[1];
        pc.cameraPosZ      = lod.cameraPos[2];
        pc.sseThresholdToCoverage      = lod.sseThresholdToCoverage;
        pc.sseThresholdToCoverageTight = lod.sseThresholdToCoverageTight;
        pc.nowSeconds       = lod.nowSeconds;
        pc.invFadeDuration  = lod.crossfadeDuration > 0.0f ? 1.0f / lod.crossfadeDuration : 0.0f;
        pc.crossfadeTailActive = lod.crossfadeDuration > 0.0f ? 1u : 0u;
        pc.tailCursorBase      = lod.tailCursorBase;
        pc.tailRecordBase      = lod.tailRecordBase;
        pc.lodHysteresisBand = lod.hysteresisBand;
        pc.smallCullCoverage = lod.smallCullCoverage;
        pc.renderedHistoryEnabled = lod.renderedHistory ? 1u : 0u;

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(pipeline);
        cl->BindDescriptorSet(0, ds, pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((instanceCount + 63u) / 64u, 1, 1);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        DispatchResult r;
        r.cursorBuffer = cursors;
        r.recordBuffer = records;
        r.indirectionBuffer = indirection;
        r.cursors     = ReadBuffer<uint32_t>(cursors, cursorSlots);
        r.records     = ReadBuffer<uint32_t>(records, recordSlots * kDrawRecordUints);
        r.indirection = ReadBuffer<uint32_t>(indirection, recordSlots);
        auto statsVec = ReadBuffer<uint32_t>(stats, 4);
        r.stats       = {statsVec[0], statsVec[1], statsVec[2], statsVec[3]};
        r.lodFade     = ReadBuffer<LodFadeState>(m_LodFade, lodFadeCount);
        r.prevLod     = ReadBuffer<uint32_t>(m_PrevLod, prevLodCount);
        r.renderedPrevious =
            ReadBuffer<uint32_t>(m_Rendered[1u - m_RenderedCurrentIndex], renderedCount);
        r.renderedCurrent =
            ReadBuffer<uint32_t>(m_Rendered[m_RenderedCurrentIndex], renderedCount);
        (void)totalRecords;
        return r;
    }

    void CheckRaster(const DispatchResult& draws, const std::set<uint32_t>& visible, bool shadow)
    {
        constexpr uint32_t width = 100, height = 20;
        const auto format = shadow ? TextureFormat::D32_FLOAT : TextureFormat::RGBA8_UNORM;
        TextureDesc td{};
        td.width = width;
        td.height = height;
        td.format = static_cast<uint32_t>(format);
        td.usage = static_cast<uint32_t>(TextureUsage::TransferSrc) |
                   static_cast<uint32_t>(shadow ? TextureUsage::DepthStencil : TextureUsage::RenderTarget);
        const auto target = m_Device->CreateTexture(td);
        ASSERT_TRUE(target.IsValid());
        DescriptorSetLayoutDesc layout{};
        for (uint32_t binding = 0; binding < 2; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1;
            b.shaderStages = kShaderStageVertex;
            layout.bindings.push_back(b);
        }
        PipelineDesc pd{};
        pd.vertexShader = Utils::LoadShaderFile("Shaders/render_layer_probe.vert.spv");
        ASSERT_FALSE(pd.vertexShader.empty());
        if (!shadow)
        {
            pd.pixelShader = Utils::LoadShaderFile("Shaders/render_layer_probe.frag.spv");
            ASSERT_FALSE(pd.pixelShader.empty());
            pd.colorAttachmentFormats = {static_cast<uint32_t>(format)};
            pd.colorBlendState.attachments.resize(1);
        }
        else
        {
            pd.depthAttachmentFormat = static_cast<uint32_t>(format);
            pd.depthStencilState.depthTestEnable = true;
            pd.depthStencilState.depthWriteEnable = true;
        }
        pd.rasterizationState.cullMode = CullModeFlagBits::None;
        pd.descriptorSetLayouts = {layout};
        auto pipeline = m_Device->CreatePipeline(pd);
        ASSERT_TRUE(pipeline.IsValid());
        DescriptorSetDesc sd{};
        sd.layout = layout;
        sd.transient = true;
        auto set = m_Device->CreateDescriptorSet(sd);
        m_Device->UpdateStorageBufferBinding(set, 0,
                                             m_Scene->GetInstanceBufferForFrame(m_Scene->GetFrameIndex()), 0, 5 * sizeof(GPUInstance));
        m_Device->UpdateStorageBufferBinding(set, 1, draws.indirectionBuffer, 0, 5 * sizeof(uint32_t));
        BufferDesc ib{};
        ib.size = 3 * sizeof(uint32_t);
        ib.usage = static_cast<uint32_t>(BufferUsage::Index);
        ib.memoryUsage = BufferMemoryUsage::Upload;
        auto indices = m_Device->CreateBuffer(ib);
        m_OwnedBuffers.push_back(indices);
        const uint32_t tri[] = {0, 1, 2};
        WriteBuffer(indices, tri, sizeof(tri));
        const auto readback = m_Device->CreateReadbackBuffer(width * height * 4);
        m_OwnedBuffers.push_back(readback);
        auto cmd = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cmd->Begin();
        cmd->Barrier(ResourceBarrier::CreateBufferBarrier(draws.recordBuffer, ResourceState::UnorderedAccess, ResourceState::IndirectArgs));
        cmd->Barrier(ResourceBarrier::CreateBufferBarrier(draws.cursorBuffer, ResourceState::UnorderedAccess, ResourceState::IndirectArgs));
        cmd->Barrier(ResourceBarrier::CreateBufferBarrier(draws.indirectionBuffer, ResourceState::UnorderedAccess, ResourceState::ShaderResource));
        const auto state = shadow ? ResourceState::DepthWrite : ResourceState::RenderTarget;
        cmd->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined, state));
        RenderPassDesc pass{};
        if (shadow)
        {
            pass.depthTarget = target;
            pass.clearDepth = true;
            pass.clearDepthValue = 0;
            pass.depthStoreOp = RenderPassDesc::StoreOp::Store;
        }
        else
        {
            pass.colorTargets[0] = target;
            pass.colorTargetCount = 1;
            pass.clearColor[0] = true;
            pass.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        }
        cmd->BeginRenderPass(pass);
        cmd->SetPipeline(pipeline);
        cmd->BindDescriptorSet(0, set, pipeline);
        cmd->SetViewport(0, 0, width, height);
        cmd->SetScissor(0, 0, width, height);
        cmd->SetIndexBuffer(indices, IndexType::Uint32);
        cmd->DrawIndexedIndirectCount(draws.recordBuffer, draws.cursorBuffer, 5, 5 * sizeof(uint32_t));
        cmd->EndRenderPass();
        cmd->Barrier(ResourceBarrier::CreateTextureBarrier(target, state, ResourceState::CopySource));
        cmd->CopyTextureToBuffer(target, readback, width, height);
        cmd->End();
        m_Device->ExecuteCommandLists({cmd.get()});
        m_Device->WaitForIdle();
        const auto* bytes = static_cast<const uint8_t*>(m_Device->MapBuffer(readback));
        ASSERT_NE(bytes, nullptr);
        for (uint32_t i = 0; i < 5; ++i)
        {
            const size_t offset = ((height / 2) * width + 10 + i * 20) * 4;
            float depth = 0;
            std::memcpy(&depth, bytes + offset, sizeof(depth));
            const bool painted = shadow ? depth > .1f : bytes[offset] > 0;
            EXPECT_EQ(painted, visible.contains(i)) << "raster shadow=" << shadow << " instance=" << i;
        }
        m_Device->UnmapBuffer(readback);
        m_Device->DestroyPipeline(pipeline);
        m_Device->DestroyTexture(target);
    }

    // Actual production RG cull feeds the scatter buffer without a CPU visibility
    // substitute. Instances deliberately share mesh/material so batch membership
    // alone cannot exclude a masked instance.
    void CullInto(BufferHandle destination, uint32_t mask, bool shadow, bool fused, bool noSpatial = false)
    {
        namespace RG = GameEngine::Rendering::RenderGraph;
        GPUScene::SetCullingShaderLoader([](const char* name)
                                         { return Utils::LoadShaderFile(name); });
        if (!m_Culling)
        {
            m_CullPersistent = std::make_unique<RG::RGResourcePool>(m_Device.get());
            m_CullTransient = std::make_unique<RG::RGTransientPool>(m_Device.get());
            m_CullRing = std::make_unique<RG::RGUploadRing>(m_Device.get(), 2, 64 * 1024);
            m_CullFrame = std::make_unique<RG::RGFrame>(m_Device.get(), m_CullPersistent.get(), m_CullTransient.get(), m_CullRing.get());
            m_Culling = GPUCullingFactory::CreateBalanced(m_Device.get());
            ElisionFrameContext context{};
            context.AllowElision = true;
            m_Culling->SetElisionFrameContext(context);
        }
        auto& frame = *m_CullFrame;
        auto& culling = m_Culling;
        frame.BeginFrame(++m_CullFrameNumber);
        culling->BeginFrame(&frame, m_Scene.get());
        ViewCullingInput input{};
        input.viewId = 1;
        input.cascadeIndex = shadow ? 0 : kCullingCascadeIndexNone;
        input.viewMatrix = input.projMatrix = input.viewProjMatrix = Matrix4x4::Identity();
        GPUCullingUtils::ExtractFrustumPlanes(input.viewProjMatrix, input.frustumPlanes);
        input.instanceCount = m_Scene->GetInstanceCount();
        input.renderLayerMask = mask;
        if (fused)
        {
            CascadeCullingGroup group{};
            group.viewId = input.viewId;
            group.cascadeCount = 4;
            group.instanceCount = input.instanceCount;
            group.renderLayerMask = mask;
            group.shadowCasterDispatch = shadow;
            for (uint32_t c = 0; c < 4; ++c)
            {
                group.lightVP[c] = input.viewProjMatrix;
                for (uint32_t p = 0; p < 6; ++p)
                    group.frustumPlanes[c][p] = input.frustumPlanes[p];
            }
            culling->SubmitCascadeGroup(group);
        }
        else if (noSpatial)
        {
            ViewCullingContext context{};
            context.Id = input.viewId;
            context.CascadeIndex = input.cascadeIndex;
            context.InstanceCount = input.instanceCount;
            context.RenderLayerMask = mask;
            context.CullingPipeline = culling.get();
            context.Scene = m_Scene.get();
            for (uint32_t p = 0; p < 6; ++p)
                context.FrustumPlanes[p] = input.frustumPlanes[p];
            NoneCullingStrategy{}.ScheduleCulling(context);
        }
        else
            culling->SubmitView(input);
        culling->EndFrame();
        const auto source = culling->GetVisibilityRG();
        ASSERT_TRUE(source.IsValid());
        const auto ranges = culling->GetViewVisibilityRanges();
        ASSERT_FALSE(ranges.empty());
        // Every fused cascade has the same frustum: use the last to exercise
        // the group's slice offset rather than accidentally testing slice zero.
        const auto range = ranges.back();
        const size_t bytes = input.instanceCount * sizeof(uint32_t);
        frame.AddPass("LayerTest.CopyVisibility", 0, [&](RG::RGPassBuilder& p)
                      { p.Read(source, RG::RGBufferRead::CopySrc); p.PreventCulling(); }, [=](RG::RGContext& ctx)
                      { ctx.Cmd->CopyBuffer(ctx.GetBuffer(source), destination, bytes,
                                            range.visibilityOffset * sizeof(uint32_t), 0); });
        frame.Execute();
        m_Device->WaitForIdle();
    }

    // Bodies the other scatter packages also run (the ScatterPackageParity suites
    // at the end of this file). Each is one TEST_P body of this suite.
    void ExpectScattersInstancesIntoCorrectRegions();
    void ExpectShadowClassModeRoutesByFlagsIntoSentinelRegions();
    void ExpectMirroredInstancesRouteIntoParityOneRegion();
    void ExpectConservativeBoundsPreserveReferenceSwitchesAndBias();
    void ExpectLodBandHistoryIsIndexedPerInstanceNotPerSlot();
    void ExpectCrossfadeLevelChangeEmitsItsPairIntoTheTailNotTheHead();

    std::unique_ptr<RenderGraph::RGResourcePool> m_CullPersistent;
    std::unique_ptr<RenderGraph::RGTransientPool> m_CullTransient;
    std::unique_ptr<RenderGraph::RGUploadRing> m_CullRing;
    std::unique_ptr<RenderGraph::RGFrame> m_CullFrame;
    std::unique_ptr<GPUCullingPipeline> m_Culling;
    uint64_t m_CullFrameNumber = 0;
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
    std::unique_ptr<GPUDrawStreamBuilder> m_Builder;
    PipelineHandle m_Pipeline; // the fetch variant under test (per GetParam())
    // Persistent across Dispatch calls so a test can build a transition history
    // the way consecutive frames do. Created on first use (all-zero = no history).
    BufferHandle m_LodFade{};
    uint32_t     m_LodFadeCount = 0;
    // Persistent across Dispatch calls so a test can build a LOD history the
    // way consecutive frames do. Created on first use (all-no-history).
    BufferHandle m_PrevLod{};
    uint32_t     m_PrevLodCount = 0;
    // The rendered level and phase PAIR, persistent across Dispatch calls.
    // m_RenderedCurrentIndex names the half this frame writes; the other half is
    // what it reads, and nothing writes it — the immutability the pair exists
    // for. RotateRenderedHistory swaps the roles the way a recorded frame does.
    BufferHandle m_Rendered[2]{};
    uint32_t     m_RenderedCount = 0;
    uint32_t     m_RenderedCurrentIndex = 0;
    std::vector<BufferHandle> m_OwnedBuffers;
};

TEST_P(BatchScatterComputeTest, RenderLayersFilterSameBatchColorAndShadow)
{
    const uint32_t mesh = AddMeshRow(3);
    const uint32_t masks[] = {0u, 1u, 2u, 0x80000000u, 0xFFFFFFFFu};
    uint32_t slot = 0;
    for (uint32_t mask : masks)
    {
        auto instance = MakeInstance(0u, mesh, 1u);
        instance.transform = Matrix4x4::Identity();
        instance.transform.Data()[12] = -.8f + .4f * slot++;
        instance.prevTransform = instance.transform;
        instance.boundingCenter = Vector3(0, 0, .5f);
        instance.boundingRadius = .01f;
        instance.renderLayerMask = mask;
        m_Scene->AddInstance(instance);
    }
    const std::vector<uint8_t> classes(1, static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided));
    for (bool shadow : {false, true})
        for (bool fused : {false, true})
            for (uint32_t mask : {0u, 1u, 2u, 0x80000000u, 0xFFFFFFFFu})
            {
                SCOPED_TRACE(::testing::Message() << "shadow=" << shadow << " fused=" << fused << " mask=" << mask);
                uint32_t total = 0;
                const auto table = shadow
                                       ? GPUDrawStreamBuilder::BuildShadowBatchTable(m_Scene->GetBatchRegistry(), classes, {}, &total)
                                       : GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
                ASSERT_EQ(table.size(), 1u);
                const auto r = Dispatch(table, total, 0, 0, 1, total,
                                        shadow ? GPUDrawStreamBuilder::kClassModeShadow : 0u, {}, 0xFFFFFFFFu, {}, 0u, {},
                                        true, mask, shadow, fused);
                std::set<uint32_t> expected;
                for (uint32_t i = 0; i < std::size(masks); ++i)
                    if ((masks[i] & mask) != 0)
                        expected.insert(i);
                EXPECT_EQ(r.cursors[0], expected.size());
                std::set<uint32_t> actual(r.indirection.begin(), r.indirection.begin() + r.cursors[0]);
                EXPECT_EQ(actual, expected);
                CheckRaster(r, expected, shadow);
                EXPECT_EQ(r.stats.tableMisses, 0u);
                EXPECT_EQ(r.stats.overflows, 0u);
            }
}

TEST_P(BatchScatterComputeTest, LayerOnlyChangesWakeRetainedGpuVisibility)
{
    const uint32_t mesh = AddMeshRow(3);
    auto instance = MakeInstance(0, mesh, 1);
    instance.renderLayerMask = 1;
    instance.boundingCenter = Vector3(0, 0, .5f);
    instance.boundingRadius = .01f;
    const auto slot = m_Scene->AddInstance(instance);
    m_Scene->FlushGPUBuffers();
    const auto visibility = MakeStorageBuffer(sizeof(uint32_t), "LayerTest.Retained");
    for (bool fused : {false, true})
    {
        for (int frame = 0; frame < 8; ++frame)
            CullInto(visibility, 1u, true, fused);
        ASSERT_TRUE(m_Culling->WasElidedThisFrame());
        EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 1u);
        CullInto(visibility, 2u, true, fused);
        EXPECT_FALSE(m_Culling->WasElidedThisFrame());
        EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 0u);
        for (int frame = 0; frame < 8; ++frame)
            CullInto(visibility, 2u, true, fused);
        ASSERT_TRUE(m_Culling->WasElidedThisFrame());
        instance.renderLayerMask = 2u;
        m_Scene->UpdateInstance(slot, instance);
        m_Scene->FlushGPUBuffers();
        CullInto(visibility, 2u, true, fused);
        EXPECT_FALSE(m_Culling->WasElidedThisFrame());
        EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 1u);
        instance.renderLayerMask = 1u;
        m_Scene->UpdateInstance(slot, instance);
        m_Scene->FlushGPUBuffers();
    }
}

TEST_P(BatchScatterComputeTest, NoSpatialPreviewStillFiltersLayers)
{
    const auto mesh = AddMeshRow(3);
    auto instance = MakeInstance(0, mesh, 1);
    instance.renderLayerMask = 1;
    instance.boundingCenter = Vector3(100, 0, .5f);
    instance.boundingRadius = .01f;
    m_Scene->AddInstance(instance);
    m_Scene->FlushGPUBuffers();
    const auto visibility = MakeStorageBuffer(sizeof(uint32_t), "LayerTest.Preview");
    CullInto(visibility, 1u, false, false);
    EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 0u);
    CullInto(visibility, 1u, false, false, true);
    EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 1u) << "None means no spatial rejection";
    CullInto(visibility, 0u, false, false, true);
    EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 0u) << "explicitly hidden preview stays hidden";
    CullInto(visibility, 2u, false, false, true);
    EXPECT_EQ(ReadBuffer<uint32_t>(visibility, 1)[0], 0u);
}

TEST(BatchTableBuild, SortsByMaterialThenMeshWithExclusivePrefix)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(2u, 5u, /*mirrored=*/false);
    reg.OnInstanceAdded(2u, 5u, /*mirrored=*/false);
    reg.OnInstanceAdded(1u, 9u, /*mirrored=*/false);
    reg.OnInstanceAdded(2u, 3u, /*mirrored=*/false);

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &total);
    ASSERT_EQ(table.size(), 3u);
    EXPECT_EQ(total, 4u);

    // (1,9) -> (2,3) -> (2,5): material primary, mesh secondary.
    EXPECT_EQ(table[0].materialIndex, 1u);
    EXPECT_EQ(table[0].meshIndex, 9u);
    EXPECT_EQ(table[0].recordOffset, 0u);
    EXPECT_EQ(table[0].capacity, 1u);

    EXPECT_EQ(table[1].materialIndex, 2u);
    EXPECT_EQ(table[1].meshIndex, 3u);
    EXPECT_EQ(table[1].recordOffset, 1u);
    EXPECT_EQ(table[1].capacity, 1u);

    EXPECT_EQ(table[2].materialIndex, 2u);
    EXPECT_EQ(table[2].meshIndex, 5u);
    EXPECT_EQ(table[2].recordOffset, 2u);
    EXPECT_EQ(table[2].capacity, 2u);
}

// Extraction det-sign: ComputeNormalMatrixColumns reports mirrored == (the
// upper-3x3 determinant is negative). This is the source of GPUInstance.flags
// bit 4 stamped at all four producer sites. Column-major matrices.
TEST(MirrorParityDetSign, DetectsNegativeDeterminantTransforms)
{
    Vector4 n0, n1, n2;
    bool mirrored = true;

    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MatrixUtils::ComputeNormalMatrixColumns(identity, n0, n1, n2, &mirrored);
    EXPECT_FALSE(mirrored) << "identity is not mirrored";

    // Pure rotation (90 deg about Z, column-major): det = +1, not mirrored.
    const float rotZ[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MatrixUtils::ComputeNormalMatrixColumns(rotZ, n0, n1, n2, &mirrored);
    EXPECT_FALSE(mirrored) << "rotation preserves winding (det = +1)";

    // Single negative-scale axis: det = -1, mirrored.
    const float negX[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MatrixUtils::ComputeNormalMatrixColumns(negX, n0, n1, n2, &mirrored);
    EXPECT_TRUE(mirrored) << "one negative scale axis flips winding";

    // Two negative-scale axes: det = +1 (even), not mirrored.
    const float negXY[16] = {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MatrixUtils::ComputeNormalMatrixColumns(negXY, n0, n1, n2, &mirrored);
    EXPECT_FALSE(mirrored) << "two negative axes cancel (det = +1)";

    // Rotation composed with one negative-scale axis: still det = -1, mirrored.
    // (A naive per-axis scale.x<0 test would misfire here — rotation
    // redistributes the negative across columns.)
    const float rotNegX[16] = {0, -1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MatrixUtils::ComputeNormalMatrixColumns(rotNegX, n0, n1, n2, &mirrored);
    EXPECT_TRUE(mirrored) << "rotation * negative-scale composition is mirrored";

    // Singular upper-3x3: reports not-mirrored (winding is undefined).
    const float singular[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    MatrixUtils::ComputeNormalMatrixColumns(singular, n0, n1, n2, &mirrored);
    EXPECT_FALSE(mirrored) << "singular transform is not treated as mirrored";
}

void BatchScatterComputeTest::ExpectScattersInstancesIntoCorrectRegions()
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);

    // 3 instances of (mat 1, meshA), 2 of (mat 2, meshB), plus one tombstoned
    // slot in the middle — the scatter must skip it without a table miss.
    std::vector<uint32_t> aIdx, bIdx;
    aIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));
    const uint32_t doomed = m_Scene->AddInstance(MakeInstance(1u, meshA));
    bIdx.push_back(m_Scene->AddInstance(MakeInstance(2u, meshB)));
    aIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));
    bIdx.push_back(m_Scene->AddInstance(MakeInstance(2u, meshB)));
    aIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));
    m_Scene->RemoveInstance(doomed); // tombstone mid-array

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(table.size(), 2u);
    ASSERT_EQ(total, 5u);
    ASSERT_EQ(table[0].capacity, 3u); // (1, meshA)
    ASSERT_EQ(table[1].capacity, 2u); // (2, meshB)

    const auto r = Dispatch(table, total, /*cursorBase=*/0u, /*recordBase=*/0u,
                            /*cursorSlots=*/2u, /*recordSlots=*/total);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 3u);
    EXPECT_EQ(r.cursors[1], 2u);

    // drawnTriangles is the stats-axis payload: Σ (lodIndexCount / 3) over the
    // survivors. 3×meshA (6/3=2) + 2×meshB (12/3=4) = 14. Present only in the
    // stats-on variant; compiled out (reads 0) when GE_SCATTER_STATS is off.
    // Both variants above produced identical records/cursors — proof the stats
    // atomic never perturbs a scatter record.
    constexpr uint32_t kExpectedTriangles = 3u * (6u / 3u) + 2u * (12u / 3u);
    EXPECT_EQ(r.stats.drawnTriangles, UseStats() ? kExpectedTriangles : 0u);

    // Region 0 = records [0,3): meshA draws (indexCount 6), indirection maps
    // to exactly the meshA instance set. Region 1 = records [3,5): meshB.
    std::set<uint32_t> gotA, gotB;
    for (uint32_t s = 0; s < 3u; ++s)
    {
        EXPECT_EQ(r.records[s * kDrawRecordUints + 0], 6u);  // indexCount
        EXPECT_EQ(r.records[s * kDrawRecordUints + 1], 1u);  // instanceCount
        EXPECT_EQ(r.records[s * kDrawRecordUints + 4], s);   // firstInstance = global slot
        gotA.insert(r.indirection[s]);
    }
    for (uint32_t s = 3; s < 5u; ++s)
    {
        EXPECT_EQ(r.records[s * kDrawRecordUints + 0], 12u);
        EXPECT_EQ(r.records[s * kDrawRecordUints + 4], s);
        gotB.insert(r.indirection[s]);
    }
    EXPECT_EQ(gotA, std::set<uint32_t>(aIdx.begin(), aIdx.end()));
    EXPECT_EQ(gotB, std::set<uint32_t>(bIdx.begin(), bIdx.end()));
}

TEST_P(BatchScatterComputeTest, ScattersInstancesIntoCorrectRegions)
{
    ExpectScattersInstancesIntoCorrectRegions();
}

TEST_P(BatchScatterComputeTest, ArenaBasesOffsetCursorsAndRecords)
{
    const uint32_t meshA = AddMeshRow(6u);
    m_Scene->AddInstance(MakeInstance(1u, meshA));
    m_Scene->AddInstance(MakeInstance(1u, meshA));

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);

    // Simulate a second schedule call in the same device frame: this slice's
    // cursors start at index 3 and its records at slot 4.
    const uint32_t kCursorBase = 3u;
    const uint32_t kRecordBase = 4u;
    const auto r = Dispatch(table, total, kCursorBase, kRecordBase,
                            /*cursorSlots=*/kCursorBase + 1u,
                            /*recordSlots=*/kRecordBase + total);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    for (uint32_t c = 0; c < kCursorBase; ++c)
        EXPECT_EQ(r.cursors[c], 0u) << "cursor below the base must be untouched";
    EXPECT_EQ(r.cursors[kCursorBase], 2u);

    for (uint32_t s = 0; s < kRecordBase; ++s)
        EXPECT_EQ(r.records[s * kDrawRecordUints + 0], 0u)
            << "record below the base must be untouched";
    for (uint32_t s = kRecordBase; s < kRecordBase + total; ++s)
    {
        EXPECT_EQ(r.records[s * kDrawRecordUints + 0], 6u);
        EXPECT_EQ(r.records[s * kDrawRecordUints + 4], s); // arena-global firstInstance
        EXPECT_NE(r.indirection[s], 0xDEADBEEFu);
    }
}

TEST_P(BatchScatterComputeTest, InstanceMissingFromTableCountsAsMiss)
{
    const uint32_t meshA = AddMeshRow(6u);
    m_Scene->AddInstance(MakeInstance(1u, meshA));
    m_Scene->AddInstance(MakeInstance(7u, meshA)); // NOT in the table below

    // Hand-build a table that only knows (1, meshA) — a registry/table
    // divergence. The stray instance must land in tableMisses, never in a
    // region.
    std::vector<GPUDrawStreamBuilder::BatchTableEntry> table(1);
    table[0] = {1u, meshA, 0u, 1u};

    const auto r = Dispatch(table, 1u, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/1u);
    EXPECT_EQ(r.stats.tableMisses, 1u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 1u);
}

TEST_P(BatchScatterComputeTest, UndersizedCapacityTripsOverflowNotOOB)
{
    const uint32_t meshA = AddMeshRow(6u);
    for (int i = 0; i < 4; ++i)
        m_Scene->AddInstance(MakeInstance(1u, meshA));

    // Capacity lies (2 < 4): simulates a broken snapshot invariant. Exactly
    // two records land; two trip the overflow counter; nothing writes past
    // the region.
    std::vector<GPUDrawStreamBuilder::BatchTableEntry> table(1);
    table[0] = {1u, meshA, 0u, 2u};

    const uint32_t kGuardSlots = 3u; // one guard record past the region
    const auto r = Dispatch(table, 2u, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/kGuardSlots);
    EXPECT_EQ(r.stats.overflows, 2u);
    EXPECT_EQ(r.records[2 * kDrawRecordUints + 0], 0u) << "guard record past capacity must stay zero";
    EXPECT_EQ(r.indirection[2], 0xDEADBEEFu);
}

// R1.5: with classMode == Shadow the shader routes on the depth-class flags
// bits, not materialIndex — shared-depth-eligible casters of a mesh collapse
// into their single/double-sided class sentinel, material-dependent ones keep
// their (materialIndex, mesh) region. The GPU routing must match the CPU-built
// shadow table exactly, with zero tripwire hits.
void BatchScatterComputeTest::ExpectShadowClassModeRoutesByFlagsIntoSentinelRegions()
{
    const uint32_t meshA = AddMeshRow(6u);

    // mat 0 = single-sided eligible (flags 0), mat 1 = double-sided eligible
    // (bit 3), mat 2 = material-dependent (bit 2). Two eligible-SS + one DS +
    // one dependent, all on meshA.
    std::vector<uint32_t> ssIdx, dsIdx, depIdx;
    ssIdx.push_back(m_Scene->AddInstance(MakeInstance(0u, meshA, 0u)));
    ssIdx.push_back(m_Scene->AddInstance(MakeInstance(0u, meshA, 0u)));
    dsIdx.push_back(
        m_Scene->AddInstance(MakeInstance(1u, meshA, kInstanceFlagDepthDoubleSided)));
    depIdx.push_back(
        m_Scene->AddInstance(MakeInstance(2u, meshA, kInstanceFlagDepthMaterialDependent)));

    const std::vector<uint8_t> classes{
        static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided),
        static_cast<uint8_t>(MaterialDepthClass::EligibleDoubleSided),
        static_cast<uint8_t>(MaterialDepthClass::MaterialDependent)};

    uint32_t total = 0u;
    const auto shadow =
        GPUDrawStreamBuilder::BuildShadowBatchTable(m_Scene->GetBatchRegistry(), classes, {}, &total);
    ASSERT_EQ(total, 4u);
    ASSERT_EQ(shadow.size(), 3u);
    // Sorted by (materialIndex, mesh): dependent(2) < DS sentinel < SS sentinel.
    ASSERT_EQ(shadow[0].materialIndex, 2u);
    ASSERT_EQ(shadow[1].materialIndex, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel);
    ASSERT_EQ(shadow[2].materialIndex, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel);

    const auto r = Dispatch(shadow, total, /*cursorBase=*/0u, /*recordBase=*/0u,
                            /*cursorSlots=*/3u, /*recordSlots=*/total,
                            /*classMode=*/GPUDrawStreamBuilder::kClassModeShadow);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 1u); // dependent (mat 2)
    EXPECT_EQ(r.cursors[1], 1u); // double-sided sentinel
    EXPECT_EQ(r.cursors[2], 2u); // single-sided sentinel (two eligible mats merged)

    // Region layout from the exclusive prefix: dependent [0,1), DS [1,2),
    // SS [2,4). Indirection in each region must map back to exactly the
    // instances the flags routed there.
    std::set<uint32_t> gotDep{r.indirection[0]};
    std::set<uint32_t> gotDs{r.indirection[1]};
    std::set<uint32_t> gotSs{r.indirection[2], r.indirection[3]};
    EXPECT_EQ(gotDep, std::set<uint32_t>(depIdx.begin(), depIdx.end()));
    EXPECT_EQ(gotDs, std::set<uint32_t>(dsIdx.begin(), dsIdx.end()));
    EXPECT_EQ(gotSs, std::set<uint32_t>(ssIdx.begin(), ssIdx.end()));
}

TEST_P(BatchScatterComputeTest, ShadowClassModeRoutesByFlagsIntoSentinelRegions)
{
    ExpectShadowClassModeRoutesByFlagsIntoSentinelRegions();
}

// P2: classMode == Color routes each instance by materialColorClass[matIdx] --
// shared-PSO opaque casters collapse into their colorClassId region while a
// transmissive material keeps its identity region. The GPU routing must match
// the CPU-built color table exactly, with zero tripwire hits (the color-only
// A/B misses this: a shared-slice desync only surfaces when the class map and
// the table disagree, which this dispatch would trip as a tableMiss).
TEST_P(BatchScatterComputeTest, ColorClassModeMergesSharedPsoAndKeepsTransmissiveIdentity)
{
    const uint32_t meshA = AddMeshRow(6u);

    // mat 0,1 share color class C0 (opaque, same PSO); mat 2 keeps identity
    // (transmissive). Two mat0 + one mat1 must merge into the class region; the
    // transmissive mat2 lands in its own.
    std::vector<uint32_t> classIdx, idIdx;
    classIdx.push_back(m_Scene->AddInstance(MakeInstance(0u, meshA)));
    idIdx.push_back(m_Scene->AddInstance(MakeInstance(2u, meshA)));
    classIdx.push_back(m_Scene->AddInstance(MakeInstance(0u, meshA)));
    classIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));

    const uint32_t C0 = GPUDrawStreamBuilder::kColorClassBase;
    const std::vector<uint32_t> colorClass{C0, C0, 2u}; // map[mat] domain rule

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildColorBatchTable(
        m_Scene->GetBatchRegistry(), colorClass, {}, &total);
    ASSERT_EQ(total, 4u);
    ASSERT_EQ(table.size(), 2u);
    // Sorted by (classKey, mesh): the identity real index (2) sorts below C0.
    ASSERT_EQ(table[0].materialIndex, 2u);
    ASSERT_EQ(table[0].capacity, 1u);
    ASSERT_EQ(table[1].materialIndex, C0);
    ASSERT_EQ(table[1].capacity, 3u);

    const auto r = Dispatch(table, total, /*cursorBase=*/0u, /*recordBase=*/0u,
                            /*cursorSlots=*/2u, /*recordSlots=*/total,
                            /*classMode=*/GPUDrawStreamBuilder::kClassModeColor, colorClass);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 1u); // identity transmissive region
    EXPECT_EQ(r.cursors[1], 3u); // merged class region (2 x mat0 + 1 x mat1)

    // Region 0 = records [0,1): the transmissive instance. Region 1 = [1,4):
    // exactly the three shared-PSO opaque instances, in one arena range.
    std::set<uint32_t> gotId{r.indirection[0]};
    std::set<uint32_t> gotClass{r.indirection[1], r.indirection[2], r.indirection[3]};
    EXPECT_EQ(gotId, std::set<uint32_t>(idIdx.begin(), idIdx.end()));
    EXPECT_EQ(gotClass, std::set<uint32_t>(classIdx.begin(), classIdx.end()));
}

// Scatter tripwire root cause (color axis): the color-class map bound at
// binding 8 MUST come from the same snapshot as the batch table. In production
// the map was uploaded once per app frame while every schedule call rebuilt its
// table from the CURRENT MaterialColorClassSpan(); a mid-frame material
// registration (scene-load pump / thumbnail extraction) grew the span, so a
// later call bound a STALE (shorter) map against a fresh table. A merge-eligible
// instance whose material index is past the stale map falls back to its raw
// materialIndex, which the merged table has no row for → tableMiss. This test
// pins that mechanism: same scene + table, stale map trips a miss, matching map
// (the per-call-upload fix) does not.
TEST_P(BatchScatterComputeTest, StaleColorClassMapTripsTableMissMatchingMapDoesNot)
{
    const uint32_t meshA = AddMeshRow(6u);

    // Three opaque casters sharing one color class C0. mat 2 stands in for a
    // material registered mid-frame — present in the fresh table's map snapshot
    // but absent from a map uploaded before it existed.
    m_Scene->AddInstance(MakeInstance(0u, meshA));
    m_Scene->AddInstance(MakeInstance(1u, meshA));
    m_Scene->AddInstance(MakeInstance(2u, meshA));

    const uint32_t C0 = GPUDrawStreamBuilder::kColorClassBase;
    const std::vector<uint32_t> freshMap{C0, C0, C0}; // table built from this
    const std::vector<uint32_t> staleMap{C0, C0};     // uploaded before mat 2 existed

    uint32_t total = 0u;
    const auto table =
        GPUDrawStreamBuilder::BuildColorBatchTable(m_Scene->GetBatchRegistry(), freshMap, {}, &total);
    ASSERT_EQ(total, 3u);
    ASSERT_EQ(table.size(), 1u); // all three merge into the single C0 row
    ASSERT_EQ(table[0].materialIndex, C0);
    ASSERT_EQ(table[0].capacity, 3u);

    // Stale map: mat 2 (index past the map) falls back to raw materialIndex 2,
    // which the C0-only table has no row for → exactly one tableMiss.
    const auto stale = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/total,
                                GPUDrawStreamBuilder::kClassModeColor, staleMap);
    EXPECT_EQ(stale.stats.tableMisses, 1u) << "stale (short) color map must trip the tripwire";
    EXPECT_EQ(stale.stats.overflows, 0u);
    EXPECT_EQ(stale.cursors[0], 2u) << "only mat 0 and mat 1 routed into C0";

    // Matching map (per-call upload): all three route into C0, zero tripwire.
    const auto fixed = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/total,
                                GPUDrawStreamBuilder::kClassModeColor, freshMap);
    EXPECT_EQ(fixed.stats.tableMisses, 0u) << "map matching the table snapshot must not miss";
    EXPECT_EQ(fixed.stats.overflows, 0u);
    EXPECT_EQ(fixed.cursors[0], 3u);
}

// Winding parity: with the color/off table split into parity-0/parity-1 sibling
// rows, the shader routes mirrored (flags bit 4) instances into the parity-1
// row (searchMesh = mesh | 1<<24) and non-mirrored into parity-0 — while the
// mesh-table read still uses the REAL meshIndex. Zero tripwire hits.
void BatchScatterComputeTest::ExpectMirroredInstancesRouteIntoParityOneRegion()
{
    const uint32_t meshA = AddMeshRow(6u);

    std::vector<uint32_t> evenIdx, oddIdx;
    evenIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA, 0u)));
    oddIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA, kInstanceFlagMirrored)));
    evenIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA, 0u)));
    oddIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA, kInstanceFlagMirrored)));
    evenIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA, 0u)));

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 5u);
    ASSERT_EQ(table.size(), 2u);          // parity-0 + parity-1 sibling rows
    ASSERT_EQ(table[0].capacity, 3u);     // parity-0 (non-mirrored)
    ASSERT_EQ(table[1].capacity, 2u);     // parity-1 (mirrored)

    const auto r = Dispatch(table, total, /*cursorBase=*/0u, /*recordBase=*/0u,
                            /*cursorSlots=*/2u, /*recordSlots=*/total);

    EXPECT_EQ(r.stats.tableMisses, 0u) << "every instance found its parity row";
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 3u); // parity-0 region
    EXPECT_EQ(r.cursors[1], 2u); // parity-1 region

    // Region 0 = records [0,3): the non-mirrored set. Region 1 = [3,5): mirrored.
    std::set<uint32_t> gotEven{r.indirection[0], r.indirection[1], r.indirection[2]};
    std::set<uint32_t> gotOdd{r.indirection[3], r.indirection[4]};
    EXPECT_EQ(gotEven, std::set<uint32_t>(evenIdx.begin(), evenIdx.end()));
    EXPECT_EQ(gotOdd, std::set<uint32_t>(oddIdx.begin(), oddIdx.end()));
}

TEST_P(BatchScatterComputeTest, MirroredInstancesRouteIntoParityOneRegion)
{
    ExpectMirroredInstancesRouteIntoParityOneRegion();
}

// Shadow-mode double-sided gate (must-fix #1): a mirrored double-sided caster
// must route to the parity-0 double-sided sentinel row (which the shadow table
// build never splits), NOT search a parity-1 row that does not exist — that
// would be a table miss and a silently dropped shadow.
TEST_P(BatchScatterComputeTest, ShadowDoubleSidedMirroredCasterRoutesToParityZeroSentinel)
{
    const uint32_t meshA = AddMeshRow(6u);

    // Two double-sided-eligible casters on meshA, one mirrored.
    std::vector<uint32_t> dsIdx;
    dsIdx.push_back(
        m_Scene->AddInstance(MakeInstance(0u, meshA, kInstanceFlagDepthDoubleSided)));
    dsIdx.push_back(m_Scene->AddInstance(
        MakeInstance(0u, meshA, kInstanceFlagDepthDoubleSided | kInstanceFlagMirrored)));

    const std::vector<uint8_t> classes{static_cast<uint8_t>(MaterialDepthClass::EligibleDoubleSided)};

    uint32_t total = 0u;
    const auto shadow =
        GPUDrawStreamBuilder::BuildShadowBatchTable(m_Scene->GetBatchRegistry(), classes, {}, &total);
    ASSERT_EQ(total, 2u);
    ASSERT_EQ(shadow.size(), 1u) << "double-sided sentinel is a single parity-0 row (no split)";
    ASSERT_EQ(shadow[0].materialIndex, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel);
    ASSERT_EQ(shadow[0].capacity, 2u) << "both parities collapse into the parity-0 sentinel";

    const auto r = Dispatch(shadow, total, /*cursorBase=*/0u, /*recordBase=*/0u,
                            /*cursorSlots=*/1u, /*recordSlots=*/total,
                            /*classMode=*/GPUDrawStreamBuilder::kClassModeShadow);

    EXPECT_EQ(r.stats.tableMisses, 0u) << "mirrored DS caster must NOT miss (its shadow survives)";
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 2u); // both casters in the one sentinel region
    std::set<uint32_t> got{r.indirection[0], r.indirection[1]};
    EXPECT_EQ(got, std::set<uint32_t>(dsIdx.begin(), dsIdx.end()));
}

// Phase C1: the emitted draw record's vertexOffset must compose base +
// lodVertexOffset[lod]. An authored LOD1 living at a relative block offset draws
// at vertexOffset + that offset; LOD0 draws at the base. forceLod pins the level
// so the test is FOV-independent. Runs under both fetch variants.
TEST_P(BatchScatterComputeTest, LodVertexOffsetComposesIntoDrawRecord)
{
    GPUMesh m{};
    m.indexCount     = 6u;
    m.indexOffset    = 0u;
    m.vertexOffset   = 50u;  // base (absolute, in vertices)
    m.vertexCount    = 8u;
    m.lodCount       = 2u;
    m.boundingRadius = 1.0f;
    m.lodIndexCount[0]  = 6u; m.lodIndexOffset[0]  = 0u; m.lodVertexOffset[0] = 0u;
    m.lodIndexCount[1]  = 3u; m.lodIndexOffset[1]  = 6u; m.lodVertexOffset[1] = 100u;
    const uint32_t meshIdx = m_Scene->AddMesh(m);
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // LOD0: vertexOffset == base, indexCount == LOD0's.
    {
        const auto r = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/1u,
                                /*classMode=*/0u, /*materialColorClass=*/{}, /*forceLod=*/0u);
        EXPECT_EQ(r.stats.tableMisses, 0u);
        EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], 6u);   // indexCount
        EXPECT_EQ(r.records[0 * kDrawRecordUints + 2], 0u);   // firstIndex
        EXPECT_EQ(r.records[0 * kDrawRecordUints + 3], 50u);  // vertexOffset = base + 0
    }
    // LOD1: vertexOffset == base + lodVertexOffset[1], its own index range.
    {
        const auto r = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/1u,
                                /*classMode=*/0u, /*materialColorClass=*/{}, /*forceLod=*/1u);
        EXPECT_EQ(r.stats.tableMisses, 0u);
        EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], 3u);    // LOD1 indexCount
        EXPECT_EQ(r.records[0 * kDrawRecordUints + 2], 6u);    // LOD1 firstIndex
        EXPECT_EQ(r.records[0 * kDrawRecordUints + 3], 150u);  // base(50) + lodVertexOffset(100)
    }
}

// ge_SelectLOD's auto-select scan on the GPU, against the C++ rule it mirrors
// (MeshLODThresholds.h::LodEffectiveThreshold). The chain mixes comparison
// spaces — a capped sloppy slot0 followed by a near-lossless SSE slot1 — which
// is the shape whose effective thresholds ASCEND across the boundary. Descent
// is enforced inside the scan, so LOD1 must stay reachable and the GPU's pick
// must equal the CPU rule's at every coverage. This is the drift guard for
// having the rule in two languages: a divergence in either fails here.
TEST_P(BatchScatterComputeTest, LodAutoSelectMatchesTheCpuRuleAcrossSpaces)
{
    GPUMesh m{};
    m.indexCount     = 30u;
    m.indexOffset    = 0u;
    m.vertexCount    = 8u;
    m.lodCount       = 3u;
    m.boundingRadius = 1.0f;
    // Distinct index counts so the emitted record identifies the chosen level.
    m.lodIndexCount[0] = 30u; m.lodIndexOffset[0] = 0u;
    m.lodIndexCount[1] = 12u; m.lodIndexOffset[1] = 30u;
    m.lodIndexCount[2] = 3u;  m.lodIndexOffset[2] = 42u;

    // slot0: sloppy -> coverage space at the cap. slot1: e = 0.0087 -> SSE.
    const float err[4]    = {0.0f, 0.5f, 0.0087f, 0.0f};
    const uint8_t slop[4] = {0, 1, 0, 0};
    const float kDefaultTable[4] = {0.5f, 0.2f, 0.08f, 0.0f};
    float threshold[4] = {};
    const uint32_t mask = Rendering::DeriveLODThresholds(
        err, slop, /*lodCount=*/3u, /*authoredChain=*/false, /*boundingRadius=*/1.0f,
        /*maxExtent=*/1.0f, kDefaultTable, 4u, threshold);
    ASSERT_EQ(mask, 0b010u) << "expected slot0 coverage, slot1 SSE";
    for (uint32_t k = 0; k < 4u; ++k)
        m.lodThreshold[k] = threshold[k];
    m.lodFlags = mask;
    // Seeded exactly as BuildGpuMeshRow does, so the sweep exercises the
    // per-mesh scale ceiling in both languages too.
    m.lodSseScaleCeil = Rendering::LodSseScaleCeil(/*boundingRadius=*/1.0f,
                                                   /*maxExtent=*/1.0f);

    const uint32_t meshIdx = m_Scene->AddMesh(m);
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // coverage = boundingRadius * projScaleY / distance, so sweeping the camera
    // distance sweeps coverage. projScaleY = 1 makes coverage = 1/distance.
    constexpr float kProjScaleY = 1.0f;
    const float toCov = Rendering::LodSseThresholdToCoverage(902u, 10.0f);
    const uint32_t expectedIndexCount[3] = {30u, 12u, 3u};

    bool sawLod1 = false;
    for (float distance : {0.05f, 0.5f, 2.0f, 8.0f, 20.0f, 40.0f, 100.0f, 400.0f}) {
        const float coverage = kProjScaleY / distance;
        // The CPU rule, applied exactly as the scan does.
        uint32_t expectedLod = m.lodCount - 1u;
        float prevEff = Rendering::kLodEffNone;
        for (uint32_t k = 0; k < m.lodCount; ++k) {
            const float eff = Rendering::LodEffectiveThreshold(
                threshold[k], ((mask >> k) & 1u) != 0u, toCov, m.lodSseScaleCeil, prevEff);
            prevEff = eff;
            if (coverage >= eff) { expectedLod = k; break; }
        }
        sawLod1 |= (expectedLod == 1u);

        LodDispatchParams lod{};
        lod.projScaleY = kProjScaleY;
        lod.cameraPos[2] = distance; // instance sits at the origin
        lod.sseThresholdToCoverage = toCov;
        lod.sseThresholdToCoverageTight = toCov;
        const auto r = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/1u,
                                /*classMode=*/0u, /*materialColorClass=*/{},
                                /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                                /*meshGroupMode=*/0u, lod);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        EXPECT_EQ(r.records[0], expectedIndexCount[expectedLod])
            << "GPU picked a different level than the CPU rule at distance " << distance
            << " (coverage " << coverage << ", expected LOD" << expectedLod << ")";
    }
    EXPECT_TRUE(sawLod1) << "the swept range must cross LOD1, or this proves nothing";
}

// LodSelectionMode::Off, end to end on the GPU: the all-zero threshold table
// DeriveLODThresholdsOff writes makes ge_SelectLOD's scan match slot 0 at any
// coverage, so the emitted record carries LOD0's index range even at a distance
// where the same chain would otherwise draw the coarsest level.
//
// The third arm is the load-bearing one: it shows Off is NOT reachable by
// zeroing the error budget. A zero budget only zeroes the per-view factor, which
// applies to SSE-FLAGGED slots — a coverage-space slot keeps its own switch
// point and still coarsens. Anything that fixes only the SSE case and leaves
// authored/sloppy/zero-error chains switching fails here.
TEST_P(BatchScatterComputeTest, OffModeZeroThresholdsDrawLod0WhereABudgetOfZeroDoesNot)
{
    GPUMesh m{};
    m.indexCount     = 30u;
    m.indexOffset    = 0u;
    m.vertexCount    = 8u;
    m.lodCount       = 3u;
    m.boundingRadius = 1.0f;
    // Distinct index counts so the emitted record identifies the chosen level.
    m.lodIndexCount[0] = 30u; m.lodIndexOffset[0] = 0u;
    m.lodIndexCount[1] = 12u; m.lodIndexOffset[1] = 30u;
    m.lodIndexCount[2] = 3u;  m.lodIndexOffset[2] = 42u;

    // Coverage space throughout (the shape a zero budget cannot rescue): the
    // shipped default table, no SSE bits.
    const float kCoverageTable[4] = {0.5f, 0.2f, 0.08f, 0.0f};
    for (uint32_t k = 0; k < 4u; ++k)
        m.lodThreshold[k] = kCoverageTable[k];
    m.lodFlags = 0u;

    const uint32_t meshIdx = m_Scene->AddMesh(m);
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // Far enough that coverage (= 1/distance at projScaleY 1) sits below every
    // switch point, so auto selection lands on the coarsest level.
    constexpr float kFarDistance = 100.0f;
    LodDispatchParams lod{};
    lod.projScaleY = 1.0f;
    lod.cameraPos[2] = kFarDistance;

    const auto dispatchAuto = [&](const LodDispatchParams& params)
    {
        return Dispatch(table, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/1u,
                        /*classMode=*/0u, /*materialColorClass=*/{},
                        /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                        /*meshGroupMode=*/0u, params);
    };

    {
        const auto r = dispatchAuto(lod);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        ASSERT_EQ(r.records[0], m.lodIndexCount[2])
            << "the control arm must actually coarsen, or Off proves nothing";
    }

    // Selection off: the row keeps its geometry, only its switch points go to 0.
    {
        GPUMesh off = m;
        Rendering::DeriveLODThresholdsOff(4u, off.lodThreshold);
        off.lodFlags = 0u;
        m_Scene->UpdateMesh(meshIdx, off);

        const auto r = dispatchAuto(lod);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        EXPECT_EQ(r.records[0], m.lodIndexCount[0]) << "Off must draw LOD0's index range";
        EXPECT_EQ(r.records[2], m.lodIndexOffset[0]) << "Off must draw LOD0's index offset";
    }

    // A zero error budget on the SAME chain: the per-view factor collapses to 0
    // (LodSseThresholdToCoverage's keep-detail fail-safe) and the coverage-space
    // slots keep switching regardless.
    {
        m_Scene->UpdateMesh(meshIdx, m);
        LodDispatchParams zeroBudget = lod;
        zeroBudget.sseThresholdToCoverage = Rendering::LodSseThresholdToCoverage(1080u, 0.0f);
        zeroBudget.sseThresholdToCoverageTight = zeroBudget.sseThresholdToCoverage;
        ASSERT_FLOAT_EQ(zeroBudget.sseThresholdToCoverage, 0.0f);

        const auto r = dispatchAuto(zeroBudget);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        EXPECT_EQ(r.records[0], m.lodIndexCount[2])
            << "a zero budget cannot express Off: coverage-space slots still coarsen";
    }
}

// ---- LOD dwell band (hysteresis) ------------------------------------------
//
// These drive the REAL ge_SelectLOD scan (projScaleY > 0, no forceLod). Every
// pre-existing LOD test pins a level with forceLod, which returns before the
// scan, so the threshold comparison had no GPU coverage at all until now. A
// two-level chain with distinct per-LOD index counts lets the emitted record
// identify which level the shader picked.
namespace
{
constexpr uint32_t kHystLod0Indices = 30u;
constexpr uint32_t kHystLod1Indices = 12u;
// Switch coverage for LOD0. With radius 1 and projScaleY 1, coverage == 1/dist,
// so LOD0 engages inside 10 units and LOD1 outside it.
constexpr float    kHystThreshold0  = 0.1f;
constexpr float    kHystBand        = 0.25f;
// Three distances around the band. The gate to GAIN detail is
// threshold*(1+band) = 0.125; the gate to KEEP it is the bare 0.1. The middle
// distance sits between the two, so its answer depends only on the history —
// that gap is the whole point of the band.
constexpr float    kNearDist        = 7.0f;  // coverage 0.1429 — clears 0.125: gains detail
constexpr float    kInBandDist      = 9.0f;  // coverage 0.1111 — inside [0.1, 0.125): holds
constexpr float    kFarDist         = 11.0f; // coverage 0.0909 — below 0.1: loses detail
// Distances that BRACKET the gain gate to within 1%, which pins the band's WIDTH
// rather than merely its presence. Gaining detail needs coverage/(1+band) >= 0.1,
// so the gate sits at coverage 0.125 (dist 8.0). Passing both rows constrains the
// effective band to (0.2375, 0.2625] — the requested 0.25 to within +/-5%, and
// narrow enough to reject a coverage*(1-band) form (effective 0.3333) or a
// half-width band (0.125). It does NOT pin the band tighter than +/-5%.
constexpr float    kBandGainDist    = 7.9208f; // coverage 0.126250 — clears 0.125
constexpr float    kBandHoldDist    = 8.0808f; // coverage 0.123750 — misses 0.125

GPUMesh MakeHysteresisMesh()
{
    GPUMesh m{};
    m.indexCount     = kHystLod0Indices;
    m.indexOffset    = 0u;
    m.vertexOffset   = 0u;
    m.vertexCount    = 32u;
    m.lodCount       = 2u;
    m.boundingRadius = 1.0f;
    m.lodIndexCount[0]  = kHystLod0Indices; m.lodIndexOffset[0] = 0u;  m.lodVertexOffset[0] = 0u;
    m.lodIndexCount[1]  = kHystLod1Indices; m.lodIndexOffset[1] = 30u; m.lodVertexOffset[1] = 0u;
    m.lodThreshold[0]   = kHystThreshold0;
    m.lodThreshold[1]   = 0.0f; // coarsest level always clears
    return m;
}

// A FOUR-level chain. Needed because with only two levels a correct multi-level
// clamp and a buggy one-level-per-frame ratchet give the same answer, so a
// two-level mesh cannot tell those two implementations apart.
constexpr uint32_t kMultiIndices[4]    = {40u, 30u, 20u, 10u};
constexpr float    kMultiThresholds[4] = {0.4f, 0.2f, 0.1f, 0.0f};
// coverage == 1/dist here; 1/1.8 = 0.5556 and 0.5556/(1+0.25) = 0.4444 >= 0.4,
// so even the BANDED scan reaches LOD0 — three levels finer than a prev of 3.
constexpr float    kMultiNearDist      = 1.8f;

GPUMesh MakeMultiLevelHysteresisMesh()
{
    GPUMesh m{};
    m.indexCount     = kMultiIndices[0];
    m.indexOffset    = 0u;
    m.vertexOffset   = 0u;
    m.vertexCount    = 64u;
    m.lodCount       = 4u;
    m.boundingRadius = 1.0f;
    uint32_t offset = 0u;
    for (uint32_t k = 0u; k < 4u; ++k)
    {
        m.lodIndexCount[k]   = kMultiIndices[k];
        m.lodIndexOffset[k]  = offset;
        m.lodVertexOffset[k] = 0u;
        m.lodThreshold[k]    = kMultiThresholds[k];
        offset += kMultiIndices[k];
    }
    return m;
}
} // namespace

// Enlarging a culling envelope must leave the reference switches in place.
// The third row is an intentionally uncorrected control: the distance sweep
// must separate it from the other two, so this cannot pass on a shader that
// ignores the new field. All cases run through compact/full and stats on/off.
void BatchScatterComputeTest::ExpectConservativeBoundsPreserveReferenceSwitchesAndBias()
{
    GPUMesh reference = MakeMultiLevelHysteresisMesh();
    ASSERT_FLOAT_EQ(reference.lodCoverageScale, 1.0f);
    GPUMesh expanded = reference;
    expanded.boundingRadius = 4.0f;
    expanded.lodCoverageScale = 0.25f;
    GPUMesh uncorrected = expanded;
    uncorrected.lodCoverageScale = 1.0f;
    const GPUMesh rows[] = {reference, expanded, uncorrected};
    GPUInstance instances[3];
    uint32_t instanceIds[3];
    // The scalar extraction uses for a nonuniform, sheared model transform.
    // This fixture exercises the resulting sphere, not the extraction system.
    const float transform[16] = {2,0,0,0, 1,3,0,0, 0,0,1,0, 11,-3,7,1};
    const float worldScale = MatrixUtils::LargestAxisScale(transform);
    ASSERT_GT(worldScale, 3.0f);
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t mesh = m_Scene->AddMesh(rows[i]);
        instances[i] = MakeInstance(1u, mesh);
        instances[i].boundingCenter = {11.0f, -3.0f, 7.0f};
        instances[i].boundingRadius = rows[i].boundingRadius * worldScale;
        instanceIds[i] = m_Scene->AddInstance(instances[i]);
    }
    uint32_t total = 0;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 3u);
    struct Sample { float coverage; float bias; uint32_t expected; };
    const Sample samples[] = {{.404f,0,0}, {.396f,0,1}, {.202f,0,1}, {.198f,0,2},
                              {.101f,0,2}, {.099f,0,3}, {.19f,1,1}, {.30f,-1,2}};
    bool controlDiffers = false;
    for (const Sample& sample : samples)
    {
        SCOPED_TRACE(::testing::Message() << "reference coverage=" << sample.coverage
                                         << " bias=" << sample.bias);
        for (uint32_t i = 0; i < 3; ++i)
        {
            instances[i].lodBias = sample.bias;
            m_Scene->UpdateInstance(instanceIds[i], instances[i]);
        }
        LodDispatchParams lod{};
        lod.projScaleY = 1.0f;
        lod.cameraPos[0] = 11.0f;
        lod.cameraPos[1] = -3.0f;
        lod.cameraPos[2] = 7.0f + worldScale / sample.coverage;
        const auto r = Dispatch(table, total, 0u, 0u, 3u, 3u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        ASSERT_EQ(r.stats.overflows, 0u);
        uint32_t counts[3] = {};
        for (uint32_t slot = 0; slot < 3; ++slot)
        {
            ASSERT_EQ(r.cursors[slot], 1u);
            ASSERT_LT(r.indirection[slot], 3u);
            counts[r.indirection[slot]] = r.records[slot * kDrawRecordUints];
        }
        EXPECT_EQ(counts[instanceIds[0]], kMultiIndices[sample.expected]);
        EXPECT_EQ(counts[instanceIds[1]], counts[instanceIds[0]]);
        controlDiffers |= counts[instanceIds[2]] != counts[instanceIds[0]];
    }
    EXPECT_TRUE(controlDiffers) << "the sweep must detect an ignored coverage scale";
}

TEST_P(BatchScatterComputeTest, ConservativeBoundsPreserveReferenceSwitchesAndBias)
{
    ExpectConservativeBoundsPreserveReferenceSwitchesAndBias();
}

// The dwell band on a reference mesh and on its conservatively expanded copy,
// which must pick identically. Inside the dwell region the previous level is
// held, and which level depends only on the history (same distance, previous 1
// holds LOD1, previous 0 holds LOD0). Outside the band the camera wins in both
// directions: the band delays a switch, it never prevents one. The gain and hold
// distances bracket the gain gate to +/-5%, so the band's width is pinned, not
// just its existence.
TEST_P(BatchScatterComputeTest, ConservativeBoundsPreserveReferenceHysteresis)
{
    GPUMesh reference = MakeHysteresisMesh();
    GPUMesh expanded = reference;
    expanded.boundingRadius = 4.0f;
    expanded.lodCoverageScale = 0.25f;
    for (const GPUMesh& row : {reference, expanded})
    {
        const uint32_t mesh = m_Scene->AddMesh(row);
        auto instance = MakeInstance(1u, mesh);
        instance.boundingRadius = row.boundingRadius;
        m_Scene->AddInstance(instance);
    }
    uint32_t total = 0;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);
    struct Sample { float distance; uint32_t previous; uint32_t expected; };
    const Sample samples[] = {{kInBandDist,1,1}, {kInBandDist,0,0}, {kNearDist,1,0},
                              {kBandGainDist,1,0}, {kBandHoldDist,1,1}, {kFarDist,0,1}};
    for (const Sample& sample : samples)
    {
        SCOPED_TRACE(::testing::Message() << "distance=" << sample.distance
                                         << " previous=" << sample.previous);
        SeedPrevLod(2u, sample.previous);
        LodDispatchParams lod{};
        lod.projScaleY = 1.0f;
        lod.cameraPos[2] = sample.distance;
        lod.hysteresisBand = kHystBand;
        const auto r = Dispatch(table, total, 0u, 0u, 2u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        ASSERT_EQ(r.stats.overflows, 0u);
        for (uint32_t slot = 0; slot < 2; ++slot)
        {
            ASSERT_EQ(r.cursors[slot], 1u);
            EXPECT_EQ(r.records[slot * kDrawRecordUints],
                      sample.expected == 0 ? kHystLod0Indices : kHystLod1Indices);
            EXPECT_EQ(r.prevLod[slot], sample.expected);
        }
    }
}

TEST_P(BatchScatterComputeTest, ConservativeBoundsPreserveSseReferenceAndCeiling)
{
    GPUMesh reference = MakeMultiLevelHysteresisMesh();
    constexpr float extent = 1.1547005f;
    const float errors[4] = {0, .06f, .12f, .25f};
    const uint8_t sloppy[4] = {};
    reference.lodFlags = DeriveLODThresholds(errors, sloppy, 4u, false, 1.0f, extent,
                                            kMultiThresholds, 4u, reference.lodThreshold);
    ASSERT_EQ(reference.lodFlags, 0b0111u);
    reference.lodSseScaleCeil = LodSseScaleCeil(1.0f, extent);
    GPUMesh expanded = reference;
    expanded.boundingRadius = 4.0f;
    expanded.lodCoverageScale = .25f;
    for (const GPUMesh& row : {reference, expanded})
    {
        const uint32_t mesh = m_Scene->AddMesh(row);
        auto instance = MakeInstance(1u, mesh);
        instance.boundingRadius = row.boundingRadius;
        m_Scene->AddInstance(instance);
    }
    uint32_t total = 0;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);
    bool ceilingMatters = false;
    bool allLevels[4] = {};
    for (uint32_t viewport : {256u, 1080u})
    {
        const float factor = LodSseThresholdToCoverage(viewport, kDefaultLodErrorBudgetPx);
        const auto select = [&](float coverage, float ceiling)
        {
            float previous = kLodEffNone;
            for (uint32_t level = 0; level < 4; ++level)
            {
                const float effective = LodEffectiveThreshold(reference.lodThreshold[level],
                    (reference.lodFlags & (1u << level)) != 0, factor, ceiling, previous);
                previous = effective;
                if (coverage >= effective) return level;
            }
            return 3u;
        };
        for (float coverage : {.45f, .30f, .20f, .10f, .05f, .01f})
        {
            SCOPED_TRACE(::testing::Message() << "viewport=" << viewport << " coverage=" << coverage);
            const uint32_t expected = select(coverage, reference.lodSseScaleCeil);
            const uint32_t uncapped = select(coverage, 0.0f);
            allLevels[expected] = true;
            ceilingMatters |= expected != uncapped;
            EXPECT_LE(expected, uncapped) << "the cap must never pick a coarser level";
            LodDispatchParams lod{};
            lod.projScaleY = 1;
            lod.cameraPos[2] = 1.0f / coverage;
            lod.sseThresholdToCoverage = factor;
            lod.sseThresholdToCoverageTight = factor;
            const auto r = Dispatch(table, total, 0u, 0u, 2u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
            ASSERT_EQ(r.stats.tableMisses, 0u);
            ASSERT_EQ(r.stats.overflows, 0u);
            for (uint32_t slot = 0; slot < 2; ++slot)
            {
                ASSERT_EQ(r.cursors[slot], 1u);
                EXPECT_EQ(r.records[slot * kDrawRecordUints], kMultiIndices[expected]);
            }
        }
    }
    EXPECT_TRUE(ceilingMatters);
    for (bool observed : allLevels) EXPECT_TRUE(observed);
}

TEST_P(BatchScatterComputeTest, DegenerateReferenceKeepsZeroCoverageAndForcedLod)
{
    GPUMesh reference = MakeMultiLevelHysteresisMesh();
    reference.boundingRadius = 0;
    GPUMesh expanded = reference;
    expanded.boundingRadius = 4;
    expanded.lodCoverageScale = 0;
    for (const GPUMesh& row : {reference, expanded})
    {
        const uint32_t mesh = m_Scene->AddMesh(row);
        auto instance = MakeInstance(1u, mesh);
        instance.boundingRadius = row.boundingRadius;
        m_Scene->AddInstance(instance);
    }
    uint32_t total = 0;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);
    for (uint32_t forced : {0xFFFFFFFFu, 0u, 2u, 99u})
    {
        SCOPED_TRACE(forced);
        LodDispatchParams lod{};
        lod.projScaleY = 1;
        lod.cameraPos[2] = .01f; // unscaled conservative coverage would select LOD0
        const uint32_t expected = forced == 0xFFFFFFFFu ? 3u : std::min(forced, 3u);
        const auto r = Dispatch(table, total, 0u, 0u, 2u, 2u, 0u, {}, forced, {}, 0u, lod);
        ASSERT_EQ(r.stats.tableMisses, 0u);
        ASSERT_EQ(r.stats.overflows, 0u);
        for (uint32_t slot = 0; slot < 2; ++slot)
        {
            ASSERT_EQ(r.cursors[slot], 1u);
            EXPECT_EQ(r.records[slot * kDrawRecordUints], kMultiIndices[expected]);
        }
    }
}

TEST_P(BatchScatterComputeTest, SmallObjectCullUsesConservativeCoverageBeforeReferenceSelection)
{
    GPUMesh reference = MakeMultiLevelHysteresisMesh();
    GPUMesh expanded = reference;
    expanded.boundingRadius = 4;
    expanded.lodCoverageScale = .25f;
    uint32_t meshIds[2];
    uint32_t i = 0;
    for (const GPUMesh& row : {reference, expanded})
    {
        meshIds[i++] = m_Scene->AddMesh(row);
        auto instance = MakeInstance(1u, meshIds[i - 1]);
        instance.boundingRadius = row.boundingRadius;
        m_Scene->AddInstance(instance);
    }
    uint32_t total = 0;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);
    ASSERT_EQ(table.size(), 2u);
    LodDispatchParams lod{};
    lod.projScaleY = 1;
    lod.cameraPos[2] = 12;
    lod.smallCullCoverage = .2f;
    const auto r = Dispatch(table, total, 0u, 0u, 2u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
    ASSERT_EQ(r.stats.tableMisses, 0u);
    ASSERT_EQ(r.stats.overflows, 0u);
    for (uint32_t row = 0; row < 2; ++row)
    {
        const bool survives = table[row].meshIndex == meshIds[1];
        EXPECT_EQ(r.cursors[row], survives ? 1u : 0u);
        const uint32_t record = table[row].recordOffset * kDrawRecordUints;
        EXPECT_EQ(r.records[record], survives ? kMultiIndices[3] : 0u);
    }
}

// Band off (0) must reproduce the stateless pick regardless of what the history
// buffer says. This is the A/B guarantee the measurement lanes depend on: a
// seeded history that WOULD change the answer must be ignored entirely.
TEST_P(BatchScatterComputeTest, LodBandOffIgnoresHistoryEntirely)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // Seed "was coarse". At kInBandDist the bare threshold is cleared, so the
    // stateless rule says LOD0 — and with the band off that must be the answer.
    SeedPrevLod(1u, 1u);
    LodDispatchParams lod{};
    lod.projScaleY     = 1.0f;
    lod.cameraPos[0]   = kInBandDist;
    lod.hysteresisBand = 0.0f;
    const auto r = Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.records[0], kHystLod0Indices);
    // Band off must not write history either — the buffer stays as seeded.
    EXPECT_EQ(r.prevLod[0], 1u);
}

// No usable history (first touch, a grown range, or a slot recycled to a mesh
// with fewer levels) must fall back to the stateless pick rather than biasing
// coarse. kLodNoHistory and an out-of-range level both mean "no history".
TEST_P(BatchScatterComputeTest, LodBandWithoutUsableHistoryTakesTheStatelessPick)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    LodDispatchParams lod{};
    lod.projScaleY     = 1.0f;
    lod.cameraPos[0]   = kInBandDist; // stateless says LOD0
    lod.hysteresisBand = kHystBand;

    for (const uint32_t seed : {GPUDrawStreamBuilder::kLodNoHistory, 2u, 7u})
    {
        SeedPrevLod(1u, seed);
        const auto r = Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
        EXPECT_EQ(r.records[0], kHystLod0Indices) << "seed=" << seed;
        EXPECT_EQ(r.prevLod[0], 0u) << "seed=" << seed;
    }
}

// The band must reach the level the coverage allows in ONE step, not creep one
// level per frame. A ratchet would both lag a fast approach and destroy the
// fixed-point property the elision gate depends on. Needs >2 levels to be
// distinguishable at all: from prev=3 a clamp lands on 0, a ratchet on 2.
TEST_P(BatchScatterComputeTest, LodBandJumpsAllRequiredLevelsInOneStep)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeMultiLevelHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    LodDispatchParams lod{};
    lod.projScaleY     = 1.0f;
    lod.cameraPos[0]   = kMultiNearDist;
    lod.hysteresisBand = kHystBand;

    SeedPrevLod(1u, 3u); // was the coarsest level
    const auto r = Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.records[0], kMultiIndices[0]) << "expected a 3-level jump to LOD0, not a 1-level step";
    EXPECT_EQ(r.prevLod[0], 0u);
}

// The band is a FIXED POINT: feeding its own output back as history returns that
// output unchanged. Two things depend on this — a second slice of the same view
// re-selecting from the updated history cannot ratchet another level, and the
// idle-elision gate (which stops dispatching once its inputs settle) cannot
// freeze a half-converged level. Verified by re-dispatching at a FIXED camera
// and requiring the level to stop moving after the first frame.
TEST_P(BatchScatterComputeTest, LodBandConvergesToAFixedPointInOneStep)
{
    // Both chain shapes: the 2-level mesh covers the ordinary boundary, and the
    // 4-level mesh is the only one that can catch a per-frame ratchet (which
    // would keep moving on the second dispatch instead of resting).
    const uint32_t twoLevel  = m_Scene->AddMesh(MakeHysteresisMesh());
    const uint32_t fourLevel = m_Scene->AddMesh(MakeMultiLevelHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, twoLevel));
    m_Scene->AddInstance(MakeInstance(2u, fourLevel));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);

    LodDispatchParams lod{};
    lod.projScaleY     = 1.0f;
    lod.hysteresisBand = kHystBand;

    for (const float dist : {kMultiNearDist, kNearDist, kInBandDist, kFarDist})
    {
        for (const uint32_t seed : {GPUDrawStreamBuilder::kLodNoHistory, 0u, 1u, 3u})
        {
            SeedPrevLod(2u, seed);
            lod.cameraPos[0] = dist;
            const auto first = Dispatch(table, total, 0u, 0u, 2u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
            const auto second = Dispatch(table, total, 0u, 0u, 2u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
            for (uint32_t i = 0; i < 2u; ++i)
            {
                EXPECT_EQ(second.records[i * kDrawRecordUints], first.records[i * kDrawRecordUints])
                    << "dist=" << dist << " seed=" << seed << " rec=" << i;
                EXPECT_EQ(second.prevLod[i], first.prevLod[i])
                    << "dist=" << dist << " seed=" << seed << " inst=" << i;
            }
        }
    }
}

// The defining property: history is per (instance, view). Every other band test
// runs 1-2 instances in ONE workgroup with a UNIFORM seed, which makes
// prevLod[instanceIndex] indistinguishable from prevLod[slot],
// prevLod[batchId], prevLod[localSlot] and prevLod[gl_LocalInvocationID.x].
// This case separates all five: >64 instances (two workgroups), two batches, a
// non-zero record base, and a per-index seed whose second workgroup is INVERTED
// relative to the local ids it would alias.
//
// At kInBandDist every seeded level is a fixed point, so the written-back
// history must come back byte-identical to the seed — an order-independent
// assertion that does not depend on the atomic's slot allocation order.
void BatchScatterComputeTest::ExpectLodBandHistoryIsIndexedPerInstanceNotPerSlot()
{
    constexpr uint32_t kInstances     = 66u; // > 64: forces a second workgroup
    constexpr uint32_t kPerBatch      = kInstances / 2u;
    constexpr uint32_t kRecordBase    = 5u;  // non-zero: separates slot from index
    constexpr uint32_t kCursorBase    = 2u;
    constexpr uint32_t kWorkgroupSize = 64u;

    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    for (uint32_t i = 0; i < kInstances; ++i)
        m_Scene->AddInstance(MakeInstance(i < kPerBatch ? 1u : 2u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, kInstances);
    ASSERT_EQ(table.size(), 2u);

    // Alternating parity defeats a batch- or slot-indexed read; inverting the
    // second workgroup defeats a gl_LocalInvocationID-indexed one.
    std::vector<uint32_t> seed(kInstances);
    for (uint32_t i = 0; i < kInstances; ++i)
        seed[i] = ((i & 1u) ^ (i / kWorkgroupSize)) & 1u;
    SeedPrevLod(seed);

    LodDispatchParams lod{};
    lod.projScaleY     = 1.0f;
    lod.cameraPos[0]   = kInBandDist;
    lod.hysteresisBand = kHystBand;

    const auto r = Dispatch(table, total, kCursorBase, kRecordBase,
                            /*cursorSlots=*/kCursorBase + 2u,
                            /*recordSlots=*/kRecordBase + kInstances,
                            0u, {}, 0xFFFFFFFFu, {}, 0u, lod);
    ASSERT_EQ(r.stats.tableMisses, 0u);
    ASSERT_EQ(r.stats.overflows, 0u);

    for (uint32_t i = 0; i < kInstances; ++i)
        EXPECT_EQ(r.prevLod[i], seed[i]) << "history must be indexed per instance; inst=" << i;

    // The READ path, exactly. indirection[slot] carries the instance that won
    // that slot, so each emitted record can be checked against ITS OWN seed
    // rather than against a population count — a count is satisfiable by a
    // per-batch read that happens to split the same way.
    for (uint32_t s = kRecordBase; s < kRecordBase + kInstances; ++s)
    {
        const uint32_t inst = r.indirection[s];
        ASSERT_LT(inst, kInstances) << "record slot " << s << " claims no instance";
        const uint32_t expected = seed[inst] == 0u ? kHystLod0Indices : kHystLod1Indices;
        EXPECT_EQ(r.records[s * kDrawRecordUints + 0], expected)
            << "slot " << s << " belongs to instance " << inst << " (seed " << seed[inst] << ")";
    }
}

TEST_P(BatchScatterComputeTest, LodBandHistoryIsIndexedPerInstanceNotPerSlot)
{
    ExpectLodBandHistoryIsIndexedPerInstanceNotPerSlot();
}

// Draw consolidation: with meshGroupMode=1 the shader routes the batch search's
// mesh axis through meshPoolGroup[], so meshes sharing a geometry-bind identity
// land in ONE region behind ONE cursor — the count one DrawIndexedIndirectCount
// consumes. Records stay self-contained (each carries its own mesh's index
// range/vertex offset), which is exactly what lets the region mix meshes.
TEST_P(BatchScatterComputeTest, GroupModeMergesMultiMeshGroupIntoOneRegion)
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);

    std::vector<uint32_t> all;
    all.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));
    all.push_back(m_Scene->AddInstance(MakeInstance(1u, meshB)));
    all.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));

    // Both meshes share pool group 7 (one synthetic geometry-bind identity).
    std::vector<uint32_t> groups(m_Scene->GetMeshes().size(), 0xFFFFFFu);
    groups[meshA] = 7u;
    groups[meshB] = 7u;

    uint32_t total = 0u;
    const auto table =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), groups, &total);
    ASSERT_EQ(table.size(), 1u); // one (mat 1, group 7) row
    ASSERT_EQ(table[0].meshIndex, 7u);
    ASSERT_EQ(table[0].capacity, 3u);

    const auto r = Dispatch(table, total, /*cursorBase=*/0u, /*recordBase=*/0u,
                            /*cursorSlots=*/1u, /*recordSlots=*/total,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, groups, /*meshGroupMode=*/1u);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 3u) << "count compaction: one cursor bounds the whole group";

    // Records interleave by arrival within the group range, but each stays
    // self-contained; the indirection set must cover exactly the instances.
    std::multiset<uint32_t> counts;
    std::set<uint32_t> targets;
    for (uint32_t s = 0; s < 3u; ++s)
    {
        counts.insert(r.records[s * kDrawRecordUints + 0]);
        EXPECT_EQ(r.records[s * kDrawRecordUints + 1], 1u); // instanceCount
        EXPECT_EQ(r.records[s * kDrawRecordUints + 4], s);  // firstInstance = slot
        targets.insert(r.indirection[s]);
    }
    EXPECT_EQ(counts, (std::multiset<uint32_t>{6u, 6u, 12u}));
    EXPECT_EQ(targets, std::set<uint32_t>(all.begin(), all.end()));
}

// Kill-switch parity: the SAME scene scattered per-bucket (mode 0) and
// consolidated (mode 1) must emit IDENTICAL draw SETS — only the slot layout
// (which region a record lands in) may differ. This is the
// GE_DRAW_CONSOLIDATION A/B contract at the record level.
TEST_P(BatchScatterComputeTest, GroupModeKillSwitchParityIdenticalDrawSets)
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);
    const uint32_t meshC = AddMeshRow(18u);

    m_Scene->AddInstance(MakeInstance(1u, meshA));
    m_Scene->AddInstance(MakeInstance(1u, meshB));
    m_Scene->AddInstance(MakeInstance(2u, meshB));
    m_Scene->AddInstance(MakeInstance(2u, meshC));
    m_Scene->AddInstance(MakeInstance(1u, meshA));

    std::vector<uint32_t> groups(m_Scene->GetMeshes().size(), 0xFFFFFFu);
    groups[meshA] = 0u;
    groups[meshB] = 0u; // A+B share a group
    groups[meshC] = 1u;

    // A draw's identity: (indexCount, firstIndex, vertexOffset, sourceInstance).
    auto collectDraws = [&](const std::vector<GPUDrawStreamBuilder::BatchTableEntry>& table,
                            uint32_t total, const std::vector<uint32_t>& map,
                            uint32_t mode)
    {
        const auto r = Dispatch(table, total, 0u, 0u,
                                /*cursorSlots=*/static_cast<uint32_t>(table.size()),
                                /*recordSlots=*/total, 0u, {}, 0xFFFFFFFFu, map, mode);
        EXPECT_EQ(r.stats.tableMisses, 0u);
        EXPECT_EQ(r.stats.overflows, 0u);
        std::multiset<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> draws;
        uint32_t consumed = 0u;
        for (uint32_t c : r.cursors)
            consumed += c;
        EXPECT_EQ(consumed, total) << "every live instance must emit exactly one record";
        for (const auto& e : table)
        {
            const uint32_t base  = e.recordOffset;
            // The region's live records are the first cursor-count entries; with
            // full visibility every capacity slot is populated.
            for (uint32_t s = base; s < base + e.capacity; ++s)
            {
                draws.insert({r.records[s * kDrawRecordUints + 0],
                              r.records[s * kDrawRecordUints + 2],
                              static_cast<uint32_t>(r.records[s * kDrawRecordUints + 3]),
                              r.indirection[s]});
            }
        }
        return draws;
    };

    uint32_t totalOff = 0u, totalOn = 0u;
    const auto tableOff =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &totalOff);
    const auto tableOn =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), groups, &totalOn);
    ASSERT_EQ(totalOff, totalOn);
    ASSERT_EQ(tableOff.size(), 4u); // per (mat, mesh): (1,A) (1,B) (2,B) (2,C)
    ASSERT_EQ(tableOn.size(), 3u);  // (1,g0) (2,g0) (2,g1)

    const auto drawsOff = collectDraws(tableOff, totalOff, {}, 0u);
    const auto drawsOn  = collectDraws(tableOn, totalOn, groups, 1u);
    EXPECT_EQ(drawsOff, drawsOn) << "consolidation must not add, drop, or alter any draw";
}

// Shadow twin: grouped tables + classMode Shadow route eligible casters of
// DIFFERENT meshes (same pool group) into one sentinel-group region.
TEST_P(BatchScatterComputeTest, GroupModeShadowMergesEligibleAcrossMeshes)
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);

    std::vector<uint32_t> ssIdx;
    ssIdx.push_back(m_Scene->AddInstance(MakeInstance(0u, meshA, 0u)));
    ssIdx.push_back(m_Scene->AddInstance(MakeInstance(0u, meshB, 0u)));
    ssIdx.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA, 0u)));

    const std::vector<uint8_t> classes{
        static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided),
        static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided)};
    std::vector<uint32_t> groups(m_Scene->GetMeshes().size(), 0xFFFFFFu);
    groups[meshA] = 4u;
    groups[meshB] = 4u;

    uint32_t total = 0u;
    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(
        m_Scene->GetBatchRegistry(), classes, groups, &total);
    ASSERT_EQ(shadow.size(), 1u); // one (SS sentinel, group 4) row
    ASSERT_EQ(shadow[0].materialIndex, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel);
    ASSERT_EQ(shadow[0].capacity, 3u);

    const auto r = Dispatch(shadow, total, 0u, 0u, /*cursorSlots=*/1u, /*recordSlots=*/total,
                            GPUDrawStreamBuilder::kClassModeShadow, {}, 0xFFFFFFFFu,
                            groups, /*meshGroupMode=*/1u);
    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 3u);
    std::set<uint32_t> got{r.indirection[0], r.indirection[1], r.indirection[2]};
    EXPECT_EQ(got, std::set<uint32_t>(ssIdx.begin(), ssIdx.end()));
}

// ---- Mesh-coherent group compaction (draw_stream_group_compact.comp) -------
//
// Production sequence on real GPU buffers: scatter into the STAGING block
// (per-mesh ordered rows), barrier, compact into the DRAW block (each
// (class, group, parity) run packed densely, mesh-major, with per-run counts).
// This is the GPU-busy fix's contract: consumers execute long same-mesh
// record runs, never scatter-arrival interleave, and a run's count equals its
// live records exactly (no capacity-bound ghost draws).

// Per-run compact output the tests assert against.
struct GroupedCompactResult
{
    std::vector<uint32_t> cursors;      // rows + runs (counts at [rows, rows+runs))
    std::vector<uint32_t> records;      // raw uints, 5 per record; draw block at recordSlots
    std::vector<uint32_t> indirection;  // staging + draw blocks
    ScatterSliceStats     stats{};
};

class GroupCompactComputeTest : public BatchScatterComputeTest
{
  protected:
    // Rows -> {runStart, runIndex} per row + run count, mirroring the
    // scheduler's run detection (class, group-of-ordered-axis, parity).
    struct RunLayout
    {
        std::vector<uint32_t> rowRun; // 2 uints per row
        uint32_t runCount = 0;
    };
    static RunLayout ComputeRuns(const std::vector<GPUDrawStreamBuilder::BatchTableEntry>& table,
                                 const std::vector<uint32_t>& orderedToGroup)
    {
        constexpr uint32_t kParityBit = 0x1000000u;
        auto groupOf = [&](const GPUDrawStreamBuilder::BatchTableEntry& e)
        {
            const uint32_t ordered = e.meshIndex & ~kParityBit;
            return ordered < orderedToGroup.size() ? orderedToGroup[ordered] : 0xFFFFFFu;
        };
        RunLayout out;
        out.rowRun.resize(table.size() * 2u);
        uint32_t runStart = 0u;
        for (uint32_t r = 0; r < static_cast<uint32_t>(table.size()); ++r)
        {
            const bool newRun = r == 0u
                || table[r].materialIndex != table[runStart].materialIndex
                || ((table[r].meshIndex ^ table[runStart].meshIndex) & kParityBit) != 0u
                || groupOf(table[r]) != groupOf(table[runStart]);
            if (newRun)
            {
                runStart = r;
                ++out.runCount;
            }
            out.rowRun[r * 2u + 0u] = runStart;
            out.rowRun[r * 2u + 1u] = out.runCount - 1u;
        }
        return out;
    }

    // Scatter (staging block) + barrier + compact (draw block) in ONE command
    // list — the production sequence. `visible` empty = all pass; otherwise
    // one uint per instance.
    GroupedCompactResult DispatchGroupedCompact(
        const std::vector<GPUDrawStreamBuilder::BatchTableEntry>& table,
        const RunLayout& runs, uint32_t totalRecords,
        const std::vector<uint32_t>& meshOrdered, const std::vector<uint32_t>& visible)
    {
        GroupedCompactResult result;
        PipelineHandle compactPipeline = m_Builder->GetOrCreateGroupCompactPipeline();
        if (!compactPipeline.IsValid())
            return result; // the caller fails the test

        m_Scene->FlushGPUBuffers();
        const uint32_t instanceCount = m_Scene->GetInstanceCount();
        const uint32_t frameIndex    = m_Scene->GetFrameIndex();
        const uint32_t rowCount      = static_cast<uint32_t>(table.size());
        const uint32_t cursorSlots   = rowCount + runs.runCount;
        const uint32_t recordSlots   = totalRecords; // per block; draw block appended

        BufferHandle visibility = MakeStorageBuffer(
            std::max<size_t>(instanceCount, 1u) * sizeof(uint32_t), "Test.Visibility");
        if (!visible.empty())
            WriteBuffer(visibility, visible.data(), visible.size() * sizeof(uint32_t));
        BufferHandle tableBuf = MakeStorageBuffer(
            rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry), "Test.BatchTable");
        WriteBuffer(tableBuf, table.data(),
                    rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry));
        BufferHandle rowRunBuf =
            MakeStorageBuffer(runs.rowRun.size() * sizeof(uint32_t), "Test.RowRun");
        WriteBuffer(rowRunBuf, runs.rowRun.data(), runs.rowRun.size() * sizeof(uint32_t));

        BufferHandle cursors = MakeStorageBuffer(cursorSlots * sizeof(uint32_t),
                                                 "Test.Cursors", /*indirect=*/true);
        const std::vector<uint32_t> zeroCursors(cursorSlots, 0u);
        WriteBuffer(cursors, zeroCursors.data(), cursorSlots * sizeof(uint32_t));

        // Staging block [0, recordSlots), draw block [recordSlots, 2*recordSlots).
        BufferHandle records = MakeStorageBuffer(
            2u * recordSlots * kDrawRecordUints * sizeof(uint32_t), "Test.Records", true);
        const std::vector<uint32_t> poisonRecords(2u * recordSlots * kDrawRecordUints,
                                                  0xAAAAAAAAu);
        WriteBuffer(records, poisonRecords.data(), poisonRecords.size() * sizeof(uint32_t));
        BufferHandle indirection = MakeStorageBuffer(2u * recordSlots * sizeof(uint32_t),
                                                     "Test.Indirection");
        const std::vector<uint32_t> poisonIndirection(2u * recordSlots, 0xDEADBEEFu);
        WriteBuffer(indirection, poisonIndirection.data(),
                    poisonIndirection.size() * sizeof(uint32_t));

        BufferHandle stats = MakeStorageBuffer(sizeof(ScatterSliceStats), "Test.Stats");
        const ScatterSliceStats zeroStats{};
        WriteBuffer(stats, &zeroStats, sizeof(zeroStats));
        BufferHandle colorClass = MakeStorageBuffer(sizeof(uint32_t), "Test.MaterialColorClass");
        BufferHandle poolGroup = MakeStorageBuffer(
            std::max<size_t>(meshOrdered.size(), 1u) * sizeof(uint32_t), "Test.MeshOrdered");
        if (!meshOrdered.empty())
            WriteBuffer(poolGroup, meshOrdered.data(), meshOrdered.size() * sizeof(uint32_t));

        DescriptorSetDesc scatterDesc{};
        scatterDesc.layout    = GPUDrawStreamBuilder::MakeScatterDescriptorSetLayout();
        scatterDesc.transient = true;
        scatterDesc.debugName = "Test.ScatterDS";
        DescriptorSetHandle scatterDS = m_Device->CreateDescriptorSet(scatterDesc);
        m_Device->UpdateStorageBufferBinding(scatterDS, 0, visibility, 0,
                                             std::max<size_t>(instanceCount, 1u) * sizeof(uint32_t));
        if (UseCompact())
            m_Device->UpdateStorageBufferBinding(
                scatterDS, 1, m_Scene->GetScatterHotBufferForFrame(frameIndex), 0,
                std::max<size_t>(instanceCount, 1u) * sizeof(GPUInstanceScatterHot));
        else
            m_Device->UpdateStorageBufferBinding(
                scatterDS, 1, m_Scene->GetInstanceBufferForFrame(frameIndex), 0,
                std::max<size_t>(instanceCount, 1u) * sizeof(GPUInstance));
        m_Device->UpdateStorageBufferBinding(scatterDS, 2, m_Scene->GetMeshBuffer(), 0,
                                             std::max<size_t>(m_Scene->GetMeshes().size(), size_t(1)) * sizeof(GPUMesh));
        m_Device->UpdateStorageBufferBinding(scatterDS, 3, tableBuf, 0,
                                             rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry));
        m_Device->UpdateStorageBufferBinding(scatterDS, 4, records, 0,
                                             2u * recordSlots * kDrawRecordUints * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(scatterDS, 5, cursors, 0,
                                             cursorSlots * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(scatterDS, 6, indirection, 0,
                                             2u * recordSlots * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(scatterDS, 7, stats, 0, sizeof(ScatterSliceStats));
        m_Device->UpdateStorageBufferBinding(scatterDS, 8, colorClass, 0, sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(scatterDS, 9, poolGroup, 0,
                                             std::max<size_t>(meshOrdered.size(), 1u) * sizeof(uint32_t));
        // The per-instance history bindings (10..13) are never read here — this
        // fixture runs no band, no fade and no rendered history — but the shader
        // declares all four, so every one of them gets a real buffer rather than
        // an unwritten descriptor. The production path binds a stand-in for the
        // same reason.
        const uint32_t historyCount = std::max(instanceCount, 1u);
        if (!m_PrevLod.IsValid() || m_PrevLodCount < historyCount)
            SeedPrevLod(historyCount, GPUDrawStreamBuilder::kLodNoHistory);
        if (!m_LodFade.IsValid() || m_LodFadeCount < historyCount)
            SeedLodFade(historyCount, 0u, 0.0f);
        if (!m_Rendered[0].IsValid() || m_RenderedCount < historyCount)
            SeedRenderedHistory(
                std::vector<uint32_t>(historyCount, GPUDrawStreamBuilder::kLodNoHistory));
        m_Device->UpdateStorageBufferBinding(scatterDS, 10, m_PrevLod, 0,
                                             historyCount * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(scatterDS, 11, m_LodFade, 0,
                                             historyCount * sizeof(LodFadeState));
        m_Device->UpdateStorageBufferBinding(scatterDS, 12, m_Rendered[1u - m_RenderedCurrentIndex],
                                             0, historyCount * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(scatterDS, 13, m_Rendered[m_RenderedCurrentIndex], 0,
                                             historyCount * sizeof(uint32_t));

        DescriptorSetDesc compactDesc{};
        compactDesc.layout    = GPUDrawStreamBuilder::MakeGroupCompactDescriptorSetLayout();
        compactDesc.transient = true;
        compactDesc.debugName = "Test.GroupCompactDS";
        DescriptorSetHandle compactDS = m_Device->CreateDescriptorSet(compactDesc);
        m_Device->UpdateStorageBufferBinding(compactDS, 0, tableBuf, 0,
                                             rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry));
        m_Device->UpdateStorageBufferBinding(compactDS, 1, rowRunBuf, 0,
                                             runs.rowRun.size() * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(compactDS, 2, cursors, 0,
                                             cursorSlots * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(compactDS, 3, records, 0,
                                             2u * recordSlots * kDrawRecordUints * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(compactDS, 4, indirection, 0,
                                             2u * recordSlots * sizeof(uint32_t));

        GPUDrawStreamBuilder::ScatterPushConstants pc{};
        pc.instanceCount   = instanceCount;
        pc.batchCount      = rowCount;
        pc.cursorBase      = 0u;
        pc.recordBase      = 0u; // staging block
        pc.disableVisCheck = visible.empty() ? 1u : 0u;
        pc.viewWorldKey    = 0u;
        pc.forceLod        = 0xFFFFFFFFu;
        pc.classMode       = 0u;
        pc.statsBase       = 0u;
        pc.meshGroupMode   = 1u;

        GPUDrawStreamBuilder::GroupCompactPushConstants cpc{};
        cpc.recordCount   = totalRecords;
        cpc.rowCount      = rowCount;
        cpc.cursorBase    = 0u;
        cpc.countBase     = rowCount;
        cpc.srcRecordBase = 0u;
        cpc.dstRecordBase = recordSlots;

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, scatterDS, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((instanceCount + 63u) / 64u, 1, 1);
        cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead)
                | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));
        cl->SetPipeline(compactPipeline);
        cl->BindDescriptorSet(0, compactDS, compactPipeline);
        cl->SetPushConstants(cpc);
        cl->Dispatch((totalRecords + 63u) / 64u, 1, 1);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        result.cursors     = ReadBuffer<uint32_t>(cursors, cursorSlots);
        result.records     = ReadBuffer<uint32_t>(records, 2u * recordSlots * kDrawRecordUints);
        result.indirection = ReadBuffer<uint32_t>(indirection, 2u * recordSlots);
        auto statsVec      = ReadBuffer<uint32_t>(stats, 4);
        result.stats       = {statsVec[0], statsVec[1], statsVec[2], statsVec[3]};
        return result;
    }

    // Compact pass ONLY, over hand-seeded cursors/staging — no scatter, no
    // scene. This is what lets a test drive the snapshot-overflow clamp
    // directly: cursor values ABOVE a row's capacity (the state an overflow
    // tripwire leaves behind) with staging content only inside the capacity
    // window. `cursorSeed` is rowCount + runCount uints; `stagingUints` /
    // `stagingIndirection` cover [0, totalRecords) — the draw block starts
    // poisoned (0xAAAAAAAA).
    GroupedCompactResult DispatchCompactOnly(
        const std::vector<GPUDrawStreamBuilder::BatchTableEntry>& table,
        const RunLayout& runs, uint32_t totalRecords,
        const std::vector<uint32_t>& cursorSeed, const std::vector<uint32_t>& stagingUints,
        const std::vector<uint32_t>& stagingIndirection,
        uint32_t pairedRecords = 0u)
    {
        GroupedCompactResult result;
        PipelineHandle compactPipeline = m_Builder->GetOrCreateGroupCompactPipeline();
        if (!compactPipeline.IsValid())
            return result; // the caller fails the test

        const uint32_t rowCount    = static_cast<uint32_t>(table.size());
        const uint32_t cursorSlots = rowCount + runs.runCount;
        const uint32_t recordSlots = totalRecords;
        EXPECT_EQ(cursorSeed.size(), cursorSlots);
        EXPECT_EQ(stagingUints.size(), recordSlots * kDrawRecordUints);
        EXPECT_EQ(stagingIndirection.size(), recordSlots);

        BufferHandle tableBuf = MakeStorageBuffer(
            rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry), "Test.BatchTable");
        WriteBuffer(tableBuf, table.data(),
                    rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry));
        BufferHandle rowRunBuf =
            MakeStorageBuffer(runs.rowRun.size() * sizeof(uint32_t), "Test.RowRun");
        WriteBuffer(rowRunBuf, runs.rowRun.data(), runs.rowRun.size() * sizeof(uint32_t));
        BufferHandle cursors = MakeStorageBuffer(cursorSlots * sizeof(uint32_t),
                                                 "Test.Cursors", /*indirect=*/true);
        WriteBuffer(cursors, cursorSeed.data(), cursorSlots * sizeof(uint32_t));

        BufferHandle records = MakeStorageBuffer(
            2u * recordSlots * kDrawRecordUints * sizeof(uint32_t), "Test.Records", true);
        std::vector<uint32_t> recordSeed(2u * recordSlots * kDrawRecordUints, 0xAAAAAAAAu);
        std::copy(stagingUints.begin(), stagingUints.end(), recordSeed.begin());
        WriteBuffer(records, recordSeed.data(), recordSeed.size() * sizeof(uint32_t));
        BufferHandle indirection = MakeStorageBuffer(2u * recordSlots * sizeof(uint32_t),
                                                     "Test.Indirection");
        std::vector<uint32_t> indirectionSeed(2u * recordSlots, 0xDEADBEEFu);
        std::copy(stagingIndirection.begin(), stagingIndirection.end(),
                  indirectionSeed.begin());
        WriteBuffer(indirection, indirectionSeed.data(),
                    indirectionSeed.size() * sizeof(uint32_t));

        DescriptorSetDesc compactDesc{};
        compactDesc.layout    = GPUDrawStreamBuilder::MakeGroupCompactDescriptorSetLayout();
        compactDesc.transient = true;
        compactDesc.debugName = "Test.GroupCompactOnlyDS";
        DescriptorSetHandle compactDS = m_Device->CreateDescriptorSet(compactDesc);
        m_Device->UpdateStorageBufferBinding(compactDS, 0, tableBuf, 0,
                                             rowCount * sizeof(GPUDrawStreamBuilder::BatchTableEntry));
        m_Device->UpdateStorageBufferBinding(compactDS, 1, rowRunBuf, 0,
                                             runs.rowRun.size() * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(compactDS, 2, cursors, 0,
                                             cursorSlots * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(compactDS, 3, records, 0,
                                             2u * recordSlots * kDrawRecordUints * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(compactDS, 4, indirection, 0,
                                             2u * recordSlots * sizeof(uint32_t));

        GPUDrawStreamBuilder::GroupCompactPushConstants cpc{};
        cpc.recordCount   = totalRecords;
        cpc.rowCount      = rowCount;
        cpc.cursorBase    = 0u;
        cpc.countBase     = rowCount;
        cpc.srcRecordBase = 0u;
        cpc.dstRecordBase = recordSlots;
        cpc.pairedRecords = pairedRecords;

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(compactPipeline);
        cl->BindDescriptorSet(0, compactDS, compactPipeline);
        cl->SetPushConstants(cpc);
        cl->Dispatch((totalRecords + 63u) / 64u, 1, 1);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        result.cursors     = ReadBuffer<uint32_t>(cursors, cursorSlots);
        result.records     = ReadBuffer<uint32_t>(records, 2u * recordSlots * kDrawRecordUints);
        result.indirection = ReadBuffer<uint32_t>(indirection, 2u * recordSlots);
        return result;
    }
};

// Full visibility: the draw block packs each run mesh-major (row order), the
// per-run counts equal the run capacities, and every compacted record is
// self-consistent (firstInstance == its slot; indirection targets an instance
// of the record's mesh).
TEST_P(GroupCompactComputeTest, PacksRunsMeshMajorWithExactCounts)
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);
    const uint32_t meshC = AddMeshRow(18u);

    // Group 0 = {A, B} (ordered ranks 0, 1), group 1 = {C} (rank 2).
    // Interleave registration so staging arrival order crosses meshes.
    std::vector<uint32_t> instMesh; // instanceIndex -> mesh
    for (int i = 0; i < 4; ++i)
    {
        instMesh.push_back(meshA); m_Scene->AddInstance(MakeInstance(1u, meshA));
        instMesh.push_back(meshB); m_Scene->AddInstance(MakeInstance(1u, meshB));
        instMesh.push_back(meshC); m_Scene->AddInstance(MakeInstance(1u, meshC));
    }

    std::vector<uint32_t> meshOrdered(m_Scene->GetMeshes().size(), 0xFFFFFFu);
    meshOrdered[meshA] = 0u;
    meshOrdered[meshB] = 1u;
    meshOrdered[meshC] = 2u;
    const std::vector<uint32_t> orderedToGroup{0u, 0u, 1u};

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(
        m_Scene->GetBatchRegistry(), meshOrdered, &total);
    ASSERT_EQ(table.size(), 3u); // per-mesh rows survive the ordered axis
    ASSERT_EQ(total, 12u);
    const auto runs = ComputeRuns(table, orderedToGroup);
    ASSERT_EQ(runs.runCount, 2u); // (mat1, g0) spanning rows 0-1; (mat1, g1) row 2

    const auto r = DispatchGroupedCompact(table, runs, total, meshOrdered, {});
    if (r.cursors.empty())
        FAIL() << "draw_stream_group_compact.shaderpkg unavailable";

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    // Per-run counts: g0 = A+B = 8, g1 = C = 4.
    EXPECT_EQ(r.cursors[3], 8u);
    EXPECT_EQ(r.cursors[4], 4u);

    // Draw block layout: run 0 at [0,8) — 4 meshA records THEN 4 meshB records
    // (mesh-major, row order); run 1 at [8,12) — 4 meshC records.
    const std::vector<uint32_t> expectedIndexCounts{6u, 6u, 6u, 6u, 12u, 12u, 12u, 12u,
                                                    18u, 18u, 18u, 18u};
    const uint32_t drawBase = total; // records block 1
    for (uint32_t s = 0; s < total; ++s)
    {
        const uint32_t* rec = &r.records[(drawBase + s) * kDrawRecordUints];
        EXPECT_EQ(rec[0], expectedIndexCounts[s]) << "slot " << s;
        EXPECT_EQ(rec[1], 1u) << "instanceCount, slot " << s;
        EXPECT_EQ(rec[4], drawBase + s) << "firstInstance must be the dst slot";
        const uint32_t src = r.indirection[drawBase + s];
        ASSERT_LT(src, instMesh.size());
        // Indirection target's mesh matches the record's geometry.
        EXPECT_EQ(m_Scene->GetMeshes()[instMesh[src]].indexCount, rec[0]) << "slot " << s;
    }
}

// Partial visibility: culled instances leave NO holes — each run packs its
// survivors densely from its base and its count shrinks to the live total, so
// a count-clamped merged draw touches only live records.
TEST_P(GroupCompactComputeTest, CulledInstancesCompactDenselyWithShrunkCounts)
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);

    std::vector<uint32_t> all;
    for (int i = 0; i < 3; ++i)
    {
        all.push_back(m_Scene->AddInstance(MakeInstance(1u, meshA)));
        all.push_back(m_Scene->AddInstance(MakeInstance(1u, meshB)));
    }

    std::vector<uint32_t> meshOrdered(m_Scene->GetMeshes().size(), 0xFFFFFFu);
    meshOrdered[meshA] = 0u;
    meshOrdered[meshB] = 1u;
    const std::vector<uint32_t> orderedToGroup{0u, 0u};

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(
        m_Scene->GetBatchRegistry(), meshOrdered, &total);
    ASSERT_EQ(table.size(), 2u);
    ASSERT_EQ(total, 6u);
    const auto runs = ComputeRuns(table, orderedToGroup);
    ASSERT_EQ(runs.runCount, 1u);

    // Cull meshA's first instance and meshB's last: 2xA + 2xB survive.
    std::vector<uint32_t> visible(m_Scene->GetInstanceCount(), 1u);
    visible[all[0]] = 0u;
    visible[all[5]] = 0u;

    const auto r = DispatchGroupedCompact(table, runs, total, meshOrdered, visible);
    if (r.cursors.empty())
        FAIL() << "draw_stream_group_compact.shaderpkg unavailable";

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 2u) << "meshA row live count";
    EXPECT_EQ(r.cursors[1], 2u) << "meshB row live count";
    EXPECT_EQ(r.cursors[2], 4u) << "run count = live total, not capacity";

    // Dense mesh-major: [A, A, B, B] from the run base — no capacity holes.
    const uint32_t drawBase = total;
    const std::vector<uint32_t> expectedIndexCounts{6u, 6u, 12u, 12u};
    for (uint32_t s = 0; s < 4u; ++s)
    {
        const uint32_t* rec = &r.records[(drawBase + s) * kDrawRecordUints];
        EXPECT_EQ(rec[0], expectedIndexCounts[s]) << "slot " << s;
        EXPECT_EQ(rec[4], drawBase + s);
        const uint32_t src = r.indirection[drawBase + s];
        EXPECT_NE(src, all[0]) << "culled instance must not appear";
        EXPECT_NE(src, all[5]) << "culled instance must not appear";
    }
    // Slots past the live total stay untouched (poison) — the count clamp is
    // what keeps them out of the draw, not a zero-fill.
    EXPECT_EQ(r.records[(drawBase + 4u) * kDrawRecordUints], 0xAAAAAAAAu);
}

// Kill-switch parity for the SHIPPED consolidated path: the same scene
// scattered per-bucket (mode 0) and through ordered-axis + compact must emit
// IDENTICAL draw SETS — the compact pass may only relocate records, never
// add, drop, or alter one. Draw identity: (indexCount, firstIndex,
// vertexOffset, sourceInstance).
TEST_P(GroupCompactComputeTest, KillSwitchParityIdenticalDrawSetsThroughCompact)
{
    const uint32_t meshA = AddMeshRow(6u);
    const uint32_t meshB = AddMeshRow(12u);
    const uint32_t meshC = AddMeshRow(18u);

    m_Scene->AddInstance(MakeInstance(1u, meshA));
    m_Scene->AddInstance(MakeInstance(1u, meshB));
    m_Scene->AddInstance(MakeInstance(2u, meshB));
    m_Scene->AddInstance(MakeInstance(2u, meshC));
    m_Scene->AddInstance(MakeInstance(1u, meshA));
    // A mirrored caster (flags bit 4): both arms split it into the parity-1
    // sibling row/run, so the set comparison covers the parity segments
    // through the compact pass too.
    m_Scene->AddInstance(MakeInstance(1u, meshA, /*flags=*/16u));

    std::vector<uint32_t> meshOrdered(m_Scene->GetMeshes().size(), 0xFFFFFFu);
    meshOrdered[meshA] = 0u;
    meshOrdered[meshB] = 1u; // A+B share group 0
    meshOrdered[meshC] = 2u; // C alone in group 1
    const std::vector<uint32_t> orderedToGroup{0u, 0u, 1u};

    using Draw = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

    // OFF arm: per-bucket scatter (mode 0), every capacity slot populated.
    uint32_t totalOff = 0u;
    const auto tableOff =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &totalOff);
    const auto off = Dispatch(tableOff, totalOff, 0u, 0u,
                              static_cast<uint32_t>(tableOff.size()), totalOff);
    EXPECT_EQ(off.stats.tableMisses, 0u);
    std::multiset<Draw> drawsOff;
    for (uint32_t s = 0; s < totalOff; ++s)
    {
        drawsOff.insert({off.records[s * kDrawRecordUints + 0],
                         off.records[s * kDrawRecordUints + 2],
                         off.records[s * kDrawRecordUints + 3],
                         off.indirection[s]});
    }

    // ON arm: ordered-axis scatter + mesh-major compact; collect the DRAW
    // block through the per-run counts (exactly what a consumer executes).
    uint32_t totalOn = 0u;
    const auto tableOn = GPUDrawStreamBuilder::BuildBatchTable(
        m_Scene->GetBatchRegistry(), meshOrdered, &totalOn);
    ASSERT_EQ(totalOn, totalOff);
    const auto runs = ComputeRuns(tableOn, orderedToGroup);
    // The mirrored caster must have produced a parity-1 sibling run:
    // (1,g0,p0) rows{A,B}, (1,g0,p1) row{A|p}, (2,g0,p0) row{B}, (2,g1,p0) row{C}.
    ASSERT_EQ(runs.runCount, 4u);
    const auto on = DispatchGroupedCompact(tableOn, runs, totalOn, meshOrdered, {});
    if (on.cursors.empty())
        FAIL() << "draw_stream_group_compact.shaderpkg unavailable";
    EXPECT_EQ(on.stats.tableMisses, 0u);

    std::multiset<Draw> drawsOn;
    const uint32_t rowCount = static_cast<uint32_t>(tableOn.size());
    for (uint32_t g = 0, row = 0; g < runs.runCount; ++g)
    {
        // Run base = its first row's capacity prefix (the published offset).
        while (row < rowCount && runs.rowRun[row * 2u + 1u] != g)
            ++row;
        const uint32_t runBase = tableOn[row].recordOffset;
        const uint32_t count   = on.cursors[rowCount + g];
        for (uint32_t s = 0; s < count; ++s)
        {
            const uint32_t slot = totalOn + runBase + s; // draw block
            drawsOn.insert({on.records[slot * kDrawRecordUints + 0],
                            on.records[slot * kDrawRecordUints + 2],
                            on.records[slot * kDrawRecordUints + 3],
                            on.indirection[slot]});
        }
    }
    EXPECT_EQ(drawsOff, drawsOn)
        << "consolidated compact path must not add, drop, or alter any draw";
}

// Snapshot-overflow self-heal: when the captured-snapshot invariant breaks,
// a row's cursor can exceed its capacity (the scatter's overflow tripwire
// fired; slots past capacity were never written). The compact pass must clamp
// with min(cursor, capacity) on BOTH the count sum and the copy window — the
// published count then covers only real records, never staging poison.
// Drives the compact pipeline alone over hand-seeded state (no scatter).
// The tail region compacts through this same shader, and its records are
// claimed in PAIRS. pairedRecords rounds each row's live count DOWN to even, so
// an odd-capacity row whose tail filled contributes its written pairs and not
// the unwritten slot the raw cursor implies. Same table, same runs, same bases --
// only the flag differs, which is the whole reason one shader serves both.
TEST_P(GroupCompactComputeTest, PairedRecordsRoundEachRowsLiveCountDownToWholePairs)
{
    // One run of two rows: row 0 capacity 3 (cursor 3 -- a written pair plus a
    // refused claim's leftover), row 1 capacity 2 (cursor 2 -- one whole pair).
    const std::vector<GPUDrawStreamBuilder::BatchTableEntry> table{
        {1u, 0u, 0u, 3u},
        {1u, 1u, 3u, 2u},
    };
    const std::vector<uint32_t> orderedToGroup{0u, 0u};
    const auto runs = ComputeRuns(table, orderedToGroup);
    ASSERT_EQ(runs.runCount, 1u);
    const uint32_t total = 5u;

    std::vector<uint32_t> staging(total * kDrawRecordUints, 0xBBBBBBBBu);
    auto seedRecord = [&](uint32_t slot, uint32_t indexCount)
    {
        uint32_t* rec = &staging[slot * kDrawRecordUints];
        rec[0] = indexCount; rec[1] = 1u; rec[2] = 0u; rec[3] = 0u; rec[4] = slot;
    };
    // Row 0's written pair, its unwritten odd slot (poison), then row 1's pair.
    seedRecord(0u, 6u);
    seedRecord(1u, 6u);
    seedRecord(3u, 12u);
    seedRecord(4u, 12u);
    const std::vector<uint32_t> stagingIndirection{10u, 11u, 0xBBBBBBBBu, 12u, 13u};
    const std::vector<uint32_t> cursorSeed{3u, 2u, 0u};

    const auto r = DispatchCompactOnly(table, runs, total, cursorSeed, staging,
                                       stagingIndirection, /*pairedRecords=*/1u);
    if (r.cursors.empty())
        FAIL() << "draw_stream_group_compact.shaderpkg unavailable";

    // (3 & ~1) + (2 & ~1) = 4. Without the rounding this reads 5 and row 0's
    // poison slot is copied into the drawn window.
    EXPECT_EQ(r.cursors[2], 4u);

    const uint32_t drawBase = total;
    const std::vector<uint32_t> expectedIndexCounts{6u, 6u, 12u, 12u};
    for (uint32_t s = 0; s < 4u; ++s)
    {
        const uint32_t* rec = &r.records[(drawBase + s) * kDrawRecordUints];
        EXPECT_EQ(rec[0], expectedIndexCounts[s]) << "slot " << s;
        EXPECT_NE(r.indirection[drawBase + s], 0xBBBBBBBBu)
            << "an unwritten tail slot leaked into the drawn window at " << s;
    }
}

TEST_P(GroupCompactComputeTest, CompactClampsOverflowedCursorsToSnapshotCapacity)
{
    // One run of two rows: row 0 capacity 2 (cursor OVERFLOWED to 5), row 1
    // capacity 3 (cursor 2 — one culled hole). Ordered axis {0,1}, one group.
    const std::vector<GPUDrawStreamBuilder::BatchTableEntry> table{
        {1u, 0u, 0u, 2u},
        {1u, 1u, 2u, 3u},
    };
    const std::vector<uint32_t> orderedToGroup{0u, 0u};
    const auto runs = ComputeRuns(table, orderedToGroup);
    ASSERT_EQ(runs.runCount, 1u);
    const uint32_t total = 5u;

    // Staging: row 0 slots {0,1} and row 1 slots {2,3} hold real records;
    // slot 4 (row 1's culled hole) holds poison that must never be copied.
    std::vector<uint32_t> staging(total * kDrawRecordUints, 0xBBBBBBBBu);
    auto seedRecord = [&](uint32_t slot, uint32_t indexCount)
    {
        uint32_t* rec = &staging[slot * kDrawRecordUints];
        rec[0] = indexCount; rec[1] = 1u; rec[2] = 0u; rec[3] = 0u; rec[4] = slot;
    };
    seedRecord(0u, 6u);
    seedRecord(1u, 6u);
    seedRecord(2u, 12u);
    seedRecord(3u, 12u);
    const std::vector<uint32_t> stagingIndirection{10u, 11u, 12u, 13u, 0xBBBBBBBBu};
    // Cursors: row 0 overflowed (5 > cap 2), row 1 live 2, count slot zeroed.
    const std::vector<uint32_t> cursorSeed{5u, 2u, 0u};

    const auto r = DispatchCompactOnly(table, runs, total, cursorSeed, staging,
                                       stagingIndirection);
    if (r.cursors.empty())
        FAIL() << "draw_stream_group_compact.shaderpkg unavailable";

    // Count = min(5,2) + min(2,3) = 4, never the raw cursor sum (7).
    EXPECT_EQ(r.cursors[2], 4u);

    // Draw block: 4 dense records [6,6,12,12]; nothing inside the
    // count-covered window is poison; the tail slot stays untouched.
    const uint32_t drawBase = total;
    const std::vector<uint32_t> expectedIndexCounts{6u, 6u, 12u, 12u};
    for (uint32_t s = 0; s < 4u; ++s)
    {
        const uint32_t* rec = &r.records[(drawBase + s) * kDrawRecordUints];
        EXPECT_EQ(rec[0], expectedIndexCounts[s]) << "slot " << s;
        EXPECT_NE(rec[0], 0xBBBBBBBBu) << "staging poison leaked into the count window";
        EXPECT_EQ(rec[4], drawBase + s) << "firstInstance must be the dst slot";
        EXPECT_NE(r.indirection[drawBase + s], 0xBBBBBBBBu) << "slot " << s;
        EXPECT_NE(r.indirection[drawBase + s], 0xDEADBEEFu) << "slot " << s;
    }
    EXPECT_EQ(r.records[(drawBase + 4u) * kDrawRecordUints], 0xAAAAAAAAu)
        << "slots past the clamped count stay untouched";
}

// ---- Dithered LOD crossfade (draw_command_scatter.comp binding 11) ---------
//
// These drive the REAL ge_SelectLOD scan (projScaleY > 0, no forceLod) plus the
// fade state machine. A two-level chain with distinct per-LOD index counts lets
// the emitted record identify which level a record carries, and the indirection
// word carries the dither weight + phase the fragment stage tests against.
namespace
{
constexpr uint32_t kFadeLod0Indices = 30u;
constexpr uint32_t kFadeLod1Indices = 12u;
// Switch coverage for LOD0. With radius 1 and projScaleY 1, coverage == 1/dist,
// so LOD0 engages inside 10 units and LOD1 outside it.
constexpr float kFadeThreshold0 = 0.1f;
constexpr float kFadeNearDist   = 5.0f;  // coverage 0.20 -> LOD0
constexpr float kFadeFarDist    = 20.0f; // coverage 0.05 -> LOD1
constexpr float kFadeDuration   = 0.25f;

GPUMesh MakeCrossfadeMesh()
{
    GPUMesh m{};
    m.indexCount     = kFadeLod0Indices;
    m.indexOffset    = 0u;
    m.vertexOffset   = 0u;
    m.vertexCount    = 32u;
    m.lodCount       = 2u;
    m.boundingRadius = 1.0f;
    m.lodIndexCount[0]  = kFadeLod0Indices; m.lodIndexOffset[0] = 0u;  m.lodVertexOffset[0] = 0u;
    m.lodIndexCount[1]  = kFadeLod1Indices; m.lodIndexOffset[1] = 30u; m.lodVertexOffset[1] = 0u;
    m.lodThreshold[0]   = kFadeThreshold0;
    m.lodThreshold[1]   = 0.0f; // coarsest level always clears
    return m;
}

uint32_t PackFadeLevels(uint32_t fromLod, uint32_t toLod)
{
    return fromLod | (toLod << GPUDrawStreamBuilder::kLodFadeToLevelShift);
}

// The exact bytes the production reset fills a fade slot with — both words, as a
// FillBuffer pattern does. Derived from the production constant rather than
// restated, so the fixture cannot keep modelling a reset the engine no longer
// performs (the failure mode that let a zero-seeded first-touch pin stay green
// while the shipped reset wrote something else).
LodFadeState NoHistoryFadeState()
{
    LodFadeState s{};
    s.Levels = GPUDrawStreamBuilder::kLodNoHistory;
    static_assert(sizeof(s.StartSeconds) == sizeof(GPUDrawStreamBuilder::kLodNoHistory));
    std::memcpy(&s.StartSeconds, &GPUDrawStreamBuilder::kLodNoHistory,
                sizeof(s.StartSeconds));
    return s;
}

// Arena shape every crossfade fixture below uses. Cursor 0 is the row's HEAD
// cursor, cursor 1 its TAIL cursor; records [0,2) are the head block and [2,4)
// the tail block, which reuses the head row's recordOffset against its own base.
// The tail bases are handed to the shader even when the feature is OFF, so a
// dispatch that wrongly touched the tail shows up as a non-zero cursor 1 rather
// than aliasing the head cursor and hiding.
constexpr uint32_t kFadeCursorSlots    = 2u;
constexpr uint32_t kFadeRecordSlots    = 4u;
constexpr uint32_t kFadeTailCursor     = 1u;
constexpr uint32_t kFadeTailRecordBase = 2u;

// A tail claim is a PAIR, and `capacity` is the same bound for both regions, so
// a row needs room for two records before anything can fade. Production gets
// that from the row's snapshot live count; a one-instance fixture states it
// directly. Single-row tables only — widening a row would shift the prefix.
std::vector<GPUDrawStreamBuilder::BatchTableEntry> PairableFadeTable(
    const BatchRegistry& registry, uint32_t* outTotal)
{
    auto table = GPUDrawStreamBuilder::BuildBatchTable(registry, {}, outTotal);
    if (table.size() == 1u && table[0].capacity < 2u)
    {
        table[0].capacity = 2u;
        if (outTotal)
            *outTotal = 2u;
    }
    return table;
}

uint32_t FadeWeightOf(uint32_t indirectionWord)
{
    return (indirectionWord >> GPUDrawStreamBuilder::kLodFadeWeightShift)
           & GPUDrawStreamBuilder::kLodFadeWeightMask;
}

bool FadePhaseOf(uint32_t indirectionWord)
{
    return (indirectionWord & GPUDrawStreamBuilder::kLodFadePhaseBit) != 0u;
}

uint32_t FadeInstanceOf(uint32_t indirectionWord)
{
    return indirectionWord & GPUDrawStreamBuilder::kLodFadeIndexMask;
}
} // namespace

// The A/B guarantee every measurement lane depends on: with the duration at 0,
// a seeded state that WOULD produce a fade must be ignored ENTIRELY — same
// record count, same record bytes, same indirection words, same cursor.
TEST_P(BatchScatterComputeTest, CrossfadeOffIsByteIdenticalToNoCrossfade)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // A state that is mid-transition 0 -> 1 at t = 0.
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(0u, 1u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY   = 1.0f;
    lod.cameraPos[2] = kFadeFarDist; // selects LOD1
    lod.nowSeconds   = 0.1f;         // mid-fade if the feature were on
    lod.tailCursorBase = kFadeTailCursor;
    lod.tailRecordBase = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 1u) << "duration 0 must claim exactly one HEAD slot";
    EXPECT_EQ(r.cursors[kFadeTailCursor], 0u)
        << "duration 0 must not claim, or even touch, the tail cursor";
    EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], kFadeLod1Indices);
    EXPECT_EQ(r.indirection[0], 0u) << "the word must be the BARE instance index";
    EXPECT_EQ(FadeWeightOf(r.indirection[0]), 0u);
    for (uint32_t slot = kFadeTailRecordBase; slot < kFadeRecordSlots; ++slot)
        EXPECT_EQ(r.indirection[slot], 0xDEADBEEFu) << "tail slot " << slot << " must be unwritten";
    // Untouched: the early return must precede the first lodFade[] read/write.
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(0u, 1u));
    EXPECT_EQ(r.lodFade[0].StartSeconds, 0.0f);
}

// A level change emits exactly two records — incoming level with phase 0,
// outgoing with phase 1, both carrying the SAME weight (which is what makes the
// two dither tests exactly complementary) and the same instance index — and it
// emits them into the TAIL, claiming NO head slot. The empty head is the whole
// point: the depth prepass draws head records only, so a fading instance is
// absent from the depth its own dither tests against.
void BatchScatterComputeTest::ExpectCrossfadeLevelChangeEmitsItsPairIntoTheTailNotTheHead()
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);
    ASSERT_EQ(total, 2u) << "the row must have room for a pair";

    // Settled on LOD0, then the camera pulls back so LOD1 is selected. This seed
    // is also the complement of the first-touch pin: ZERO is a real settled state
    // and must still fade, which is what stops the no-history sentinel from being
    // "crossfading is off for anything that starts at LOD0".
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(0u, 0u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 10.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.stats.overflows, 0u);
    EXPECT_EQ(r.cursors[0], 0u)
        << "a fading instance writes NO head record — its pair lives in the tail "
           "block instead, which both raster passes draw through the dither variant";
    ASSERT_EQ(r.cursors[kFadeTailCursor], 2u) << "one pair claimed from the tail";
    EXPECT_EQ(r.indirection[0], 0xDEADBEEFu) << "the head region must be untouched";
    constexpr uint32_t kA = kFadeTailRecordBase;      // incoming
    constexpr uint32_t kB = kFadeTailRecordBase + 1u; // outgoing
    // Record A: the incoming level (LOD1), phase 0.
    EXPECT_EQ(r.records[kA * kDrawRecordUints + 0], kFadeLod1Indices);
    EXPECT_FALSE(FadePhaseOf(r.indirection[kA]));
    // Record B: the outgoing level (LOD0), phase 1.
    EXPECT_EQ(r.records[kB * kDrawRecordUints + 0], kFadeLod0Indices);
    EXPECT_TRUE(FadePhaseOf(r.indirection[kB]));
    // Complementarity is exact only if both carry the same weight.
    EXPECT_EQ(FadeWeightOf(r.indirection[kA]), FadeWeightOf(r.indirection[kB]));
    EXPECT_GE(FadeWeightOf(r.indirection[kA]), 1u) << "0 is the not-fading sentinel";
    EXPECT_EQ(FadeInstanceOf(r.indirection[kA]), 0u);
    EXPECT_EQ(FadeInstanceOf(r.indirection[kB]), 0u);
    // firstInstance is the ARENA-GLOBAL slot, so the vertex stage reads the
    // matching indirection word — it must follow the record into the tail.
    EXPECT_EQ(r.records[kA * kDrawRecordUints + 4], kA);
    EXPECT_EQ(r.records[kB * kDrawRecordUints + 4], kB);
    // State now describes the live transition 0 -> 1 stamped at this frame.
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(0u, 1u));
    EXPECT_FLOAT_EQ(r.lodFade[0].StartSeconds, 10.0f);
}

TEST_P(BatchScatterComputeTest, CrossfadeLevelChangeEmitsItsPairIntoTheTailNotTheHead)
{
    ExpectCrossfadeLevelChangeEmitsItsPairIntoTheTailNotTheHead();
}

// First touch of a slot the reset path just filled — a brand-new or recycled
// instance slot, which EnsureLodFadeBuffer refills with kLodNoHistory. An
// instance that has never been DISPLAYED has no level to dissolve from, so the
// state machine must adopt its first pick as the settled from == to state. Both
// arms run on their own pristine sentinel seed and neither may produce a pair.
//
// The far arm is the one that regressed: the state machine tests the selection
// against toLod BEFORE it tests fromLod == toLod, so a slot seeded with ZEROES
// (a real state — settled, displaying LOD0) whose first pick was any coarser
// level started a full-duration fade from geometry it had never shown. That fade
// is live on the very first frame a batch publishes a tail, which is also the
// frame the LodCrossfade variant is first REQUESTED; the compile is async and a
// fading instance writes no head record, so the window between request and need
// was zero frames wide and the instance disappeared from colour AND depth for
// it. The sentinel makes first touch unable to reach that window at all.
TEST_P(BatchScatterComputeTest, CrossfadeFirstTouchAdoptsItsPickInsteadOfFadingIn)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);
    ASSERT_EQ(total, 2u) << "the row must have room for a pair";

    // Seeded with the bytes the production reset writes, taken from the
    // production constant. A zeroed state must NOT stand in for first touch: it
    // is the real settled "displaying LOD0" pair, and conflating the two is the
    // defect.
    const LodFadeState kNoHistory = NoHistoryFadeState();
    ASSERT_FALSE(kNoHistory == LodFadeState{})
        << "the no-history sentinel must be distinguishable from a settled LOD0 state";
    SeedLodFade(std::vector<LodFadeState>{kNoHistory});

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 10.0f;
    lod.tailCursorBase    = kFadeTailCursor;
    lod.tailRecordBase    = kFadeTailRecordBase;

    // Arm 1 — the first pick is LOD0: no transition, one opaque head record, and
    // the sentinel replaced by the settled state it just adopted.
    lod.cameraPos[2] = kFadeNearDist;
    const auto nearArm = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                                  /*classMode=*/0u, /*materialColorClass=*/{},
                                  /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                                  /*meshGroupMode=*/0u, lod);
    ASSERT_EQ(nearArm.cursors[0], 1u) << "a first-touch LOD0 pick must claim one HEAD slot";
    EXPECT_EQ(nearArm.cursors[kFadeTailCursor], 0u) << "and never a tail pair";
    EXPECT_EQ(nearArm.records[0 * kDrawRecordUints + 0], kFadeLod0Indices);
    EXPECT_EQ(nearArm.indirection[0], 0u) << "the word must be the BARE instance index";
    EXPECT_EQ(nearArm.lodFade[0].Levels, PackFadeLevels(0u, 0u))
        << "first touch must replace the sentinel with its pick as a from == to state";
    EXPECT_FLOAT_EQ(nearArm.lodFade[0].StartSeconds, 10.0f);

    // Arm 2 — a fresh pristine sentinel seed, first pick LOD1. This is the arm
    // the sentinel exists for: it must claim a HEAD record at the level it
    // picked, never a tail pair fading in from a level it never displayed.
    SeedLodFade(std::vector<LodFadeState>{kNoHistory});
    lod.cameraPos[2] = kFadeFarDist;
    const auto farArm = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                                 /*classMode=*/0u, /*materialColorClass=*/{},
                                 /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                                 /*meshGroupMode=*/0u, lod);
    EXPECT_EQ(farArm.stats.overflows, 0u);
    EXPECT_EQ(farArm.cursors[kFadeTailCursor], 0u)
        << "a first-touch pick above LOD0 must claim NO tail pair: that pair has no "
           "head record, so the instance is absent from the depth prepass and from "
           "the colour pass too on any frame its tail draw is skipped";
    ASSERT_EQ(farArm.cursors[0], 1u) << "it must claim exactly one HEAD slot instead";
    EXPECT_EQ(farArm.records[0 * kDrawRecordUints + 0], kFadeLod1Indices)
        << "and draw the level it actually picked, not the LOD0 a zeroed state implied";
    EXPECT_EQ(farArm.indirection[0], 0u) << "the word must be the BARE instance index";
    for (uint32_t slot = kFadeTailRecordBase; slot < kFadeRecordSlots; ++slot)
        EXPECT_EQ(farArm.indirection[slot], 0xDEADBEEFu)
            << "tail slot " << slot << " must be unwritten";
    EXPECT_EQ(farArm.lodFade[0].Levels, PackFadeLevels(1u, 1u))
        << "the pick becomes the settled state, so a LATER change fades from LOD1";
    EXPECT_FLOAT_EQ(farArm.lodFade[0].StartSeconds, 10.0f);
}

// The state write is a conditional stamp, not an accumulation: re-running the
// same dispatch at the same clock must leave the state bit-identical and emit
// the same records. This is what makes the several concurrent slices of one view
// (HZB phase A/B, the thumbnail seam) safe without a barrier between them.
TEST_P(BatchScatterComputeTest, CrossfadeStateWriteIsIdempotent)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);

    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(0u, 0u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 7.5f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto first = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots, 0u, {},
                                0xFFFFFFFFu, {}, 0u, lod);
    const auto second = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots, 0u, {},
                                 0xFFFFFFFFu, {}, 0u, lod);

    ASSERT_EQ(first.cursors[kFadeTailCursor], 2u);
    EXPECT_EQ(second.cursors[kFadeTailCursor], 2u);
    EXPECT_EQ(second.cursors[0], first.cursors[0]);
    EXPECT_EQ(second.lodFade[0], first.lodFade[0]) << "a re-run must rewrite the SAME bytes";
    for (uint32_t slot = kFadeTailRecordBase; slot < kFadeRecordSlots; ++slot)
        EXPECT_EQ(second.indirection[slot], first.indirection[slot]) << "slot " << slot;
    for (uint32_t i = 0; i < kFadeRecordSlots * kDrawRecordUints; ++i)
        EXPECT_EQ(second.records[i], first.records[i]) << "record uint " << i;
}

// Reversing mid-fade REFLECTS the elapsed time instead of restarting, so the
// pixel split is continuous across the reversal and a camera oscillating at a
// threshold fades smoothly back rather than snapping to a fresh fade.
TEST_P(BatchScatterComputeTest, CrossfadeReversalReflectsElapsedTimeInsteadOfRestarting)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);

    // Halfway through 0 -> 1: started at t = 0, now t = duration/2.
    constexpr float kNow = kFadeDuration * 0.5f;
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(0u, 1u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeNearDist; // selection reverses to LOD0
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = kNow;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots, 0u, {},
                            0xFFFFFFFFu, {}, 0u, lod);

    ASSERT_EQ(r.cursors[kFadeTailCursor], 2u) << "the reversed transition is still a fade";
    EXPECT_EQ(r.cursors[0], 0u) << "and still writes no head record";
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(1u, 0u)) << "endpoints swap";
    // Reflected, NOT restarted: a restart would stamp start == now, putting the
    // reversed fade at weight ~0 and popping the pixel split from 50/50 to 0/100.
    EXPECT_NE(r.lodFade[0].StartSeconds, kNow) << "a restart would stamp `now`";
    EXPECT_NEAR(r.lodFade[0].StartSeconds, 0.0f, 1e-5f)
        << "reflecting 50% elapsed of a symmetric fade lands back on the original start";
    // Weight continuity: still the half-way split, with the levels swapped.
    const uint32_t weight = FadeWeightOf(r.indirection[kFadeTailRecordBase]);
    EXPECT_NEAR(static_cast<float>(weight)
                    / static_cast<float>(GPUDrawStreamBuilder::kLodFadeMaxWeight),
                0.5f, 0.02f);
    EXPECT_EQ(r.records[kFadeTailRecordBase * kDrawRecordUints + 0], kFadeLod0Indices)
        << "incoming is now LOD0";
    EXPECT_EQ(r.records[(kFadeTailRecordBase + 1u) * kDrawRecordUints + 0], kFadeLod1Indices)
        << "outgoing is now LOD1";
}

// An exhausted TAIL degrades to ONE opaque head record — today's pop — never a
// dropped instance, never a half-written pair, and never an overflow tripwire
// (the degradation is a quality trade, not a broken invariant). Modelled by a
// row whose capacity has no room for a pair.
TEST_P(BatchScatterComputeTest, CrossfadeTailExhaustionFallsBackToOneOpaqueHeadRecord)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    // Capacity 1: a pair does not fit, which is exactly the exhausted-tail state.
    const auto table =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);
    ASSERT_EQ(table[0].capacity, 1u);
    ASSERT_EQ(GPUDrawStreamBuilder::TailDrawBound(1u), 0u)
        << "and the consumer bound agrees the tail can draw nothing";

    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(0u, 0u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 3.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.stats.overflows, 0u) << "the instance must NOT be dropped, nor trip the tripwire";
    // One drawable HEAD record, opaque, at the incoming level.
    EXPECT_EQ(r.cursors[0], 1u);
    EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], kFadeLod1Indices);
    EXPECT_EQ(r.indirection[0], 0u) << "fallback must write the BARE instance index";
    EXPECT_EQ(FadeWeightOf(r.indirection[0]), 0u);
    // The rejected tail claim still advanced the tail cursor past capacity. That
    // is safe only because the consumer clamps with TailDrawBound, so nothing in
    // the tail region may have been written.
    EXPECT_EQ(r.cursors[kFadeTailCursor], 2u);
    for (uint32_t slot = kFadeTailRecordBase; slot < kFadeRecordSlots; ++slot)
        EXPECT_EQ(r.indirection[slot], 0xDEADBEEFu)
            << "tail slot " << slot << " must stay unwritten when the pair was refused";
}

// alpha reaching 1.0 ends the transition: back to ONE record with fade code 0,
// even though the state still records the (completed) endpoints.
TEST_P(BatchScatterComputeTest, CrossfadeCompletionReturnsToASingleOpaqueRecord)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);

    // Mid-transition 0 -> 1 started at t = 0; sample well past the duration.
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(0u, 1u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist; // still selects LOD1 == toLod
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = kFadeDuration * 4.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.cursors[0], 1u) << "a completed transition claims one HEAD slot again";
    EXPECT_EQ(r.cursors[kFadeTailCursor], 0u) << "and never touches the tail";
    EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], kFadeLod1Indices);
    EXPECT_EQ(r.indirection[0], 0u);
    EXPECT_EQ(FadeWeightOf(r.indirection[0]), 0u) << "fade code must be 0 once alpha reaches 1";
    // selected == toLod, so the stamp condition is false and the state is untouched.
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(0u, 1u));
    EXPECT_EQ(r.lodFade[0].StartSeconds, 0.0f);
}

// The state is per (instance, view): two instances of one batch transition
// independently, and neither reads the other's slot. This also pins the CPU
// mirror's 8-byte element stride against the shader's uvec2 from the data
// side; ShaderReflectionDiffTests pins the same stride from the reflected
// layout.
TEST_P(BatchScatterComputeTest, CrossfadeStateIsPerInstance)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx)); // instance 0
    m_Scene->AddInstance(MakeInstance(1u, meshIdx)); // instance 1
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);
    ASSERT_EQ(total, 2u) << "capacity is the snapshot live count — two instances, two records";

    // Instance 0 is settled ON the level the camera selects, so it must not
    // fade. Instance 1 is settled on the other level, so it must.
    SeedLodFade({LodFadeState{PackFadeLevels(1u, 1u), 0.0f},
                 LodFadeState{PackFadeLevels(0u, 0u), 0.0f}});

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist; // both select LOD1
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 5.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.stats.overflows, 0u);
    // The settled instance takes ONE head slot; the fading one takes a tail
    // pair. This split is exactly what the depth prepass consumes vs skips.
    EXPECT_EQ(r.cursors[0], 1u);
    EXPECT_EQ(r.cursors[kFadeTailCursor], 2u);
    // Slot 0 is unread by the settled instance and must be exactly as seeded;
    // reading it back through an 8-byte stride is the stride pin.
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(1u, 1u)) << "settled instance: no stamp";
    EXPECT_EQ(r.lodFade[0].StartSeconds, 0.0f);
    EXPECT_EQ(r.lodFade[1].Levels, PackFadeLevels(0u, 1u)) << "transitioning instance: stamped";
    EXPECT_FLOAT_EQ(r.lodFade[1].StartSeconds, 5.0f);

    // The head record is the settled instance, opaque and bare.
    EXPECT_EQ(r.indirection[0], 0u) << "instance 0 is settled: bare index, no fade code";
    // Exactly one instance is fading: both tail words carry a non-zero weight
    // and address instance 1 — the complementary pair.
    uint32_t faded = 0u;
    for (uint32_t slot = kFadeTailRecordBase; slot < kFadeRecordSlots; ++slot)
        if (FadeWeightOf(r.indirection[slot]) != 0u)
        {
            ++faded;
            EXPECT_EQ(FadeInstanceOf(r.indirection[slot]), 1u) << "only instance 1 fades";
        }
    EXPECT_EQ(faded, 2u);
}

// A tail that fills mid-frame must degrade the SURPLUS instances to opaque head
// records while the instances that did claim a pair keep their dither. Four
// instances of one row (capacity 4) all transition: the tail holds two pairs, so
// two fade and two pop. This is the ordering guard -- a scatter that skipped its
// head record BEFORE knowing the tail claim succeeded would drop those two
// instances entirely, and the object would vanish rather than pop.
TEST_P(BatchScatterComputeTest, CrossfadeTailOverflowPopsTheSurplusWithoutLosingInstances)
{
    constexpr uint32_t kInstances  = 4u;
    constexpr uint32_t kTailCursor = 1u;
    constexpr uint32_t kTailBase   = kInstances;      // head block [0,4), tail [4,8)
    constexpr uint32_t kRecords    = kInstances * 2u;

    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    for (uint32_t i = 0; i < kInstances; ++i)
        m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, kInstances);

    // Every instance settled on LOD0 while the camera selects LOD1.
    SeedLodFade(kInstances, PackFadeLevels(0u, 0u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 4.0f;
    lod.tailCursorBase    = kTailCursor;
    lod.tailRecordBase    = kTailBase;
    const auto r = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/2u, kRecords,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.stats.overflows, 0u) << "a full tail is a quality trade, not a tripwire";
    EXPECT_EQ(r.cursors[kTailCursor], kInstances * 2u) << "all four attempted a pair";
    EXPECT_EQ(r.cursors[0], 2u) << "the two that lost the race fall back to head records";

    // Two complete pairs fill the tail, and every one of them is dithered.
    for (uint32_t slot = kTailBase; slot < kTailBase + kInstances; ++slot)
    {
        ASSERT_NE(r.indirection[slot], 0xDEADBEEFu) << "tail slot " << slot << " must be written";
        EXPECT_GE(FadeWeightOf(r.indirection[slot]), 1u) << "tail slot " << slot;
    }
    for (uint32_t slot = 0; slot < 2u; ++slot)
        EXPECT_EQ(FadeWeightOf(r.indirection[slot]), 0u)
            << "head slot " << slot << " must be an opaque pop, not a dithered half";

    // Every instance is accounted for exactly once: two pairs + two singles.
    std::vector<uint32_t> seen;
    for (uint32_t slot = 0; slot < 2u; ++slot)
        seen.push_back(FadeInstanceOf(r.indirection[slot]));
    for (uint32_t slot = kTailBase; slot < kTailBase + kInstances; ++slot)
        seen.push_back(FadeInstanceOf(r.indirection[slot]));
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    EXPECT_EQ(seen.size(), kInstances) << "every instance drew, none was dropped";
}

// An ODD-capacity row can never fill its last tail slot: claims are pairs from
// slot 0, so the last accepted pair ends at capacity-2 for an even capacity and
// capacity-3 for an odd one. The raw cursor still runs past that, which is why
// the consumer bound is TailDrawBound and not the capacity -- without the
// rounding this unwritten slot reaches the rasteriser as a garbage record.
TEST_P(BatchScatterComputeTest, CrossfadeOddTailCapacityLeavesItsLastSlotUnwritten)
{
    constexpr uint32_t kInstances  = 3u;
    constexpr uint32_t kTailCursor = 1u;
    constexpr uint32_t kTailBase   = kInstances;      // head block [0,3), tail [3,6)
    constexpr uint32_t kRecords    = kInstances * 2u;

    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    for (uint32_t i = 0; i < kInstances; ++i)
        m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table =
        GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, kInstances);
    ASSERT_EQ(table[0].capacity, 3u);

    SeedLodFade(kInstances, PackFadeLevels(0u, 0u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 6.0f;
    lod.tailCursorBase    = kTailCursor;
    lod.tailRecordBase    = kTailBase;
    const auto r = Dispatch(table, total, 0u, 0u, /*cursorSlots=*/2u, kRecords,
                            /*classMode=*/0u, /*materialColorClass=*/{},
                            /*forceLod=*/0xFFFFFFFFu, /*meshPoolGroup=*/{},
                            /*meshGroupMode=*/0u, lod);

    EXPECT_EQ(r.stats.overflows, 0u);
    // One pair fits (slots 0,1); the next claim lands at 2, whose partner would
    // be slot 3 == capacity, so it is refused and that instance pops instead.
    EXPECT_EQ(r.cursors[0], 2u);
    EXPECT_EQ(r.cursors[kTailCursor], kInstances * 2u) << "the raw cursor runs past capacity";
    EXPECT_NE(r.indirection[kTailBase + 0u], 0xDEADBEEFu);
    EXPECT_NE(r.indirection[kTailBase + 1u], 0xDEADBEEFu);
    EXPECT_EQ(r.indirection[kTailBase + 2u], 0xDEADBEEFu)
        << "the odd last slot can never hold half a pair and must stay unwritten";
    // Which is exactly the bound the publisher hands the consumer.
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(table[0].capacity), 2u);
}

// Shadow slices keep exactly today's single-record path. The CPU expresses that
// by leaving crossfadeTailActive 0, so this pins the shader's own gate: even
// with a duration set, a pinned level must neither fade nor touch the state.
TEST_P(BatchScatterComputeTest, CrossfadeSkipsForcedLevelSlices)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);

    // Seeded settled on LOD1 while forceLod pins LOD0, so selected != toLod: a
    // slice WITHOUT the gate would stamp a transition and emit a pair here. That
    // disagreement is what makes the gate observable at all.
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(1u, 1u), 0.0f);

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[2]      = kFadeFarDist;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 2.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots, 0u, {},
                            /*forceLod=*/0u, {}, 0u, lod);

    EXPECT_EQ(r.cursors[0], 1u) << "a pinned level never transitions, so it never pairs";
    EXPECT_EQ(r.cursors[kFadeTailCursor], 0u) << "and never claims a tail slot";
    EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], kFadeLod0Indices);
    EXPECT_EQ(r.indirection[0], 0u);
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(1u, 1u))
        << "a forced-level slice must not touch the state";
}

// ---- Dwell band x crossfade interaction ------------------------------------
//
// The fade consumes the BANDED selection: ge_SelectLOD (band applied) feeds
// ge_UpdateLodFade, so a level the band HOLDS is not a transition and starts no
// fade, while a change the band ALLOWS fades from the held level. These two pin
// that ordering — a composition that fed the fade the loose pick would emit a
// record pair in the first case, and one that committed history from the fade's
// outgoing level would leave prev at the old level in the second.

// In the dwell region (bare threshold cleared, gain gate missed) the band holds
// the coarse level. The fade must see that HELD level as the selection: settled
// state stays settled, one record, no dither weight.
TEST_P(BatchScatterComputeTest, HysteresisHoldStartsNoCrossfade)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);
    ASSERT_EQ(total, 2u);

    SeedPrevLod(1u, 1u);                                  // history: holding LOD1
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(1u, 1u), 0.0f); // fade settled at LOD1

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[0]      = kInBandDist; // loose pick = LOD0; banded pick holds LOD1
    lod.hysteresisBand    = kHystBand;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 10.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, {}, 0xFFFFFFFFu, {}, 0u, lod);

    EXPECT_EQ(r.stats.tableMisses, 0u);
    ASSERT_EQ(r.cursors[0], 1u) << "a held level is not a transition: no record pair";
    EXPECT_EQ(r.cursors[kFadeTailCursor], 0u) << "and no tail claim";
    EXPECT_EQ(r.records[0 * kDrawRecordUints + 0], kHystLod1Indices);
    EXPECT_EQ(r.indirection[0], 0u) << "no fade code on a settled record";
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(1u, 1u))
        << "a fade fed the LOOSE pick would have stamped 1 -> 0 here";
    EXPECT_EQ(r.prevLod[0], 1u);
}

// Past the gain gate the band releases the level. The fade then animates that
// banded change — outgoing = the held level — and the dwell history commits the
// INCOMING level, so the fade's outgoing record never re-enters selection.
TEST_P(BatchScatterComputeTest, HysteresisReleaseFadesFromTheHeldLevel)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);
    ASSERT_EQ(total, 2u);

    SeedPrevLod(1u, 1u);                                  // history: holding LOD1
    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(1u, 1u), 0.0f); // fade settled at LOD1

    LodDispatchParams lod{};
    lod.projScaleY        = 1.0f;
    lod.cameraPos[0]      = kNearDist; // coverage clears threshold*(1+band): banded pick = LOD0
    lod.hysteresisBand    = kHystBand;
    lod.crossfadeDuration = kFadeDuration;
    lod.nowSeconds        = 10.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, {}, 0xFFFFFFFFu, {}, 0u, lod);

    EXPECT_EQ(r.stats.overflows, 0u);
    ASSERT_EQ(r.cursors[kFadeTailCursor], 2u)
        << "a banded change fades: incoming + outgoing pair, in the tail";
    EXPECT_EQ(r.cursors[0], 0u) << "and no head record";
    constexpr uint32_t kIn  = kFadeTailRecordBase;
    constexpr uint32_t kOut = kFadeTailRecordBase + 1u;
    EXPECT_EQ(r.records[kIn * kDrawRecordUints + 0], kHystLod0Indices);
    EXPECT_FALSE(FadePhaseOf(r.indirection[kIn]));
    EXPECT_EQ(r.records[kOut * kDrawRecordUints + 0], kHystLod1Indices)
        << "the outgoing level is the one the band was holding";
    EXPECT_TRUE(FadePhaseOf(r.indirection[kOut]));
    EXPECT_GE(FadeWeightOf(r.indirection[kIn]), 1u);
    EXPECT_EQ(r.lodFade[0].Levels, PackFadeLevels(1u, 0u));
    EXPECT_FLOAT_EQ(r.lodFade[0].StartSeconds, 10.0f);
    EXPECT_EQ(r.prevLod[0], 0u)
        << "history tracks the INCOMING level, not the fade's outgoing one";
}

// ---- Rendered level and phase history (bindings 12/13) --------------------
//
// The scatter is the only stage that knows the selected level, the crossfade
// phase and the instance identity in one invocation, so it owns the history a
// consumer needs to pair this frame's surface with the previous frame's. These
// cases drive it at the SHADER level: the pair is two buffers, the previous one
// is never written during a frame, and the gate is independent of the dwell
// band — the band defaults to zero, so a history that only worked with
// hysteresis enabled would be a history nothing could use.

namespace
{
// Dispatch parameters shared by the history cases: a real auto-select scan, no
// band, no fade. Distances come from the hysteresis fixture above (coverage ==
// 1/dist against a 0.1 threshold), so kFarDist selects LOD1 and kNearDist LOD0.
LodDispatchParams HistoryLod(float dist)
{
    LodDispatchParams lod{};
    lod.projScaleY      = 1.0f;
    lod.cameraPos[0]    = dist;
    lod.renderedHistory = true;
    return lod;
}
} // namespace

// The gate is off on every view today, so this is the byte-for-byte promise
// that S4 changes nothing until a consumer exists: neither half is touched, and
// the records are identical to the same dispatch with the gate on.
TEST_P(BatchScatterComputeTest, RenderedHistoryOffTouchesNeitherHalfAndKeepsRecordsIdentical)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // A pattern neither half could produce, so any write shows up.
    constexpr uint32_t kSeed = 0x0BADF00Du;
    SeedRenderedHistory(std::vector<uint32_t>(1u, kSeed));
    WriteBuffer(m_Rendered[m_RenderedCurrentIndex], &kSeed, sizeof(kSeed));

    LodDispatchParams off = HistoryLod(kFarDist);
    off.renderedHistory   = false;
    const auto gateOff = Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, off);
    EXPECT_EQ(gateOff.renderedPrevious[0], kSeed) << "the gate must not read-modify-write";
    EXPECT_EQ(gateOff.renderedCurrent[0], kSeed) << "and must not write the current half";

    const auto gateOn =
        Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kFarDist));
    EXPECT_EQ(gateOn.cursors, gateOff.cursors);
    EXPECT_EQ(gateOn.records, gateOff.records) << "recording history must not move a draw record";
    EXPECT_EQ(gateOn.indirection, gateOff.indirection);
    EXPECT_EQ(gateOn.stats.tableMisses, 0u);
    EXPECT_EQ(gateOn.stats.overflows, 0u);
}

// T24: the level and the continuity verdict, across three frames, with the dwell
// band at its default of ZERO throughout — so an implementation that quietly
// depended on hysteresis being enabled fails here. prevLod is asserted untouched
// for the same reason.
TEST_P(BatchScatterComputeTest, RenderedHistoryTracksLevelContinuityWithTheDwellBandOff)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    SeedPrevLod(1u, GPUDrawStreamBuilder::kLodNoHistory);
    SeedRenderedHistory(std::vector<uint32_t>(1u, GPUDrawStreamBuilder::kLodNoHistory));

    // Frame 1: no previous entry at all — the first frame of a view, or one
    // whose slot was just invalidated. Records the level, continuity denied.
    const auto first =
        Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kFarDist));
    ASSERT_EQ(first.cursors[0], 1u);
    EXPECT_EQ(first.renderedCurrent[0], MakeRenderedEntry(1u, false, false));
    EXPECT_EQ(first.prevLod[0], GPUDrawStreamBuilder::kLodNoHistory)
        << "the pair must not write the dwell band's history";

    // Frame 2: same level, and the previous entry now names it — continuous.
    RotateRenderedHistory();
    const auto held =
        Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kFarDist));
    EXPECT_EQ(held.renderedPrevious[0], MakeRenderedEntry(1u, false, false))
        << "the rotation must hand the previous frame's entry to this one";
    EXPECT_EQ(held.renderedCurrent[0], MakeRenderedEntry(1u, false, true));

    // Frame 3: the instance crosses the threshold. Same batch key, same slot,
    // same mesh — only the rendered level moved, and only this history sees it.
    RotateRenderedHistory();
    const auto crossed =
        Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kNearDist));
    EXPECT_EQ(crossed.records[0], kHystLod0Indices) << "the crossing frame must draw LOD0";
    EXPECT_EQ(crossed.renderedCurrent[0], MakeRenderedEntry(0u, false, false))
        << "a level change breaks continuity on the crossing frame";

    // Frame 4: settled again at the new level — continuity returns without any
    // consumer being re-enabled. Invalid history must be able to end.
    RotateRenderedHistory();
    const auto settled =
        Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kNearDist));
    EXPECT_EQ(settled.renderedCurrent[0], MakeRenderedEntry(0u, false, true));
    EXPECT_EQ(settled.prevLod[0], GPUDrawStreamBuilder::kLodNoHistory);
}

// The immutability the pair exists for: the world scatter's phases and any later
// dispatch of the SAME frame must all read the previous RENDERED frame, not a
// value an earlier dispatch of this frame already committed. The seeded previous
// level differs from the one this frame selects, so a shared in-place buffer
// would flip the second dispatch's verdict to continuous.
TEST_P(BatchScatterComputeTest, RenderedHistoryPreviousSurvivesEveryDispatchOfTheFrame)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    const uint32_t slotA = m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    const uint32_t slotB = m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);

    // The previous frame rendered both instances at the level this frame also
    // selects, so the correct verdict is CONTINUOUS for both — and a dispatch
    // that read the half this frame writes would see the cleared value instead
    // and report a break.
    const uint32_t previousEntry = MakeRenderedEntry(1u, false, true);
    SeedRenderedHistory(std::vector<uint32_t>(2u, previousEntry));

    // One frame, two culling generations over DISJOINT visibility slices: the
    // shape the world scatter runs as, not two copies of one dispatch.
    LodDispatchParams phaseAParams = HistoryLod(kFarDist);
    phaseAParams.visibility        = {0u, 0u};
    phaseAParams.visibility[slotA] = 1u;
    LodDispatchParams phaseBParams = HistoryLod(kFarDist);
    phaseBParams.visibility        = {0u, 0u};
    phaseBParams.visibility[slotB] = 1u;

    const auto phaseA =
        Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, phaseAParams);
    ASSERT_EQ(phaseA.cursors[0], 1u) << "phase A must see its slice alone";
    EXPECT_EQ(phaseA.renderedCurrent[slotA], MakeRenderedEntry(1u, false, true));
    EXPECT_EQ(phaseA.renderedCurrent[slotB], GPUDrawStreamBuilder::kLodNoHistory)
        << "the other generation's instance has not been scattered yet";

    const auto phaseB =
        Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, phaseBParams);
    ASSERT_EQ(phaseB.cursors[0], 1u);
    EXPECT_EQ(phaseB.renderedPrevious[slotA], previousEntry)
        << "phase A committed into the CURRENT half; the previous half is read-only this frame";
    EXPECT_EQ(phaseB.renderedPrevious[slotB], previousEntry);
    EXPECT_EQ(phaseB.renderedCurrent[slotB], MakeRenderedEntry(1u, false, true))
        << "phase B compares against the previous RENDERED frame, not against what "
           "phase A just wrote for its own slice";
    EXPECT_EQ(phaseB.renderedCurrent[slotA], MakeRenderedEntry(1u, false, true))
        << "and it leaves phase A's commit alone";

    // A consumer dispatch after both phases — a motion producer reading the
    // previous-frame level for every instance in the frame.
    const auto consumer =
        Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kFarDist));
    EXPECT_EQ(consumer.renderedPrevious[slotA], previousEntry);
    EXPECT_EQ(consumer.renderedPrevious[slotB], previousEntry);
    EXPECT_EQ(consumer.renderedCurrent[slotA], MakeRenderedEntry(1u, false, true))
        << "a consumer running after both world phases still reads the previous "
           "RENDERED frame and reaches the same verdict they did";
    EXPECT_EQ(consumer.renderedCurrent[slotB], MakeRenderedEntry(1u, false, true));
}

// Frame eligibility. An instance that claims no record — culled here, and a
// table miss or an undispatched view elsewhere — leaves the no-history value in
// the frame's current half, so when it reappears its previous entry denies
// continuity even though nothing about the instance changed. That is the
// opposite of the dwell band's rule (which keeps the last DRAWN level), and it
// is deliberate: a frame that rendered nothing has no previous surface.
TEST_P(BatchScatterComputeTest, RenderedHistoryDeniesContinuityToAReappearingInstance)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    GPUInstance nearInstance = MakeInstance(1u, meshIdx);
    GPUInstance farInstance  = MakeInstance(1u, meshIdx);
    farInstance.boundingCenter = {0.0f, 0.0f, 40.0f};
    const uint32_t nearSlot = m_Scene->AddInstance(nearInstance);
    const uint32_t farSlot  = m_Scene->AddInstance(farInstance);
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 2u);

    SeedRenderedHistory(std::vector<uint32_t>(2u, GPUDrawStreamBuilder::kLodNoHistory));

    // Two settled frames, so the half the culled frame below writes into holds a
    // REAL entry from two frames ago rather than a fresh buffer's no-history —
    // the only arrangement in which the frame's clear is load-bearing.
    for (int frame = 0; frame < 2; ++frame)
    {
        const auto both = Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u,
                                   HistoryLod(kFarDist));
        ASSERT_EQ(both.cursors[0], 2u);
        EXPECT_NE(both.renderedCurrent[nearSlot], GPUDrawStreamBuilder::kLodNoHistory);
        EXPECT_NE(both.renderedCurrent[farSlot], GPUDrawStreamBuilder::kLodNoHistory);
        RotateRenderedHistory();
    }

    // Frame 3: the far instance falls under the small-object cull and claims
    // nothing. Its entry stays at the no-history value the frame's clear left.
    LodDispatchParams culled  = HistoryLod(kFarDist);
    culled.smallCullCoverage  = 0.05f; // the far instance's coverage is ~0.02
    const auto dropped =
        Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, culled);
    ASSERT_EQ(dropped.cursors[0], 1u) << "exactly one instance survived the cull";
    EXPECT_NE(dropped.renderedCurrent[nearSlot], GPUDrawStreamBuilder::kLodNoHistory);
    EXPECT_EQ(dropped.renderedCurrent[farSlot], GPUDrawStreamBuilder::kLodNoHistory)
        << "an instance that claimed no record must leave no previous surface behind";

    // Frame 4: it reappears at the level it last drew. Continuity is denied for
    // this frame and granted on the next, so the invalidation ends by itself.
    RotateRenderedHistory();
    const auto back =
        Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kFarDist));
    ASSERT_EQ(back.cursors[0], 2u);
    EXPECT_EQ(back.renderedPrevious[farSlot], GPUDrawStreamBuilder::kLodNoHistory);
    EXPECT_EQ(back.renderedCurrent[farSlot], MakeRenderedEntry(1u, false, false));
    EXPECT_EQ(back.renderedCurrent[nearSlot], MakeRenderedEntry(1u, false, true))
        << "the instance that kept drawing is unaffected by its neighbour's gap";

    RotateRenderedHistory();
    const auto recovered =
        Dispatch(table, total, 0u, 0u, 1u, 2u, 0u, {}, 0xFFFFFFFFu, {}, 0u, HistoryLod(kFarDist));
    EXPECT_EQ(recovered.renderedCurrent[farSlot], MakeRenderedEntry(1u, false, true));
}

// The phase half of the pair: what is recorded is whether the crossfade PAIR was
// actually emitted, not whether the state machine wanted to fade, so a consumer
// reading it knows which surface was on screen.
TEST_P(BatchScatterComputeTest, RenderedHistoryRecordsTheEmittedCrossfadePhase)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeCrossfadeMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = PairableFadeTable(m_Scene->GetBatchRegistry(), &total);
    ASSERT_EQ(total, 2u);

    SeedLodFade(/*instanceCount=*/1u, PackFadeLevels(1u, 1u), 0.0f); // settled at LOD1
    SeedRenderedHistory(std::vector<uint32_t>(1u, MakeRenderedEntry(1u, false, true)));

    LodDispatchParams lod  = HistoryLod(kFadeNearDist); // selects LOD0: a transition
    lod.crossfadeDuration  = kFadeDuration;
    lod.nowSeconds         = 10.0f;
    lod.tailCursorBase     = kFadeTailCursor;
    lod.tailRecordBase     = kFadeTailRecordBase;
    const auto r = Dispatch(table, total, 0u, 0u, kFadeCursorSlots, kFadeRecordSlots,
                            /*classMode=*/0u, {}, 0xFFFFFFFFu, {}, 0u, lod);

    ASSERT_EQ(r.cursors[kFadeTailCursor], 2u) << "the instance must be mid-dissolve here";
    EXPECT_EQ(r.renderedCurrent[0], MakeRenderedEntry(0u, /*fading=*/true, /*continuous=*/false))
        << "the incoming level plus the fading phase, and a phase change is a break";
}

// T24's companion: with the dwell band ON, the two histories describe the same
// rendered level and agree, and the band's own buffer and records are exactly
// what they are with the history off — storage sharing was rejected precisely so
// this could be asserted rather than argued.
TEST_P(BatchScatterComputeTest, RenderedHistoryAgreesWithTheDwellBandAndLeavesItUnchanged)
{
    const uint32_t meshIdx = m_Scene->AddMesh(MakeHysteresisMesh());
    m_Scene->AddInstance(MakeInstance(1u, meshIdx));
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(m_Scene->GetBatchRegistry(), {}, &total);
    ASSERT_EQ(total, 1u);

    // History OFF, band on, in the dwell region: the band holds LOD1.
    SeedPrevLod(1u, 1u);
    SeedRenderedHistory(std::vector<uint32_t>(1u, GPUDrawStreamBuilder::kLodNoHistory));
    LodDispatchParams banded = HistoryLod(kInBandDist);
    banded.hysteresisBand    = kHystBand;
    banded.renderedHistory   = false;
    const auto bandOnly =
        Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, banded);
    EXPECT_EQ(bandOnly.prevLod[0], 1u);
    EXPECT_EQ(bandOnly.records[0], kHystLod1Indices);

    // Same frame inputs with the history ON: identical band history, identical
    // record, and the recorded level is the level the band held.
    SeedPrevLod(1u, 1u);
    banded.renderedHistory = true;
    const auto both = Dispatch(table, total, 0u, 0u, 1u, 1u, 0u, {}, 0xFFFFFFFFu, {}, 0u, banded);
    EXPECT_EQ(both.prevLod[0], bandOnly.prevLod[0]) << "the band's own history is untouched";
    EXPECT_EQ(both.records, bandOnly.records);
    EXPECT_EQ(both.cursors, bandOnly.cursors);
    EXPECT_EQ(both.renderedCurrent[0], MakeRenderedEntry(1u, false, false))
        << "the recorded level is the level the band held, not the stateless pick";
}

INSTANTIATE_TEST_SUITE_P(
    GroupCompactVariants, GroupCompactComputeTest,
    ::testing::Values(ScatterVariant{false, false}, ScatterVariant{true, false}),
    [](const ::testing::TestParamInfo<ScatterVariant>& info)
    { return std::string(info.param.compact ? "Compact32B" : "FullFat240B"); });

std::string ScatterVariantName(const ::testing::TestParamInfo<ScatterVariant>& info)
{
    return std::string(info.param.compact ? "Compact32B" : "FullFat240B")
           + (info.param.stats ? "_StatsOn" : "_StatsOff");
}

// Every BatchScatterComputeTest body runs on the shipping package: the compact
// 32 B mirror at binding 1, stats off.
INSTANTIATE_TEST_SUITE_P(ScatterVariants, BatchScatterComputeTest,
                         ::testing::Values(ScatterVariant{true, false}), ScatterVariantName);

// The other three packages differ from the shipping one only in the binding-1
// struct (full-fat 240 B) and in the stats blocks, which write only sliceStats.
// Each runs the bodies that can tell it apart:
// - full-fat, stats off: the bodies that between them read every binding-1 field
//   the shader uses (meshIndex, materialIndex, the depth and mirror flag bits,
//   boundingCenter, boundingRadius, lodBias) with values that change the result;
// - compact, stats on: the drawnTriangles block, the lodChanges block (the band on
//   across two workgroups) and an invocation that writes two records;
// - full-fat, stats on: the core routing body, so every package is loaded and
//   asserted at least once.
class BatchScatterFullFatTest : public BatchScatterComputeTest
{
};

TEST_P(BatchScatterFullFatTest, ScattersInstancesIntoCorrectRegions)
{
    ExpectScattersInstancesIntoCorrectRegions();
}

TEST_P(BatchScatterFullFatTest, ShadowClassModeRoutesByFlagsIntoSentinelRegions)
{
    ExpectShadowClassModeRoutesByFlagsIntoSentinelRegions();
}

TEST_P(BatchScatterFullFatTest, MirroredInstancesRouteIntoParityOneRegion)
{
    ExpectMirroredInstancesRouteIntoParityOneRegion();
}

TEST_P(BatchScatterFullFatTest, ConservativeBoundsPreserveReferenceSwitchesAndBias)
{
    ExpectConservativeBoundsPreserveReferenceSwitchesAndBias();
}

INSTANTIATE_TEST_SUITE_P(ScatterPackageParity, BatchScatterFullFatTest,
                         ::testing::Values(ScatterVariant{false, false}), ScatterVariantName);

class BatchScatterStatsTest : public BatchScatterComputeTest
{
};

TEST_P(BatchScatterStatsTest, ScattersInstancesIntoCorrectRegions)
{
    ExpectScattersInstancesIntoCorrectRegions();
}

TEST_P(BatchScatterStatsTest, LodBandHistoryIsIndexedPerInstanceNotPerSlot)
{
    ExpectLodBandHistoryIsIndexedPerInstanceNotPerSlot();
}

TEST_P(BatchScatterStatsTest, CrossfadeLevelChangeEmitsItsPairIntoTheTailNotTheHead)
{
    ExpectCrossfadeLevelChangeEmitsItsPairIntoTheTailNotTheHead();
}

INSTANTIATE_TEST_SUITE_P(ScatterPackageParity, BatchScatterStatsTest,
                         ::testing::Values(ScatterVariant{true, true}), ScatterVariantName);

class BatchScatterFullFatStatsTest : public BatchScatterComputeTest
{
};

TEST_P(BatchScatterFullFatStatsTest, ScattersInstancesIntoCorrectRegions)
{
    ExpectScattersInstancesIntoCorrectRegions();
}

INSTANTIATE_TEST_SUITE_P(ScatterPackageParity, BatchScatterFullFatStatsTest,
                         ::testing::Values(ScatterVariant{false, true}), ScatterVariantName);

} // namespace
