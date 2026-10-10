// Host-side state tests for GPUDrawStreamBuilder's scatter arena: slice
// registration lifecycle, range-map queries, arena-frame reset, and the
// stream-ready barrier / zero-fill contract. GPU execution of the scatter
// pass is covered by BatchScatterComputeTests (real dispatch + readback) and
// RGGpuDrivenCoreTests (RenderGraph pass shape + ordering proxy).

#include <gtest/gtest.h>

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "TestDeviceHelper.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

// Loader that returns empty bytes -- pipeline creation fails gracefully,
// host-side arena management is still exercisable.
std::vector<uint8_t> NoOpShaderLoader(const char*) { return {}; }

using BatchTableEntry = GPUDrawStreamBuilder::BatchTableEntry;

// Locate a shadow/color batch table row by its (materialIndex, meshIndex) key.
// For the shadow table, materialIndex is the class key (sentinel or real).
std::optional<BatchTableEntry> FindEntry(const std::vector<BatchTableEntry>& table,
                                         uint32_t materialIndex, uint32_t meshIndex)
{
    for (const auto& e : table)
        if (e.materialIndex == materialIndex && e.meshIndex == meshIndex)
            return e;
    return std::nullopt;
}

constexpr uint8_t kSS  = static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided);
constexpr uint8_t kDS  = static_cast<uint8_t>(MaterialDepthClass::EligibleDoubleSided);
constexpr uint8_t kDep = static_cast<uint8_t>(MaterialDepthClass::MaterialDependent);

// matIdx 0,1 = single-sided eligible; 2 = double-sided eligible; 3 = dependent.
// (0,10)=3 (1,10)=2 (2,10)=4 (3,10)=1 (0,20)=7  => N = 17 live instances.
BatchRegistry MakeMixedRegistry()
{
    BatchRegistry reg;
    for (int i = 0; i < 3; ++i) reg.OnInstanceAdded(0u, 10u, /*mirrored=*/false);
    for (int i = 0; i < 2; ++i) reg.OnInstanceAdded(1u, 10u, /*mirrored=*/false);
    for (int i = 0; i < 4; ++i) reg.OnInstanceAdded(2u, 10u, /*mirrored=*/false);
    reg.OnInstanceAdded(3u, 10u, /*mirrored=*/false);
    for (int i = 0; i < 7; ++i) reg.OnInstanceAdded(0u, 20u, /*mirrored=*/false);
    return reg;
}

class GPUDrawStreamBuilderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        GPUDrawStreamBuilder::SetShaderLoader(&NoOpShaderLoader);
        m_Builder = std::make_unique<GPUDrawStreamBuilder>(m_Device.get());
    }

    void TearDown() override
    {
        if (m_Builder)
            m_Builder->Shutdown();
        m_Builder.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    std::unique_ptr<IDevice>              m_Device;
    std::unique_ptr<GPUDrawStreamBuilder> m_Builder;
};

} // namespace

// M2a survivor lookup: the pure (view, cascade, phase) match that drives the
// batch-walk early-out. A present slice returns its passed-caster count — 0 is a
// real "empty slice" answer (the skip signal), distinct from an absent slice,
// which returns the unknown sentinel so the caller records instead of skipping.
TEST(GPUDrawStreamShadowSurvivors, LookupMatchesViewCascadePhaseElseUnknown)
{
    using Stat  = GPUDrawStreamBuilder::ShadowArcSliceStat;
    using Phase = GPUDrawStreamBuilder::SlicePhase;
    // PointShadowCullingIndex(2): kPointShadowCullingIndexBase (0x10) + slot*6+face.
    // The lookup treats cascadeIndex as an opaque key; the value only mirrors the
    // real encoding (RenderServicesDetail.h is private to Engine/Source).
    constexpr uint8_t kPointFace2 = static_cast<uint8_t>(0x10u + 2u);
    const uint32_t kUnknown = GPUDrawStreamBuilder::kShadowSurvivorsUnknown;

    std::vector<Stat> stats = {
        Stat{/*viewId*/ 3u, /*cascadeIndex*/ 0u,          /*phase*/ 0u, /*passedCasters*/ 42u, 0u},
        Stat{/*viewId*/ 3u, /*cascadeIndex*/ kPointFace2, /*phase*/ 0u, /*passedCasters*/ 0u,  0u},
        Stat{/*viewId*/ 7u, /*cascadeIndex*/ 0u,          /*phase*/ 1u, /*passedCasters*/ 5u,  0u},
    };

    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors(stats, 3u, 0u, Phase::A), 42u);
    // A real zero (empty face) is returned AS zero — the early-out's skip signal.
    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors(stats, 3u, kPointFace2, Phase::A), 0u);
    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors(stats, 7u, 0u, Phase::B), 5u);

    // Any key mismatch → unknown (fail-safe: the caller must record, never skip).
    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors(stats, 3u, 0u, Phase::B), kUnknown);
    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors(stats, 3u, 1u, Phase::A), kUnknown);
    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors(stats, 9u, 0u, Phase::A), kUnknown);
    EXPECT_EQ(GPUDrawStreamBuilder::LookupShadowSurvivors({}, 3u, 0u, Phase::A), kUnknown);
}

TEST(GPUDrawStreamBuilderBarrierTest, StreamReadyBarrierIncludesTransferZeroFillAndComputeWrites)
{
    const auto barrier = GPUDrawStreamBuilder::CreateStreamReadyBarrier();

    EXPECT_EQ(barrier.type, ResourceBarrier::Memory);
    EXPECT_EQ(barrier.srcStageMask,
              static_cast<uint64_t>(PipelineStageMask::Transfer)
                  | static_cast<uint64_t>(PipelineStageMask::ComputeShader));
    EXPECT_EQ(barrier.dstStageMask,
              static_cast<uint64_t>(PipelineStageMask::DrawIndirect)
                  | static_cast<uint64_t>(PipelineStageMask::GraphicsVertex));
    EXPECT_EQ(barrier.srcAccessMask,
              static_cast<uint64_t>(ResourceAccessMask::TransferWrite)
                  | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite));
    EXPECT_EQ(barrier.dstAccessMask,
              static_cast<uint64_t>(ResourceAccessMask::IndirectCommandRead)
                  | static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
}

TEST_F(GPUDrawStreamBuilderTest, InitializeSucceedsWithoutShaderScatterPipelineLazyFails)
{
    // Initialize creates the ordering sentinel; the scatter pipeline is lazy
    // and fails gracefully (logged once, retried) when the package is absent.
    EXPECT_TRUE(m_Builder->Initialize());
    EXPECT_FALSE(m_Builder->GetOrCreateScatterPipeline().IsValid());
    EXPECT_TRUE(m_Builder->GetSentinelBuffer().IsValid());
}

TEST_F(GPUDrawStreamBuilderTest, SliceRegistrationLifecycle)
{
    ASSERT_TRUE(m_Builder->Initialize());
    EXPECT_EQ(m_Builder->GetPendingSliceCount(), 0u);

    GPUDrawStreamBuilder::SliceRegistration s{};
    s.viewId = 7u;
    EXPECT_EQ(s.table, GPUDrawStreamBuilder::SliceTable::Color) << "a slice is a color slice by default";
    m_Builder->RegisterSlice(s);
    s.cascadeIndex = 2u;
    s.table = GPUDrawStreamBuilder::SliceTable::Shadow;
    m_Builder->RegisterSlice(s);
    // A color fan-out slice (probe face): Color table at a non-None cascade.
    s.cascadeIndex = 0x80u;
    s.table = GPUDrawStreamBuilder::SliceTable::Color;
    m_Builder->RegisterSlice(s);
    EXPECT_EQ(m_Builder->GetPendingSliceCount(), 3u);

    // BeginArenaFrame drops unconsumed slices and any published ranges.
    m_Builder->BeginArenaFrame();
    EXPECT_EQ(m_Builder->GetPendingSliceCount(), 0u);
    EXPECT_EQ(m_Builder->GetRangeCount(), 0u);
}

TEST_F(GPUDrawStreamBuilderTest, FindBatchDrawRangeInvalidWhenUnpublished)
{
    ASSERT_TRUE(m_Builder->Initialize());
    const auto range = m_Builder->FindBatchDrawRange(
        1u, GPUDrawStreamBuilder::kCascadeIndexNone, 3u, 4u,
        GPUDrawStreamBuilder::SlicePhase::A);
    EXPECT_FALSE(range.IsValid());
    EXPECT_EQ(range.even.maxDrawCount, 0u);
    EXPECT_FALSE(range.recordBuffer.IsValid());
}

TEST_F(GPUDrawStreamBuilderTest, StreamKeyPhaseBitIsDisjointFromViewAndCascade)
{
    // The phase bit must produce a distinct key for otherwise-identical
    // coordinates, and must NOT alias any (viewId < 128, cascade) pair — the
    // top viewKey bit was ceded to the phase (design §5-A3).
    using B = GPUDrawStreamBuilder;
    const uint64_t a = B::MakeStreamKey(5u, B::kCascadeIndexNone, 7u, 9u, B::SlicePhase::A);
    const uint64_t b = B::MakeStreamKey(5u, B::kCascadeIndexNone, 7u, 9u, B::SlicePhase::B);
    EXPECT_NE(a, b);
    EXPECT_EQ(a & ~(1ull << 63), b & ~(1ull << 63)) << "phase is exactly bit 63";
    for (uint32_t v = 0; v < 128u; ++v)
        EXPECT_NE(B::MakeStreamKey(v, 0u, 7u, 9u, B::SlicePhase::A), b)
            << "phase-B keys must not collide with any 7-bit view's phase-A key";
}

TEST_F(GPUDrawStreamBuilderTest, ScatterStatsReadZeroWithoutArena)
{
    ASSERT_TRUE(m_Builder->Initialize());
    const auto stats = m_Builder->ReadScatterStats();
    EXPECT_EQ(stats.tableMisses, 0u);
    EXPECT_EQ(stats.overflows, 0u);
}

TEST_F(GPUDrawStreamBuilderTest, ShutdownClearsArenaState)
{
    ASSERT_TRUE(m_Builder->Initialize());
    GPUDrawStreamBuilder::SliceRegistration s{};
    m_Builder->RegisterSlice(s);
    m_Builder->Shutdown();
    EXPECT_EQ(m_Builder->GetPendingSliceCount(), 0u);
    EXPECT_EQ(m_Builder->GetRangeCount(), 0u);
    EXPECT_EQ(m_Builder->GetSharedIndirectionAddress(), 0ull);
}

// Mechanism anchor for the macOS/MoltenVK ghost-draw bug (`1f185dea`): on
// backends whose DrawIndexedIndirectCount EMULATES the count buffer by
// walking ALL maxDrawCount records (caps.supportsDrawIndirectCountNative ==
// false), the scatter's transfer phase must zero-fill this frame's record
// range — unused records must read indexCount=0 to produce degenerate no-op
// draws. Native-count backends consume exactly `count` records, so only the
// cursor words are cleared.
//
// This test exercises the FillBuffer + stream-ready barrier mechanism on a
// record-shaped buffer; the conditional fill decision itself is untestable
// on native-count devices, Metal included. Only a Vulkan device without
// drawIndirectCount (MoltenVK, for example) and WebGPU still run the
// emulated path.
TEST_F(GPUDrawStreamBuilderTest, FillBuffer_ZeroesRecordRangeForEmulationContract)
{
    constexpr uint32_t kRecords         = 32u;
    constexpr size_t   kBytesPerCommand = 5 * sizeof(uint32_t); // VkDrawIndexedIndirectCommand
    constexpr size_t   kRecordBytes     = static_cast<size_t>(kRecords) * kBytesPerCommand;

    BufferDesc desc{};
    desc.size  = kRecordBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage)
               | static_cast<uint32_t>(BufferUsage::Indirect)
               | static_cast<uint32_t>(BufferUsage::TransferDst)
               | static_cast<uint32_t>(BufferUsage::TransferSrc);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.debugName   = "test.recordFillContract";
    const BufferHandle records = m_Device->CreateBuffer(desc);
    ASSERT_TRUE(records.IsValid());

    // Pollute with a non-zero pattern — stands in for stale prior-frame
    // records the emulation path would replay as ghost draws.
    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl);
    cl->Begin();
    cl->FillBuffer(records, 0, kRecordBytes, 0xDEADBEEFu);
    cl->End();
    {
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    // Apply the same fill + barrier the scatter pass issues.
    auto cl2 = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl2);
    cl2->Begin();
    cl2->FillBuffer(records, 0, kRecordBytes, 0u);
    cl2->Barrier(GPUDrawStreamBuilder::CreateStreamReadyBarrier());
    cl2->End();
    {
        std::vector<CommandList*> lists{cl2.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    BufferHandle rb = m_Device->CreateReadbackBuffer(kRecordBytes, "RecordFillRB");
    ASSERT_TRUE(rb.IsValid());
    auto cl3 = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl3->Begin();
    cl3->CopyBuffer(records, rb, kRecordBytes);
    cl3->End();
    {
        std::vector<CommandList*> lists{cl3.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::vector<uint32_t> data(kRecordBytes / sizeof(uint32_t), 0xFFFFFFFFu);
    void* mapped = m_Device->MapBuffer(rb);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(data.data(), mapped, kRecordBytes);
    m_Device->UnmapBuffer(rb);
    for (uint32_t v : data)
        ASSERT_EQ(v, 0u) << "record range must read fully zeroed after the fill";

    m_Device->DestroyBuffer(rb);
    m_Device->DestroyBuffer(records);
}

// ---- BuildShadowBatchTable: (depth-class, mesh) grouping (R1.5) ------------
//
// Pure host-side: no device. The shadow table merges shared-depth-eligible
// materials of a mesh into two class sentinels (single/double-sided) while
// material-dependent casters keep their (materialIndex, mesh) identity.

TEST(BuildShadowBatchTable, MergesEligibleMaterialsOfSameMeshAndSide)
{
    const BatchRegistry reg = MakeMixedRegistry();
    const std::vector<uint8_t> classes{kSS, kSS, kDS, kDep};

    uint32_t total = 0u;
    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {}, &total);

    // mat0 + mat1 (both single-sided eligible) on mesh 10 collapse to one
    // single-sided sentinel row with summed count 3 + 2 = 5.
    const auto ss10 =
        FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 10u);
    ASSERT_TRUE(ss10.has_value());
    EXPECT_EQ(ss10->capacity, 5u);

    // mat0 on mesh 20 is a distinct mesh -> its own single-sided sentinel row.
    const auto ss20 =
        FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 20u);
    ASSERT_TRUE(ss20.has_value());
    EXPECT_EQ(ss20->capacity, 7u);
}

TEST(BuildShadowBatchTable, SingleAndDoubleSidedEligibleSplitBySentinel)
{
    const BatchRegistry reg = MakeMixedRegistry();
    const std::vector<uint8_t> classes{kSS, kSS, kDS, kDep};

    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {});

    // Same mesh 10, opposite sidedness -> two separate class-sentinel rows.
    const auto ss =
        FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 10u);
    const auto ds =
        FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel, 10u);
    ASSERT_TRUE(ss.has_value());
    ASSERT_TRUE(ds.has_value());
    EXPECT_EQ(ss->capacity, 5u); // mat0(3) + mat1(2)
    EXPECT_EQ(ds->capacity, 4u); // mat2(4)
}

TEST(BuildShadowBatchTable, MaterialDependentKeepsIdentity)
{
    const BatchRegistry reg = MakeMixedRegistry();
    const std::vector<uint8_t> classes{kSS, kSS, kDS, kDep};

    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {});

    // mat3 is dependent: it must NOT fold into a sentinel; it keeps its real
    // (materialIndex, mesh) row, and no sentinel absorbed its count.
    const auto dep = FindEntry(shadow, 3u, 10u);
    ASSERT_TRUE(dep.has_value());
    EXPECT_EQ(dep->capacity, 1u);
}

TEST(BuildShadowBatchTable, SentinelsSortAboveRealMaterialIndices)
{
    const BatchRegistry reg = MakeMixedRegistry();
    const std::vector<uint8_t> classes{kSS, kSS, kDS, kDep};

    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {});

    // Rows are sorted by (materialIndex, meshIndex); the two sentinels are the
    // top of the 24-bit domain, so every real materialIndex sorts before them.
    // This is what keeps the shader's binary search valid unchanged.
    ASSERT_FALSE(shadow.empty());
    EXPECT_EQ(shadow.front().materialIndex, 3u); // the only real (dependent) index
    for (size_t i = 1; i < shadow.size(); ++i)
        EXPECT_LE(shadow[i - 1].materialIndex, shadow[i].materialIndex);

    // Exclusive-prefix record offsets are contiguous and ordered.
    uint32_t running = 0u;
    for (const auto& e : shadow)
    {
        EXPECT_EQ(e.recordOffset, running);
        running += e.capacity;
    }
}

TEST(BuildShadowBatchTable, TotalRecordsEqualsColorTableTotal)
{
    const BatchRegistry reg = MakeMixedRegistry();
    const std::vector<uint8_t> classes{kSS, kSS, kDS, kDep};

    uint32_t colorTotal = 0u;
    const auto color = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &colorTotal);
    uint32_t shadowTotal = 0u;
    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {}, &shadowTotal);

    // Merging only reduces the row count, never the record count.
    EXPECT_EQ(colorTotal, 17u);
    EXPECT_EQ(shadowTotal, colorTotal);
    EXPECT_LE(shadow.size(), color.size());
}

TEST(BuildShadowBatchTable, EmptyClassSpanDegradesToMaterialDependent)
{
    const BatchRegistry reg = MakeMixedRegistry();

    uint32_t total = 0u;
    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, {}, {}, &total);

    // With no class information every material is treated as dependent, so no
    // sentinel rows appear and each (mat, mesh) keeps its identity — same shape
    // as the color table. Safe fallback: nothing ever merges wrongly.
    EXPECT_FALSE(FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 10u)
                     .has_value());
    EXPECT_FALSE(FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel, 10u)
                     .has_value());
    ASSERT_TRUE(FindEntry(shadow, 0u, 10u).has_value());
    EXPECT_EQ(FindEntry(shadow, 0u, 10u)->capacity, 3u);
    EXPECT_EQ(total, 17u);
}

// ---- Winding parity: mirrored (negative-determinant) sibling rows ----------
//
// A mirrored instance rides parity bit 24 of the meshIndex field. The color
// tables split every batch into parity-0/parity-1 sibling rows; the shadow
// table splits single-sided + material-dependent classes but NOT the
// double-sided sentinel (its casters are cull-None and the scatter forces
// their parity to 0). Mirror-free registries must produce a byte-identical
// table to the pre-parity layout (the bench-invariant guarantee).

namespace
{
constexpr uint32_t kTestMeshParityBit = 1u << 24; // must match GPUDrawStreamBuilder's kMeshParityBit
}

TEST(BuildBatchTableParity, SplitsMirroredIntoSiblingSortedAfterEven)
{
    BatchRegistry reg;
    for (int i = 0; i < 3; ++i) reg.OnInstanceAdded(2u, 5u, /*mirrored=*/false);
    for (int i = 0; i < 2; ++i) reg.OnInstanceAdded(2u, 5u, /*mirrored=*/true);

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &total);
    ASSERT_EQ(table.size(), 2u);
    EXPECT_EQ(total, 5u); // conservation: even(3) + odd(2)

    // Parity-0 row sorts before its parity-1 sibling (bit 24 is high).
    EXPECT_EQ(table[0].materialIndex, 2u);
    EXPECT_EQ(table[0].meshIndex, 5u);
    EXPECT_EQ(table[0].capacity, 3u);
    EXPECT_EQ(table[0].recordOffset, 0u);
    EXPECT_EQ(table[1].materialIndex, 2u);
    EXPECT_EQ(table[1].meshIndex, 5u | kTestMeshParityBit);
    EXPECT_EQ(table[1].capacity, 2u);
    EXPECT_EQ(table[1].recordOffset, 3u);
}

TEST(BuildBatchTableParity, AllMirroredBatchProducesOnlyTheParityOneRow)
{
    BatchRegistry reg;
    for (int i = 0; i < 4; ++i) reg.OnInstanceAdded(1u, 9u, /*mirrored=*/true);

    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &total);
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(total, 4u);
    EXPECT_EQ(table[0].meshIndex, 9u | kTestMeshParityBit) << "no even instances → no parity-0 row";
    EXPECT_EQ(table[0].capacity, 4u);
}

TEST(BuildBatchTableParity, MirrorFreeRegistryIsByteIdenticalToPreParityLayout)
{
    // Every row is parity-0 with capacity == the whole batch, sorted the same
    // way, no parity bit set — the bench-invariant guarantee by construction.
    const BatchRegistry reg = MakeMixedRegistry();
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &total);
    EXPECT_EQ(total, 17u);
    EXPECT_EQ(table.size(), reg.BatchCount()); // one row per batch, no extra parity rows
    for (const auto& e : table)
        EXPECT_EQ(e.meshIndex & kTestMeshParityBit, 0u) << "mirror-free table has no parity-1 rows";
    // Spot-check a known batch is exactly the old row.
    const auto e = FindEntry(table, 0u, 20u);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->capacity, 7u);
}

TEST(BuildShadowBatchTableParity, DoubleSidedSentinelDoesNotSplit)
{
    // A mirrored double-sided-eligible batch must collapse both parities into
    // the parity-0 sentinel row (must-fix #1): the scatter forces DS parity to
    // 0, so a parity-1 row would be dead and a mirrored DS caster searching it
    // would have its shadow silently dropped.
    BatchRegistry reg;
    for (int i = 0; i < 3; ++i) reg.OnInstanceAdded(0u, 10u, /*mirrored=*/false);
    for (int i = 0; i < 2; ++i) reg.OnInstanceAdded(0u, 10u, /*mirrored=*/true);
    const std::vector<uint8_t> classes{kDS};

    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {});
    const auto ds0 =
        FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel, 10u);
    ASSERT_TRUE(ds0.has_value());
    EXPECT_EQ(ds0->capacity, 5u) << "even(3) + odd(2) collapse into the parity-0 sentinel row";
    EXPECT_FALSE(FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel,
                           10u | kTestMeshParityBit)
                     .has_value())
        << "double-sided sentinel must never emit a parity-1 sibling";
}

TEST(BuildShadowBatchTableParity, SingleSidedAndDependentSplit)
{
    // Single-sided sentinel and material-dependent rows DO split (their shadow
    // draws cull Back, so mirrored casters need the flipped winding).
    BatchRegistry reg;
    for (int i = 0; i < 3; ++i) reg.OnInstanceAdded(0u, 10u, /*mirrored=*/false); // SS
    for (int i = 0; i < 2; ++i) reg.OnInstanceAdded(0u, 10u, /*mirrored=*/true);  // SS mirrored
    reg.OnInstanceAdded(1u, 10u, /*mirrored=*/false);                             // dependent
    reg.OnInstanceAdded(1u, 10u, /*mirrored=*/true);                              // dependent mirrored
    const std::vector<uint8_t> classes{kSS, kDep};

    const auto shadow = GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, {});
    // SS sentinel: parity-0 (3) + parity-1 (2).
    const auto ss0 = FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel, 10u);
    const auto ss1 = FindEntry(shadow, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel,
                               10u | kTestMeshParityBit);
    ASSERT_TRUE(ss0.has_value() && ss1.has_value());
    EXPECT_EQ(ss0->capacity, 3u);
    EXPECT_EQ(ss1->capacity, 2u);
    // Dependent (real mat 1): parity-0 (1) + parity-1 (1).
    const auto dep0 = FindEntry(shadow, 1u, 10u);
    const auto dep1 = FindEntry(shadow, 1u, 10u | kTestMeshParityBit);
    ASSERT_TRUE(dep0.has_value() && dep1.has_value());
    EXPECT_EQ(dep0->capacity, 1u);
    EXPECT_EQ(dep1->capacity, 1u);
}

// ---- BuildColorBatchTable: (color-class, mesh) grouping (P2) ---------------
//
// Pure host-side: no device. The color table merges same-PSO opaque casters
// of a mesh into their downward-allocated colorClassId while blend /
// transmissive / material-dependent casters keep their real (materialIndex,
// mesh) identity — exactly mirroring the shadow table, but driven by the
// per-materialIndex color-class map instead of the depth-class enum.

namespace
{
// mat0,mat1 share color class C0; mat2 keeps identity (e.g. transmissive).
// (0,10)=3 (1,10)=2 (2,10)=1 (0,20)=7  => 13 live instances.
BatchRegistry MakeColorRegistry()
{
    BatchRegistry reg;
    for (int i = 0; i < 3; ++i) reg.OnInstanceAdded(0u, 10u, /*mirrored=*/false);
    for (int i = 0; i < 2; ++i) reg.OnInstanceAdded(1u, 10u, /*mirrored=*/false);
    reg.OnInstanceAdded(2u, 10u, /*mirrored=*/false);
    for (int i = 0; i < 7; ++i) reg.OnInstanceAdded(0u, 20u, /*mirrored=*/false);
    return reg;
}

constexpr uint32_t kC0 = GPUDrawStreamBuilder::kColorClassBase; // first class id
} // namespace

TEST(BuildColorBatchTable, MergesSameClassMaterialsAndKeepsIdentityRows)
{
    const BatchRegistry reg = MakeColorRegistry();
    // map[mat] : mat0,mat1 -> class C0 ; mat2 -> identity (2).
    const std::vector<uint32_t> map{kC0, kC0, 2u};

    uint32_t total = 0u;
    const auto color = GPUDrawStreamBuilder::BuildColorBatchTable(reg, map, {}, &total);

    // mat0 + mat1 on mesh 10 collapse into one class-C0 row (3 + 2 = 5).
    const auto c10 = FindEntry(color, kC0, 10u);
    ASSERT_TRUE(c10.has_value());
    EXPECT_EQ(c10->capacity, 5u);
    // mat0 on mesh 20 is a distinct mesh -> its own class-C0 row.
    const auto c20 = FindEntry(color, kC0, 20u);
    ASSERT_TRUE(c20.has_value());
    EXPECT_EQ(c20->capacity, 7u);
    // mat2 is identity: it keeps its real (materialIndex, mesh) row, unmerged.
    const auto id10 = FindEntry(color, 2u, 10u);
    ASSERT_TRUE(id10.has_value());
    EXPECT_EQ(id10->capacity, 1u);

    EXPECT_EQ(total, 13u);
}

TEST(BuildColorBatchTable, TotalRecordsEqualsUnmergedColorTable)
{
    const BatchRegistry reg = MakeColorRegistry();
    const std::vector<uint32_t> map{kC0, kC0, 2u};

    uint32_t baseTotal = 0u;
    const auto base = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &baseTotal);
    uint32_t mergedTotal = 0u;
    const auto merged = GPUDrawStreamBuilder::BuildColorBatchTable(reg, map, {}, &mergedTotal);

    // Merging only reduces the row count, never the record count (preserves the
    // shadowRecords == colorRecords contract in ScheduleUnifiedScatter).
    EXPECT_EQ(mergedTotal, baseTotal);
    EXPECT_LT(merged.size(), base.size());

    // Contiguous, ordered exclusive-prefix offsets, class ids sort above reals.
    uint32_t running = 0u;
    for (const auto& e : merged)
    {
        EXPECT_EQ(e.recordOffset, running);
        running += e.capacity;
    }
}

TEST(BuildColorBatchTable, EmptySpanDegradesToUnmergedTable)
{
    const BatchRegistry reg = MakeColorRegistry();

    uint32_t total = 0u;
    const auto color = GPUDrawStreamBuilder::BuildColorBatchTable(reg, {}, {}, &total);

    // No class information -> every material keeps identity, same shape as
    // BuildBatchTable. Safe fallback (nothing merges wrongly).
    EXPECT_FALSE(FindEntry(color, kC0, 10u).has_value());
    ASSERT_TRUE(FindEntry(color, 0u, 10u).has_value());
    EXPECT_EQ(FindEntry(color, 0u, 10u)->capacity, 3u);
    ASSERT_TRUE(FindEntry(color, 1u, 10u).has_value());
    EXPECT_EQ(FindEntry(color, 1u, 10u)->capacity, 2u);
    EXPECT_EQ(total, 13u);
}

TEST(BuildColorBatchTable, ClassIdDomainStaysDisjointFromRealMaterialIndices)
{
    const BatchRegistry reg = MakeColorRegistry();
    const std::vector<uint32_t> map{kC0, kC0, 2u};

    const auto color = GPUDrawStreamBuilder::BuildColorBatchTable(reg, map, {});

    // Every merged (class) row's key is a downward-from-base class id; every
    // identity row's key is a small real materialIndex. The two domains must be
    // strictly separated so no class group folds an identity row.
    uint32_t maxIdentity = 0u;
    uint32_t minClass    = 0xFFFFFFFFu;
    bool anyIdentity = false, anyClass = false;
    for (const auto& e : color)
    {
        if (e.materialIndex >= GPUDrawStreamBuilder::kColorClassBase - 8u) // class region
        {
            anyClass = true;
            minClass = std::min(minClass, e.materialIndex);
        }
        else
        {
            anyIdentity = true;
            maxIdentity = std::max(maxIdentity, e.materialIndex);
        }
    }
    ASSERT_TRUE(anyIdentity && anyClass);
    EXPECT_LT(maxIdentity, minClass);
}

// Constant ordering: color class ids allocate downward strictly below the R1.5
// shadow sentinels, so the two schemes can never share a value.
static_assert(GPUDrawStreamBuilder::kColorClassBase
                  < GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel,
              "color class base must stay below the shadow class sentinels");

#if !defined(NDEBUG)
TEST(BuildColorBatchTableDeathTest, DomainDisjointnessAssertFiresOnAliasingClassId)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    BatchRegistry reg;
    reg.OnInstanceAdded(0u, 10u, /*mirrored=*/false);
    reg.OnInstanceAdded(1u, 10u, /*mirrored=*/false);
    // Synthetic violation: mat0 maps to class value 1, but mat1 is identity 1 —
    // the class id aliases a real materialIndex present in the same table.
    const std::vector<uint32_t> bad{1u, 1u};
    ASSERT_DEATH({ GPUDrawStreamBuilder::BuildColorBatchTable(reg, bad, {}); }, "");
}
#endif

// ---- Grouped tables (draw consolidation) -----------------------------------
// The meshPoolGroup span remaps the mesh axis to geometry-bind group ids,
// merging rows across meshes that share pools. Σ capacities is invariant; the
// empty span keeps today's per-mesh tables bit-for-bit (the
// GE_DRAW_CONSOLIDATION=0 contract).

TEST(BuildGroupedTables, MergesRowsAcrossMeshesSharingPoolGroup)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(1u, 0u, false); // group 5
    reg.OnInstanceAdded(1u, 1u, false); // group 5
    reg.OnInstanceAdded(1u, 2u, false); // group 9

    const std::vector<uint32_t> groups{5u, 5u, 9u};
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, groups, &total);
    ASSERT_EQ(table.size(), 2u);
    EXPECT_EQ(total, 3u);
    EXPECT_EQ(table[0].materialIndex, 1u);
    EXPECT_EQ(table[0].meshIndex, 5u); // group id in the mesh field
    EXPECT_EQ(table[0].capacity, 2u);  // meshes 0+1 merged
    EXPECT_EQ(table[0].recordOffset, 0u);
    EXPECT_EQ(table[1].meshIndex, 9u);
    EXPECT_EQ(table[1].capacity, 1u);
    EXPECT_EQ(table[1].recordOffset, 2u);
}

TEST(BuildGroupedTables, ColorClassAndGroupAxesComposeSigmaPreserved)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(0u, 0u, false);
    reg.OnInstanceAdded(1u, 1u, false); // same class as mat 0, same group
    reg.OnInstanceAdded(2u, 2u, false); // identity (transmissive), own group

    const uint32_t C0 = GPUDrawStreamBuilder::kColorClassBase;
    const std::vector<uint32_t> classMap{C0, C0, 2u};
    const std::vector<uint32_t> groups{4u, 4u, 6u};
    uint32_t total = 0u;
    const auto table =
        GPUDrawStreamBuilder::BuildColorBatchTable(reg, classMap, groups, &total);
    ASSERT_EQ(table.size(), 2u);
    EXPECT_EQ(total, 3u);
    // (2, 6) identity row sorts below (C0, 4).
    EXPECT_EQ(table[0].materialIndex, 2u);
    EXPECT_EQ(table[0].meshIndex, 6u);
    EXPECT_EQ(table[0].capacity, 1u);
    EXPECT_EQ(table[1].materialIndex, C0);
    EXPECT_EQ(table[1].meshIndex, 4u);
    EXPECT_EQ(table[1].capacity, 2u); // cross-mesh + cross-material merge
}

TEST(BuildGroupedTables, ShadowDoubleSidedSentinelDoesNotSplitOnGroupAxis)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(0u, 0u, /*mirrored=*/false); // DS eligible, group 3
    reg.OnInstanceAdded(0u, 1u, /*mirrored=*/true);  // DS eligible, group 3

    const std::vector<uint8_t> classes{
        static_cast<uint8_t>(MaterialDepthClass::EligibleDoubleSided)};
    const std::vector<uint32_t> groups{3u, 3u};
    uint32_t total = 0u;
    const auto shadow =
        GPUDrawStreamBuilder::BuildShadowBatchTable(reg, classes, groups, &total);
    ASSERT_EQ(shadow.size(), 1u) << "DS sentinel collapses parity on the group axis too";
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(shadow[0].materialIndex, GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel);
    EXPECT_EQ(shadow[0].meshIndex, 3u);
    EXPECT_EQ(shadow[0].capacity, 2u);
}

TEST(BuildGroupedTables, ParitySiblingsSplitOnGroupAxis)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(1u, 0u, /*mirrored=*/false); // group 2
    reg.OnInstanceAdded(1u, 1u, /*mirrored=*/true);  // group 2

    const std::vector<uint32_t> groups{2u, 2u};
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, groups, &total);
    ASSERT_EQ(table.size(), 2u);
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(table[0].meshIndex, 2u);              // parity-0 group row
    EXPECT_EQ(table[1].meshIndex, 2u | (1u << 24)); // parity-1 sibling
    EXPECT_EQ(table[0].capacity, 1u);
    EXPECT_EQ(table[1].capacity, 1u);
}

TEST(BuildGroupedTables, MeshesPastTheSpanMergeUnderTheAbsentKey)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(1u, 5u, false); // past the 2-entry span
    reg.OnInstanceAdded(1u, 6u, false); // past the span too

    const std::vector<uint32_t> groups{0u, 0u};
    uint32_t total = 0u;
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(reg, groups, &total);
    // Both rows merge under the absent pseudo group; Σ capacities preserved.
    // Dead rows by construction: their instances self-reject at the scatter's
    // mesh-row checks, and consumers skip them via the entry-missing guard.
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].meshIndex, 0xFFFFFFu);
    EXPECT_EQ(table[0].capacity, 2u);
    EXPECT_EQ(total, 2u);
}

// ---- Crossfade tail region -------------------------------------------------

// A tail row is claimed in PAIRS from slot 0, so its written prefix is always
// even. An odd-capacity row therefore leaves its last slot permanently
// unwritten, and the consumer bound must round down or that slot is drawn as
// garbage. This is the whole contract behind TailDrawBound.
TEST(GPUDrawStreamCrossfadeTail, DrawBoundRoundsDownToWholePairs)
{
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(0u), 0u);
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(1u), 0u) << "one slot cannot hold a pair";
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(2u), 2u);
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(3u), 2u);
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(7u), 6u);
    EXPECT_EQ(GPUDrawStreamBuilder::TailDrawBound(8u), 8u);
}

// The tail addresses rows through the HEAD table's own recordOffset/capacity
// against a different block base, which is what lets one table, one run map and
// one compact shader serve both regions. That only works while the two tables
// keep their pre-existing Sigma-capacity lockstep, since one uniform per-slice
// record stride places both kinds of slice AND both regions.
TEST(GPUDrawStreamCrossfadeTail, ColourAndShadowTablesKeepEqualRecordTotals)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(2u, 5u, /*mirrored=*/false);
    reg.OnInstanceAdded(2u, 5u, /*mirrored=*/false);
    reg.OnInstanceAdded(1u, 9u, /*mirrored=*/false);
    reg.OnInstanceAdded(3u, 7u, /*mirrored=*/true);

    // Every material eligible single-sided, so the shadow table MERGES rows the
    // colour table keeps apart -- the case where the two totals could diverge.
    const std::vector<uint8_t> depthClass(4u, kSS);

    uint32_t colorTotal = 0u, shadowTotal = 0u;
    const auto color = GPUDrawStreamBuilder::BuildBatchTable(reg, {}, &colorTotal);
    (void)GPUDrawStreamBuilder::BuildShadowBatchTable(reg, depthClass, {}, &shadowTotal);

    EXPECT_EQ(shadowTotal, colorTotal);
    EXPECT_EQ(colorTotal, 4u) << "capacity is the snapshot live count, never scaled";

    uint32_t expectedOffset = 0u;
    for (size_t i = 0; i < color.size(); ++i)
    {
        EXPECT_EQ(color[i].recordOffset, expectedOffset) << "row " << i;
        expectedOffset += color[i].capacity;
    }
    EXPECT_EQ(expectedOffset, colorTotal);
}

// Cursor blocks are claimed per slice, so non-aliasing is the prefix sum rather
// than a uniform stride. This walks the returned bases and asserts the blocks
// tile the claim exactly with no gap and no overlap -- the property `s * stride`
// used to give for free, and the one an off-by-one here would destroy silently
// (a slice would sum another slice's cursors, or scatter into them).
TEST(GPUDrawStreamCrossfadeTail, PerSliceCursorBlocksTileTheClaimWithoutAliasing)
{
    constexpr uint32_t kNone = GPUDrawStreamBuilder::kNoTailBlock;
    constexpr uint32_t kHalf = 7u;
    // A crossfading call as the frame spine builds one: the main camera slice
    // owns a tail, the four cascades and the phase-B bucket do not.
    const std::vector<uint32_t> tailIndex{0u, kNone, kNone, kNone, kNone, kNone};

    uint32_t total = 0u;
    const auto offsets = GPUDrawStreamBuilder::BuildSliceCursorOffsets(tailIndex, kHalf, &total);
    ASSERT_EQ(offsets.size(), tailIndex.size());

    // One slice doubles, five do not: 2*7 + 5*7 = 49, against 6*14 = 84 uniform.
    EXPECT_EQ(total, 49u) << "uniform striding would have claimed " << (6u * 2u * kHalf);

    uint32_t expected = 0u;
    for (size_t s = 0; s < offsets.size(); ++s)
    {
        EXPECT_EQ(offsets[s], expected) << "slice " << s << " base";
        expected += (tailIndex[s] != kNone) ? kHalf * 2u : kHalf;
    }
    EXPECT_EQ(expected, total) << "blocks must tile the claim exactly";

    // A tail half must land inside its own slice's block, never the next one's.
    for (size_t s = 0; s < offsets.size(); ++s)
    {
        if (tailIndex[s] == kNone)
            continue;
        const uint32_t blockEnd = offsets[s] + kHalf * 2u;
        EXPECT_LE(offsets[s] + kHalf + kHalf, blockEnd) << "tail half escapes slice " << s;
        if (s + 1 < offsets.size())
            EXPECT_LE(blockEnd, offsets[s + 1]) << "slice " << s << " overlaps its successor";
    }
}

// Every slice tail-capable is the degenerate case the prefix sum must still get
// right -- it is the only shape where the old uniform stride was also correct,
// so a regression here would look like "nothing changed".
TEST(GPUDrawStreamCrossfadeTail, AllSlicesTailCapableMatchesTheDoubledUniformClaim)
{
    constexpr uint32_t kHalf = 5u;
    const std::vector<uint32_t> tailIndex{0u, 1u, 2u};
    uint32_t total = 0u;
    const auto offsets = GPUDrawStreamBuilder::BuildSliceCursorOffsets(tailIndex, kHalf, &total);
    EXPECT_EQ(total, 3u * 2u * kHalf);
    for (size_t s = 0; s < offsets.size(); ++s)
        EXPECT_EQ(offsets[s], static_cast<uint32_t>(s) * 2u * kHalf);
}

// A call with no crossfading slice must claim exactly what it claimed before the
// tail existed -- the feature-off byte-identity contract.
TEST(GPUDrawStreamCrossfadeTail, NoTailCapableSliceClaimsTheUndoubledStride)
{
    constexpr uint32_t kNone = GPUDrawStreamBuilder::kNoTailBlock;
    constexpr uint32_t kHalf = 11u;
    const std::vector<uint32_t> tailIndex{kNone, kNone, kNone, kNone};
    uint32_t total = 0u;
    const auto offsets = GPUDrawStreamBuilder::BuildSliceCursorOffsets(tailIndex, kHalf, &total);
    EXPECT_EQ(total, 4u * kHalf);
    for (size_t s = 0; s < offsets.size(); ++s)
        EXPECT_EQ(offsets[s], static_cast<uint32_t>(s) * kHalf);
}

// ---- Scatter descriptor layout + push-constant shape -----------------------

TEST(GPUDrawStreamScatterLayout, HasFourteenStorageBufferBindings)
{
    const auto layout = GPUDrawStreamBuilder::MakeScatterDescriptorSetLayout();
    ASSERT_EQ(layout.bindings.size(), 14u);
    for (uint32_t i = 0; i < 14u; ++i)
    {
        EXPECT_EQ(layout.bindings[i].binding, i);
        EXPECT_EQ(layout.bindings[i].type, DescriptorType::StorageBuffer);
    }
}

static_assert(sizeof(GPUDrawStreamBuilder::ScatterPushConstants) == 100,
              "the PC block is 100 B: 13 arena/LOD/routing words + the P0 shadow-arc "
              "statsBase + the consolidation meshGroupMode + the small-object cull "
              "threshold + the two per-view SSE coverage scales + the five LOD "
              "crossfade words (clock, 1/duration, tail-active, tail cursor base, "
              "tail record base) + the LOD dwell band + the rendered level and phase "
              "history gate; must match draw_command_scatter.comp's push_constant block");
static_assert(GPUDrawStreamBuilder::kClassModeOff == 0u
                  && GPUDrawStreamBuilder::kClassModeShadow == 1u
                  && GPUDrawStreamBuilder::kClassModeColor == 2u,
              "classMode values must match the shader's kClassMode* constants");
