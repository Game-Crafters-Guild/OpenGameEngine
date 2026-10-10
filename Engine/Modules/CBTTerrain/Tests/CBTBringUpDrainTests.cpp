// GPU-resource-lifetime policy oracles for the CBT bring-up path
// (design gpu-resource-lifetime-policy, slice a).
//
// Two independent things are pinned here:
//
//  1. DRAIN-FREEDOM. No CBT entry point reachable from the render-graph declare phase may call
//     IDevice::WaitForIdle. A device drain does not just stall: it flips VulkanDevice's
//     m_DeviceKnownIdle, which silently reroutes every later-declaring node's resource destroys
//     to the immediate path for the rest of the declare phase. The counter these tests read is
//     itself under test first (IdleDrainCounterMovesOnADrain) — a zero delta read from an
//     instrument that cannot move proves nothing.
//
//  2. SEED STATE. The ordering that replaced those drains is expressed in barriers inside one
//     submission, so the seeded tree must still be byte-identical and must be observable BEFORE
//     the first update runs. Every assertion here reads the state InitializeRoots itself wrote,
//     not state a subsequent update rebuilt.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "Rendering/Core/CommandList.h"
#include "CBTTestHarness.h"
#include "../../Rendering/Source/Vulkan/VulkanCommandList.h"
#include "../../Rendering/Source/Vulkan/VulkanDevice.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
// CBTNeighbors and CBTBisectorData word strides, for indexing a raw u32 readback.
constexpr uint32_t kNeighborWords = sizeof(CBTNeighbors) / 4u;         // 4
constexpr uint32_t kBisectorDataWords = sizeof(CBTBisectorData) / 4u;  // 8
constexpr uint32_t kBisectorFlagsWord = 2u;                            // CBTBisectorData::Flags
constexpr uint32_t kHeapIdWords = 2u;                                  // u64 per slot

// Runs one update + readback the way the render graph does, then waits. The wait is test-side
// (a test is not runtime code); every drain assertion below brackets only the CBT call it is
// about, so these waits cannot mask a regression.
void RunUpdate(IDevice& device, CBTInstance& instance, uint32_t frameIndex)
{
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, CBTInertClassify(), CBTIdentityFrameParams(), frameIndex);
    instance.RecordReadback(*cl);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
}

// An unsignaled host event keeps a real update pending independently of GPU speed.
// The bounded host release also makes a mistakenly restored device drain fail its
// counter assertion instead of hanging the suite.
class PendingUpdate
{
  public:
    explicit PendingUpdate(IDevice& device) : m_Device(device) {}

    ~PendingUpdate()
    {
        if (m_ReleaseThread.joinable())
        {
            m_ReleaseThread.request_stop();
            m_ReleaseThread.join();
        }
        if (m_Event != VK_NULL_HANDLE)
        {
            Release();
            m_Device.WaitGpuSyncToken(m_Token);
            vkDestroyEvent(NativeDevice(), m_Event, nullptr);
        }
    }

    bool Submit(CBTInstance& instance, uint32_t frameIndex)
    {
        VkEventCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        if (vkCreateEvent(NativeDevice(), &info, nullptr, &m_Event) != VK_SUCCESS)
            return false;
        m_Commands = m_Device.CreateCommandList(IDevice::QueueType::Graphics);
        if (!m_Commands)
            return false;
        m_Commands->Begin();
        const auto native = static_cast<VulkanCommandList&>(*m_Commands).GetVkCommandBuffer();
        vkCmdWaitEvents(native, 1, &m_Event, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
        instance.RecordUpdate(*m_Commands, CBTInertClassify(), CBTIdentityFrameParams(), frameIndex);
        instance.RecordReadback(*m_Commands);
        m_Commands->End();
        m_Device.ExecuteCommandLists({m_Commands.get()});
        m_Token = m_Device.LastGraphicsSubmissionToken();
        return m_Token.IsValid();
    }

    IDevice::GpuSyncStatus Status() const { return m_Device.QueryGpuSyncToken(m_Token); }

    void ReleaseSoon()
    {
        m_ReleaseThread = std::jthread([this](std::stop_token stop) { ReleaseWhenDue(stop); });
    }

  private:
    VkDevice NativeDevice() const { return static_cast<VulkanDevice&>(m_Device).GetVkDevice(); }

    void Release() { EXPECT_EQ(vkSetEvent(NativeDevice(), m_Event), VK_SUCCESS); }

    void ReleaseWhenDue(std::stop_token stop)
    {
        // A liveness bound, not a frame-time or throughput assertion. The pending
        // token is checked before this timer starts and again at the transition.
        constexpr auto kHostReleaseDelay = std::chrono::milliseconds(100);
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        condition.wait_for(lock, stop, kHostReleaseDelay, [] { return false; });
        Release();
    }

    IDevice& m_Device;
    VkEvent m_Event = VK_NULL_HANDLE;
    std::unique_ptr<CommandList> m_Commands;
    IDevice::GpuSyncToken m_Token;
    std::jthread m_ReleaseThread;
};

std::vector<uint32_t> ReadIdentityIndices(IDevice& device, CBTInstance& instance,
                                        uint32_t firstWord, uint32_t wordCount)
{
    const size_t bytes = static_cast<size_t>(wordCount) * sizeof(uint32_t);
    const auto source = instance.GetResources().GetIdentityIndexBuffer();
    const auto readback = device.CreateReadbackBuffer(bytes, "CBT.TestIdentityReadback");
    EXPECT_TRUE(readback.IsValid());
    if (!readback.IsValid())
        return {};
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateBufferBarrier(
        source, ResourceState::IndexBuffer, ResourceState::CopySource));
    commands->CopyBuffer(source, readback, bytes,
                         static_cast<size_t>(firstWord) * sizeof(uint32_t), 0);
    commands->Barrier(ResourceBarrier::CreateBufferBarrier(
        source, ResourceState::CopySource, ResourceState::IndexBuffer));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    EXPECT_TRUE(device.WaitGpuSyncToken(device.LastGraphicsSubmissionToken()));
    std::vector<uint32_t> words(wordCount);
    const void* mapped = device.MapBuffer(readback);
    EXPECT_NE(mapped, nullptr);
    if (mapped)
    {
        std::memcpy(words.data(), mapped, bytes);
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return words;
}

uint32_t ReadSumTreeRoot(CBTInstance& instance)
{
    const uint32_t rootWord = CBTSumTreeHeapBase(kDefaultBisectorPoolSize);
    return instance.DebugReadWords(CBTBinding::SumTree, 1u, rootWord).front();
}

uint64_t ReadHeapId(const std::vector<uint32_t>& words, uint32_t slot)
{
    return static_cast<uint64_t>(words[slot * kHeapIdWords]) |
           (static_cast<uint64_t>(words[slot * kHeapIdWords + 1u]) << 32);
}
} // namespace

class CBTBringUpTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }

    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// ---------------------------------------------------------------------------
// 1. The instrument, before any reading taken with it
// ---------------------------------------------------------------------------

// POSITIVE CONTROL for every drain assertion below. IDevice::GetIdleDrainCount has a base-class
// default of 0, so a backend that does not track drains would satisfy an "unchanged" assertion
// vacuously. This test fails on such a backend, which is the point: it converts the zero deltas
// below from "nothing was observed" into "a drain would have been observed".
TEST_F(CBTBringUpTest, IdleDrainCounterMovesOnADrain)
{
    const uint64_t before = m_Device->GetIdleDrainCount();
    m_Device->WaitForIdle();
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before + 1u)
        << "the drain counter does not move, so every zero-delta assertion in this file is vacuous";
    m_Device->WaitForIdle();
    m_Device->WaitForIdle();
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before + 3u) << "the drain counter must count every call";
}

// ---------------------------------------------------------------------------
// 2. Drain-freedom of the declare-reachable entry points
// ---------------------------------------------------------------------------

// CBTRenderNode::DeclareForView -> CBTRenderFeature::EnsureInitialized -> InitializeRoots. This is
// the packed seed upload, partial clears and first reduce.
TEST_F(CBTBringUpTest, PlanarInitializeRootsPerformsNoDeviceDrain)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));

    const uint64_t before = m_Device->GetIdleDrainCount();
    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before)
        << "InitializeRoots drained the device; bring-up ordering must be barrier-expressed";
}

TEST_F(CBTBringUpTest, SphericalInitializeRootsPerformsNoDeviceDrain)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));

    const uint64_t before = m_Device->GetIdleDrainCount();
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before) << "spherical InitializeRoots drained the device";
}

// A live domain re-seed retires pending graphics users before replacing the sculpt
// descriptors, then orders the topology reset in its graphics submission.
TEST_F(CBTBringUpTest, LiveDomainReseedPerformsNoDeviceDrainAndStaysConforming)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));
    RunUpdate(*m_Device, instance, 0u);
    PendingUpdate pending(*m_Device);
    ASSERT_TRUE(pending.Submit(instance, 1u));
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    pending.ReleaseSoon();

    const uint64_t before = m_Device->GetIdleDrainCount();
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    EXPECT_EQ(pending.Status(), IDevice::GpuSyncStatus::Complete);
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before) << "the live domain re-seed drained the device";

    EXPECT_EQ(instance.GetRootCount(), kSphereRootCount);
    RunUpdate(*m_Device, instance, 2u);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u)
        << "the re-seed raced in-flight GPU writes: link reciprocity broke after the switch";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u)
        << "the re-seed raced in-flight GPU writes: bitfield/HeapID disagree after the switch";
}

// CBTRenderNode::DeclareForView -> CBTRenderFeature::ConsumeTerrainRetire -> RefreshTerrainSources.
// The rebind rewrites descriptor ring elements in-flight frames may still sample, so the protection
// must survive; only its SCOPE changes (graphics timeline, not a device drain).
TEST_F(CBTBringUpTest, RefreshTerrainSourcesPerformsNoDeviceDrain)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = "DrainOracleHeightmap";
    const TextureHandle retired = m_Device->CreateTexture(td);
    ASSERT_TRUE(retired.IsValid());
    for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
        instance.SetHeightSource(slot, retired);
    PendingUpdate pending(*m_Device);
    ASSERT_TRUE(pending.Submit(instance, 0u));
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    pending.ReleaseSoon();
    ASSERT_EQ(instance.CountTerrainSourceSlots(retired), kCBTFrameParamsRing);

    const uint64_t before = m_Device->GetIdleDrainCount();
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    EXPECT_TRUE(instance.RefreshTerrainSources());
    EXPECT_EQ(pending.Status(), IDevice::GpuSyncStatus::Complete)
        << "descriptor rebinding returned while its prior graphics user was pending";
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before)
        << "RefreshTerrainSources drained the device; wait the graphics timeline instead";

    // The protection itself, unchanged: no ring element may still name the retired texture.
    EXPECT_EQ(instance.CountTerrainSourceSlots(retired), 0u);
    instance.SetHeightSource(1u, retired);
    RunUpdate(*m_Device, instance, 1u);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u) << "post-refresh update";

    m_Device->DestroyTexture(retired);
}

TEST_F(CBTBringUpTest, PageTableGrowthWaitsForPendingGraphicsWithoutDeviceDrain)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    PendingUpdate pending(*m_Device);
    ASSERT_TRUE(pending.Submit(instance, 0u));
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    pending.ReleaseSoon();

    const uint64_t before = m_Device->GetIdleDrainCount();
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    ASSERT_TRUE(instance.GetResources().ProvisionPageTableWords(kCBTPageTableMinRingWords + 1u));
    EXPECT_EQ(pending.Status(), IDevice::GpuSyncStatus::Complete)
        << "page-table growth returned while its prior graphics user was pending";
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before)
        << "page-table growth drained the device; wait the graphics timeline instead";
}

// Far-field bulk free. No production caller today (FarFieldReseedDetector is unwired), but it is a
// declare-phase-shaped API — "call BETWEEN frames" — and the policy is about the code, not the
// current call graph.
TEST_F(CBTBringUpTest, RegionFreeToBasePerformsNoDeviceDrainAndStaysConforming)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    RunUpdate(*m_Device, instance, 0u);

    PendingUpdate pending(*m_Device);
    ASSERT_TRUE(pending.Submit(instance, 1u));
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    pending.ReleaseSoon();

    // One cube face's four slices — a valid interior region for the base-adjacency link kernel.
    constexpr uint32_t kOneFaceMask = 0xFu;
    const uint64_t before = m_Device->GetIdleDrainCount();
    ASSERT_EQ(pending.Status(), IDevice::GpuSyncStatus::Pending);
    instance.RegionFreeToBase(kOneFaceMask);
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before) << "RegionFreeToBase drained the device";

    RunUpdate(*m_Device, instance, 1u);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u)
        << "the region free raced in-flight GPU writes: link reciprocity broke";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u)
        << "the region free raced in-flight GPU writes: bitfield/HeapID disagree";
}

// The debug readbacks genuinely need a CPU-side wait — but on their OWN submission's timeline
// value, not a device-wide drain. Reachable at runtime through the debug server
// (TerrainDebugHandlers -> ReadTessellationStats).
TEST_F(CBTBringUpTest, DebugReadbacksWaitTheirOwnSubmissionNotTheDevice)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    RunUpdate(*m_Device, instance, 0u);

    uint64_t before = m_Device->GetIdleDrainCount();
    const auto words = instance.DebugReadWords(CBTBinding::HeapID, 4u);
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before) << "DebugReadWords drained the device";
    // The wait still has to work: slot 0 holds planar root HeapID 2.
    EXPECT_EQ(ReadHeapId(words, 0), 2u) << "the scoped wait did not order the readback copy";

    before = m_Device->GetIdleDrainCount();
    const CBTTessellationStats stats = instance.ReadTessellationStats();
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before) << "ReadTessellationStats drained the device";
    EXPECT_EQ(stats.LiveCount, kRootHalfedgeCount) << "the scoped wait did not order the bulk readback";

    before = m_Device->GetIdleDrainCount();
    (void)instance.ReadVertexEvalCount();
    EXPECT_EQ(m_Device->GetIdleDrainCount(), before) << "ReadWorkQueueCounter drained the device";
}

// ---------------------------------------------------------------------------
// 3. Seed state, observed before the first update
// ---------------------------------------------------------------------------

// Every value InitializeRoots writes, read back before any update runs. On the pre-slice code the
// three drains published these writes; afterwards a single submission's barriers do, and the
// reduce's sum tree must be visible to the very first Reset. Byte-level, both domains below.
TEST_F(CBTBringUpTest, PlanarSeedStateIsCompleteBeforeTheFirstUpdate)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));

    // HeapID: root i == 2^baseDepth + i, and slot 2 (past the roots) must be free (the zero-fill).
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, 3u * kHeapIdWords);
    EXPECT_EQ(ReadHeapId(heap, 0), (uint64_t(1) << kDefaultBaseDepth) + 0u);
    EXPECT_EQ(ReadHeapId(heap, 1), (uint64_t(1) << kDefaultBaseDepth) + 1u);
    EXPECT_EQ(ReadHeapId(heap, 2), kFreeSlotHeapID) << "the zero-fill did not reach slot 2";

    // NeighborsA: two triangles sharing their hypotenuse; the legs are domain boundaries.
    const auto nbr = instance.DebugReadWords(CBTBinding::NeighborsA, 2u * kNeighborWords);
    EXPECT_EQ(nbr[0], kInvalidPointer);
    EXPECT_EQ(nbr[1], kInvalidPointer);
    EXPECT_EQ(nbr[2], 1u) << "root 0's twin must be root 1";
    EXPECT_EQ(nbr[kNeighborWords + 0u], kInvalidPointer);
    EXPECT_EQ(nbr[kNeighborWords + 1u], kInvalidPointer);
    EXPECT_EQ(nbr[kNeighborWords + 2u], 0u) << "root 1's twin must be root 0";

    // BisectorData: every root visible.
    const auto bd = instance.DebugReadWords(CBTBinding::BisectorData, 2u * kBisectorDataWords);
    EXPECT_EQ(bd[kBisectorFlagsWord] & kFlagVisible, kFlagVisible);
    EXPECT_EQ(bd[kBisectorDataWords + kBisectorFlagsWord] & kFlagVisible, kFlagVisible);

    // Occupancy: bits 0..rootCount-1 of bitfield word 0, and nothing else.
    const auto bitfield = instance.DebugReadWords(CBTBinding::Bitfield, 2u);
    EXPECT_EQ(bitfield[0], (1u << kRootHalfedgeCount) - 1u);
    EXPECT_EQ(bitfield[1], 0u) << "the zero-fill did not reach bitfield word 1";

    // Compact live-index stream: the identity prefix the first Classify dispatches over.
    const auto indices = instance.DebugReadWords(CBTBinding::IndicesAll, 2u);
    EXPECT_EQ(indices[0], 0u);
    EXPECT_EQ(indices[1], 1u);

    // The reduce ran: the sum-tree root is the live count.
    EXPECT_EQ(ReadSumTreeRoot(instance), kRootHalfedgeCount)
        << "the seed reduce is not visible; the first frame's Reset would read a stale FreeCount";
}

TEST_F(CBTBringUpTest, SphericalSeedStateIsCompleteBeforeTheFirstUpdate)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    // All 24 cube-sphere roots at depth kSphereBaseDepth, matching the CPU root table exactly.
    const std::array<CBTSphereRoot, kSphereRootCount> expected = BuildSphereRoots();
    const auto heap =
        instance.DebugReadWords(CBTBinding::HeapID, (kSphereRootCount + 1u) * kHeapIdWords);
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
        EXPECT_EQ(ReadHeapId(heap, r), expected[r].HeapID) << "root " << r;
    EXPECT_EQ(ReadHeapId(heap, kSphereRootCount), kFreeSlotHeapID)
        << "the zero-fill did not reach the first non-root slot";

    const auto nbr = instance.DebugReadWords(CBTBinding::NeighborsA,
                                                   kSphereRootCount * kNeighborWords);
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        EXPECT_EQ(nbr[r * kNeighborWords + 0u], expected[r].Neighbors.Neighbor0) << "root " << r;
        EXPECT_EQ(nbr[r * kNeighborWords + 1u], expected[r].Neighbors.Neighbor1) << "root " << r;
        EXPECT_EQ(nbr[r * kNeighborWords + 2u], expected[r].Neighbors.Twin) << "root " << r;
    }

    const auto bitfield = instance.DebugReadWords(CBTBinding::Bitfield, 2u);
    EXPECT_EQ(bitfield[0], (1u << kSphereRootCount) - 1u);
    EXPECT_EQ(bitfield[1], 0u);

    const auto indices = instance.DebugReadWords(CBTBinding::IndicesAll, kSphereRootCount);
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
        EXPECT_EQ(indices[r], r) << "identity seed slot " << r;

    EXPECT_EQ(ReadSumTreeRoot(instance), kSphereRootCount);
}

// The draw-side resources InitializeRoots settles once. The identity index buffer is the largest
// bring-up upload (3 * poolSize indices) and the one whose staged copy has to land in the same
// submission as the barrier that transitions it to IndexBuffer.
TEST_F(CBTBringUpTest, IdentityIndexBufferIsSeededBeforeTheFirstUpdate)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    // Windowed reads at both ends and across the 64 KiB boundary, so a short, mis-offset or
    // truncated staged copy is caught — not only a zero-length one. Reading 12 MiB back would
    // dominate the suite, so the windows are small and the interesting offsets explicit.
    const uint32_t total = IdentityIndexCount(kDefaultBisectorPoolSize);

    const auto head = ReadIdentityIndices(*m_Device, instance, 0u, 4u);
    ASSERT_EQ(head.size(), 4u);
    for (uint32_t i = 0; i < 4u; ++i)
        EXPECT_EQ(head[i], i) << "identity index " << i;

    // 64 KiB is 16384 words: the chunk boundary any staged upload is most likely to stop at.
    const auto mid = ReadIdentityIndices(*m_Device, instance, 16382u, 4u);
    ASSERT_EQ(mid.size(), 4u);
    for (uint32_t i = 0; i < 4u; ++i)
        EXPECT_EQ(mid[i], 16382u + i) << "the staged identity copy breaks at the 64 KiB boundary";

    const auto tail = ReadIdentityIndices(*m_Device, instance, total - 4u, 4u);
    ASSERT_EQ(tail.size(), 4u);
    for (uint32_t i = 0; i < 4u; ++i)
        EXPECT_EQ(tail[i], total - 4u + i) << "the staged identity copy is short";
}
