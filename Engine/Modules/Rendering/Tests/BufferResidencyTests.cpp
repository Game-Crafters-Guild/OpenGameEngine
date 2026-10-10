// Where CPU-written, GPU-read buffers land in memory, and that asking for that
// residency never costs the mapping the engine depends on.
//
// Two layers:
//   * The policy itself is a pure function, so its table is asserted on every
//     platform without a GPU.
//   * The resolved memory type is a property of the running device, so those
//     assertions run against a real VkDevice and skip when none can be created.

#include "Source/Vulkan/VulkanBufferResidency.h"
#include "Source/Vulkan/VulkanDevice.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

namespace
{

constexpr uint32_t kStorage = static_cast<uint32_t>(BufferUsage::Storage);
constexpr uint32_t kUniform = static_cast<uint32_t>(BufferUsage::Uniform);
constexpr uint32_t kVertex = static_cast<uint32_t>(BufferUsage::Vertex);
constexpr uint32_t kIndex = static_cast<uint32_t>(BufferUsage::Index);
constexpr uint32_t kIndirect = static_cast<uint32_t>(BufferUsage::Indirect);
constexpr uint32_t kTransferSrc = static_cast<uint32_t>(BufferUsage::TransferSrc);
constexpr uint32_t kTransferDst = static_cast<uint32_t>(BufferUsage::TransferDst);
constexpr uint32_t kDeviceAddress = static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
constexpr uint32_t kAsBuildInput =
    static_cast<uint32_t>(BufferUsage::AccelerationStructureBuildInput);

constexpr bool kDiscrete = false;
constexpr bool kIntegrated = true;

constexpr uint64_t kMiB = 1024ull * 1024ull;

// (widest host-writable device-local window, total device-local bytes) — the
// gate's numerator and denominator in that order.
std::optional<DeviceMemoryTopology> MakeTopology(bool unified,
                                                 uint64_t apertureBytes,
                                                 uint64_t deviceLocalTotalBytes)
{
    DeviceMemoryTopology topology{};
    topology.isUnifiedMemory = unified;
    topology.largestHostVisibleDeviceLocalHeapBytes = apertureBytes;
    topology.deviceLocalHeapBytesTotal = deviceLocalTotalBytes;
    return topology;
}

// A backend that does not walk its heaps (D3D12 today). Must never be read as
// a device whose numbers happen to compare equal at zero.
const std::optional<DeviceMemoryTopology> kNoTopology = std::nullopt;
// Resizable BAR: the host-visible device-local type is backed by the VRAM heap
// itself, so the two numbers are the same heap's size. Matches the RTX 5080
// reading that motivated this gate (15977 MiB / 15977 MiB).
const std::optional<DeviceMemoryTopology> kResizableBar = MakeTopology(false, 15977 * kMiB, 15977 * kMiB);
// Classic BAR: a small separate device-local heap against a large VRAM heap.
const std::optional<DeviceMemoryTopology> kClassicBar = MakeTopology(false, 256 * kMiB, 8192 * kMiB);
// Unified: every byte is both device-local and host-visible, so the saturation
// comparison holds and only the unified flag refuses it.
const std::optional<DeviceMemoryTopology> kUnifiedTopology = MakeTopology(true, 8192 * kMiB, 8192 * kMiB);

// Every topology a device can report, for arms that must not read any of them.
const std::vector<std::optional<DeviceMemoryTopology>> kAllTopologies = {
    kNoTopology, kResizableBar, kClassicBar, kUnifiedTopology};

// The residency policy this branch replaces, transcribed verbatim from
// origin/main (VulkanBufferResidency.h: BufferIsGeometryStreamOnly plus the
// Upload arm that consumed it). Kept here so "the ineligible arm is
// byte-identical to today" is a computed equality rather than an argument about
// the shape of the gate.
BufferResidencyPolicy PolicyBeforeThisBranch(uint32_t bufferUsage, bool isIntegratedGpu)
{
    constexpr uint32_t kGeometry = kVertex | kIndex;
    constexpr uint32_t kGeometryCompanions =
        kGeometry | kTransferSrc | kTransferDst | kDeviceAddress | kAsBuildInput;
    const bool geometryStreamOnly =
        (bufferUsage & kGeometry) != 0 && (bufferUsage & ~kGeometryCompanions) == 0;

    const bool preferDeviceHeap =
        BufferHasDirectDeviceAccess(bufferUsage) && !isIntegratedGpu && !geometryStreamOnly;

    BufferResidencyPolicy policy{};
    policy.Usage = preferDeviceHeap ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    policy.Flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    return policy;
}

// Every usage mask a mesh pool is created with (MeshGPURegistry.cpp,
// CreateBucketStreamPool): vertex or index, plus the device-address /
// AS-build-input pair added when ray query is available.
const std::vector<uint32_t> kMeshPoolUsages = {kVertex,
                                               kIndex,
                                               kVertex | kDeviceAddress | kAsBuildInput,
                                               kIndex | kDeviceAddress | kAsBuildInput};

// Every topology on which the gate refuses. The aperture sizes are the three
// the 460-report survey actually observed (214 / 246 / 256 MiB) against small
// and large cards alike — no absolute size was ever safe, so all of them are
// pinned rather than one representative.
const std::vector<std::optional<DeviceMemoryTopology>> kIneligibleTopologies = {
    kNoTopology,
    kUnifiedTopology,
    MakeTopology(false, 0, 0),
    MakeTopology(false, 214 * kMiB, 8192 * kMiB),
    MakeTopology(false, 246 * kMiB, 8192 * kMiB),
    MakeTopology(false, 256 * kMiB, 8192 * kMiB),
    MakeTopology(false, 214 * kMiB, 2048 * kMiB),
    MakeTopology(false, 246 * kMiB, 2048 * kMiB),
    MakeTopology(false, 256 * kMiB, 1024 * kMiB),
    // The carve-out shape the max-denominator gate used to grant: a genuine
    // 256 MiB classic BAR on a card small enough that the unmappable remainder
    // no longer dominates. Refused now, on the same terms as every other
    // classic BAR.
    MakeTopology(false, 256 * kMiB, 512 * kMiB)};

void SetEnvHeadless()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
}

// Null when no Vulkan device can be created (no ICD, no adapter, CI without a
// GPU). Callers GTEST_SKIP rather than fail — the policy layer above still runs.
std::unique_ptr<IDevice> CreateHeadlessDevice()
{
    SetEnvHeadless();
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        return nullptr;
    return device;
}

} // namespace

// A buffer the GPU reads directly must not have DEVICE_LOCAL excluded from its
// candidate set: AUTO_PREFER_HOST marks DEVICE_LOCAL not-preferred, which is
// what kept every Upload buffer out of BAR VRAM.
TEST(BufferResidency, GpuReadUploadPrefersDeviceHeapOnDiscrete)
{
    // Looped over every topology on purpose: plain Upload must not read the
    // memory topology at all. Only UploadDeviceLocalPreferred does, and this is
    // the pin that keeps that arm from leaking into the general one.
    for (const auto& topology : kAllTopologies)
    {
        for (const uint32_t usage : {kStorage, kUniform, kStorage | kTransferDst, 0u})
        {
            const BufferResidencyPolicy policy =
                ResolveBufferResidencyPolicy(BufferMemoryUsage::Upload, usage, kDiscrete, topology);
            EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO) << "usage bits " << usage;
            EXPECT_EQ(policy.Flags, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
                << "usage bits " << usage;
        }
    }
}

// Staging exists to be copied out once. Preferring a device heap for it spends
// a scarce BAR window on bytes that are never re-read.
TEST(BufferResidency, TransferOnlyUploadStaysHostPreferred)
{
    for (const auto& topology : kAllTopologies)
    {
        for (const uint32_t usage : {kTransferSrc, kTransferDst, kTransferSrc | kTransferDst})
        {
            const BufferResidencyPolicy policy =
                ResolveBufferResidencyPolicy(BufferMemoryUsage::Upload, usage, kDiscrete, topology);
            EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST) << "usage bits " << usage;
        }
    }
}

// One physical pool: preferring DEVICE_LOCAL cannot win bandwidth there, and can
// only steer the allocation into a smaller carve-out where a driver exposes one.
TEST(BufferResidency, IntegratedGpuKeepsTheHostPreference)
{
    for (const auto& topology : kAllTopologies)
    {
        for (const uint32_t usage : {kStorage, kUniform, kVertex, 0u})
        {
            const BufferResidencyPolicy policy = ResolveBufferResidencyPolicy(
                BufferMemoryUsage::Upload, usage, kIntegrated, topology);
            EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST) << "usage bits " << usage;
        }
    }
}

// ALLOW_TRANSFER_INSTEAD demotes HOST_VISIBLE from required to preferred, so an
// Upload buffer could resolve to memory the engine cannot map. Every Upload
// caller here maps, so the flag must never appear — including on the
// device-preferring arm, where a caller might reasonably fear it does.
TEST(BufferResidency, UploadNeverAllowsTransferInstead)
{
    for (const BufferMemoryUsage memoryUsage :
         {BufferMemoryUsage::Upload, BufferMemoryUsage::UploadDeviceLocalPreferred})
    {
        for (const auto& topology : kAllTopologies)
        {
            for (const bool integrated : {kDiscrete, kIntegrated})
            {
                for (const uint32_t usage : {kStorage, kTransferSrc, kVertex, 0u})
                {
                    const BufferResidencyPolicy policy = ResolveBufferResidencyPolicy(
                        memoryUsage, usage, integrated, topology);
                    EXPECT_EQ(policy.Flags &
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
                              0u);
                }
            }
        }
    }
}

// The CPU reads these. HOST_ACCESS_RANDOM is what asks VMA for a cached type;
// SEQUENTIAL_WRITE would ask for the write-combined type instead.
TEST(BufferResidency, ReadbackAsksForRandomHostAccess)
{
    for (const bool integrated : {kDiscrete, kIntegrated})
    {
        const BufferResidencyPolicy policy = ResolveBufferResidencyPolicy(
            BufferMemoryUsage::Readback, kStorage | kTransferDst, integrated, kResizableBar);
        EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        EXPECT_EQ(policy.Flags, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    }
}

TEST(BufferResidency, DeviceLocalAndAutoPreferTheDeviceHeap)
{
    for (const BufferMemoryUsage memoryUsage :
         {BufferMemoryUsage::DeviceLocal, BufferMemoryUsage::Auto})
    {
        const BufferResidencyPolicy policy =
            ResolveBufferResidencyPolicy(memoryUsage, kStorage, kDiscrete, kResizableBar);
        EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        EXPECT_EQ(policy.Flags, 0u);
    }
}

TEST(BufferResidency, UsageWithNoBitsCountsAsDeviceAccess)
{
    // CreateBuffer turns a zero usage mask into storage + uniform, so the GPU
    // does read it.
    EXPECT_TRUE(BufferHasDirectDeviceAccess(0u));
    EXPECT_FALSE(BufferHasDirectDeviceAccess(kTransferSrc | kTransferDst));
    EXPECT_TRUE(BufferHasDirectDeviceAccess(kTransferSrc | kStorage));
}

// The gate, and the whole of it: no size constant, only the question of whether
// the CPU-writable device-local window IS the device-local heap.
TEST(BufferResidency, FullyHostWritableDeviceLocalPredicate)
{
    // Resizable BAR / SAM: the host-visible type is backed by the VRAM heap, so
    // the two numbers are the same heap's size.
    EXPECT_TRUE(DeviceLocalMemoryIsFullyHostWritable(kResizableBar));

    // A classic BAR is a small separate heap. This is the case the gate exists
    // for: the window is a scarce shared aperture, so mesh pools must not be
    // allowed to compete for it.
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(kClassicBar));

    // A backend that does not walk its heaps keeps today's behaviour. Reading a
    // disengaged optional as "0 >= 0" would classify every unimplemented backend
    // as fully host-writable, which is the failure the optional exists to make
    // impossible.
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(kNoTopology));

    // Unified memory satisfies the comparison and must still be refused: there
    // is no separate device-local heap to move bytes into.
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(kUnifiedTopology));

    // A device reporting no device-local heap at all is not a device whose
    // window covers it.
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(MakeTopology(false, 0, 0)));

    // Partial BAR resize (firmware exposing 512 MB / 1 GB / 2 GB steps) is
    // deliberately refused rather than cut at some fraction. Conservative:
    // forgoes a win, never risks the contention.
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(MakeTopology(false, 2048 * kMiB, 8192 * kMiB)));

    // Strictly greater is eligible too: a device exposing a host-visible
    // device-local heap larger than its largest single device-local heap has no
    // scarce aperture to protect.
    EXPECT_TRUE(DeviceLocalMemoryIsFullyHostWritable(MakeTopology(false, 16384 * kMiB, 8192 * kMiB)));
}

// The mesh-pool arm. Granted only where the window is the heap; everywhere else
// it resolves exactly as Upload did before this value existed, which is what
// keeps every other device byte-identical.
TEST(BufferResidency, DeviceLocalPreferredUploadIsGrantedOnlyWhenTheWindowIsTheHeap)
{
    // The mesh-pool usage masks CreateBucketStreamPool actually sets — vertex or
    // index, plus the device-address / AS-build-input pair a pool carries when
    // ray query is available — and one transfer-dst variant that no pool sets
    // today, so the arm stays pinned if a pool ever gains that bit.
    const std::vector<uint32_t> poolUsages = {kVertex,
                                              kIndex,
                                              kVertex | kDeviceAddress | kAsBuildInput,
                                              kIndex | kDeviceAddress | kAsBuildInput,
                                              kVertex | kTransferDst};

    for (const uint32_t usage : poolUsages)
    {
        EXPECT_EQ(ResolveBufferResidencyPolicy(BufferMemoryUsage::UploadDeviceLocalPreferred,
                                               usage, kDiscrete, kResizableBar).Usage,
                  VMA_MEMORY_USAGE_AUTO)
            << "usage bits " << usage;

        for (const auto& refused : {kClassicBar, kNoTopology, kUnifiedTopology})
        {
            EXPECT_EQ(ResolveBufferResidencyPolicy(BufferMemoryUsage::UploadDeviceLocalPreferred,
                                                   usage, kDiscrete, refused).Usage,
                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST)
                << "usage bits " << usage;
        }
    }
}

// The invariant the value's whole contract rests on: the preference changes
// where the bytes land and nothing else. Both arms must ask for the same host
// access, or a caller would need a fallback path for its mapping.
TEST(BufferResidency, DeviceLocalPreferredUploadNeverLosesHostAccess)
{
    for (const auto& topology : kAllTopologies)
    {
        const BufferResidencyPolicy policy = ResolveBufferResidencyPolicy(
            BufferMemoryUsage::UploadDeviceLocalPreferred, kVertex, kDiscrete, topology);
        EXPECT_EQ(policy.Flags, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        // Never AUTO_PREFER_DEVICE: that branch of VMA's resolver drops
        // HOST_VISIBLE from required to merely preferred.
        EXPECT_NE(policy.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    }
}

// Geometry bits no longer carry a policy of their own. Mesh pools declare their
// shape through the memory usage, so plain Upload with geometry-only bits now
// follows the same rule as every other GPU-read Upload buffer instead of a
// special case that inferred "this is a mesh pool" from a usage mask.
TEST(BufferResidency, GeometryBitsNoLongerGetTheirOwnUploadPolicy)
{
    for (const uint32_t usage : {kVertex,
                                 kIndex,
                                 kVertex | kTransferDst,
                                 kVertex | kDeviceAddress | kAsBuildInput,
                                 kIndex | kDeviceAddress | kAsBuildInput})
    {
        const BufferResidencyPolicy policy = ResolveBufferResidencyPolicy(
            BufferMemoryUsage::Upload, usage, kDiscrete, kClassicBar);
        EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO) << "usage bits " << usage;
        EXPECT_EQ(policy.Flags, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
            << "usage bits " << usage;
    }
}

// ===========================================================================
// The INELIGIBLE arm — classic BAR / ReBAR disabled in firmware / a backend
// that reports no topology. This is the arm nobody here can put on hardware, so
// it is pinned by equality against the policy this branch deletes rather than
// by inspection.
// ===========================================================================

// Byte-identical, computed: for every mesh-pool usage mask on every topology
// the gate refuses, the requested VMA usage AND flags must equal what
// origin/main produced for the same buffer.
TEST(BufferResidencyIneligibleArm, MeshPoolPolicyEqualsThePreBranchPolicyExactly)
{
    for (const auto& topology : kIneligibleTopologies)
    {
        for (const uint32_t usage : kMeshPoolUsages)
        {
            const BufferResidencyPolicy before = PolicyBeforeThisBranch(usage, kDiscrete);
            const BufferResidencyPolicy after = ResolveBufferResidencyPolicy(
                BufferMemoryUsage::UploadDeviceLocalPreferred, usage, kDiscrete, topology);

            EXPECT_EQ(after.Usage, before.Usage) << "usage bits " << usage;
            EXPECT_EQ(after.Flags, before.Flags) << "usage bits " << usage;
            // Stated absolutely as well as relatively, so a change to the
            // transcribed pre-branch policy cannot make this pass vacuously.
            EXPECT_EQ(after.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST) << "usage bits " << usage;
            EXPECT_EQ(after.Flags, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
                << "usage bits " << usage;
        }
    }
}

// The same equality on an integrated part, where the Upload arm's own
// integrated guard and the gate's wider unified guard both have to agree.
TEST(BufferResidencyIneligibleArm, IntegratedMeshPoolPolicyEqualsThePreBranchPolicy)
{
    for (const auto& topology : kIneligibleTopologies)
    {
        for (const uint32_t usage : kMeshPoolUsages)
        {
            EXPECT_EQ(ResolveBufferResidencyPolicy(BufferMemoryUsage::UploadDeviceLocalPreferred,
                                                   usage, kIntegrated, topology).Usage,
                      PolicyBeforeThisBranch(usage, kIntegrated).Usage)
                << "usage bits " << usage;
        }
    }
}

// The control that keeps the equality above from being vacuous: on an eligible
// device the two policies must DIFFER, or the pin would pass even if the branch
// did nothing.
TEST(BufferResidencyIneligibleArm, EligibleArmDiffersFromThePreBranchPolicy)
{
    for (const uint32_t usage : kMeshPoolUsages)
    {
        const BufferResidencyPolicy before = PolicyBeforeThisBranch(usage, kDiscrete);
        const BufferResidencyPolicy after = ResolveBufferResidencyPolicy(
            BufferMemoryUsage::UploadDeviceLocalPreferred, usage, kDiscrete, kResizableBar);

        EXPECT_EQ(before.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST) << "usage bits " << usage;
        EXPECT_EQ(after.Usage, VMA_MEMORY_USAGE_AUTO) << "usage bits " << usage;
        EXPECT_NE(after.Usage, before.Usage) << "usage bits " << usage;
    }
}

// The one place the ineligible arm is NOT byte-identical: plain Upload with
// geometry-only bits. Three callers in the tree have that shape and all three
// are test-only — Tests/Rendering/MaterialLitCubeTests.cpp (a cube VB and IB)
// and DeviceMemoryTopologyTests.cpp's own observation buffer — so shipped code
// is byte-identical and this is the pin that will notice when a shipped caller
// reappears.
TEST(BufferResidencyIneligibleArm, PlainGeometryOnlyUploadNoLongerMatchesThePreBranchPolicy)
{
    for (const uint32_t usage : kMeshPoolUsages)
    {
        const BufferResidencyPolicy before = PolicyBeforeThisBranch(usage, kDiscrete);
        const BufferResidencyPolicy after =
            ResolveBufferResidencyPolicy(BufferMemoryUsage::Upload, usage, kDiscrete, kClassicBar);

        EXPECT_EQ(before.Usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST) << "usage bits " << usage;
        EXPECT_EQ(after.Usage, VMA_MEMORY_USAGE_AUTO)
            << "a geometry-only Upload buffer would now spend the classic BAR window; usage bits "
            << usage;
    }
}

// A buffer the shaders read as a uniform or storage buffer gets the device
// heap. Covers the per-frame upload ring's own usage mask, which pairs Vertex
// with Uniform|Storage.
TEST(BufferResidency, ShaderReadableUploadStillPrefersTheDeviceHeap)
{
    for (const uint32_t usage : {kUniform,
                                 kStorage,
                                 kVertex | kStorage,
                                 kUniform | kStorage | kVertex | kTransferSrc,
                                 kIndex | kIndirect})
    {
        const BufferResidencyPolicy policy =
            ResolveBufferResidencyPolicy(BufferMemoryUsage::Upload, usage, kDiscrete, kResizableBar);
        EXPECT_EQ(policy.Usage, VMA_MEMORY_USAGE_AUTO) << "usage bits " << usage;
    }
}

// The whole change is only safe because HOST_VISIBLE stays a hard requirement.
// If a driver or a VMA upgrade ever breaks that, this fails before anything
// dereferences a null mapping.
TEST(BufferResidency, MappedUploadBufferIsStillMappable)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(device.get());

    BufferDesc desc{};
    desc.size = 64 * 1024;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "ResidencyTest_UploadMapped";
    const BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const VkMemoryPropertyFlags resolved = vk->GetBufferResolvedMemoryProperties(buffer);
    EXPECT_NE(resolved & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0u)
        << "Upload resolved to a non-host-visible type; every mapped write would fault";

    void* mapped = device->MapBuffer(buffer);
    ASSERT_NE(mapped, nullptr);
    const std::vector<uint8_t> pattern(desc.size, 0xA5);
    std::memcpy(mapped, pattern.data(), pattern.size());
    device->UnmapBuffer(buffer);

    device->DestroyBuffer(buffer);
}

// The mesh-pool shape on the real device: it must stay MAPPABLE. That is the
// whole precondition — both arms of the gate keep HOST_VISIBLE required, and a
// pool that lost its mapping would fault on the next write.
//
// Coherence is deliberately NOT asserted. Every byte written into pool memory
// goes through MeshGPURegistry::UploadMesh -> IDevice::UpdateBuffer, whose
// persistent-map branch calls vmaFlushAllocation after the memcpy;
// VmaAllocator_T::GetFlushOrInvalidateRange gates the flush on
// IsMemoryTypeNonCoherent, so vkFlushMappedMemoryRanges is issued exactly when
// the resolved type needs it and skipped otherwise. A non-coherent
// DEVICE_LOCAL|HOST_VISIBLE type is therefore correct on this write path, and
// requiring coherence here would reject hardware the engine handles. VMA does
// not ask for it either: the AUTO / AUTO_PREFER_HOST + SEQUENTIAL_WRITE branch
// of FindMemoryPreferences sets HOST_VISIBLE required and HOST_CACHED
// not-preferred, and names HOST_COHERENT nowhere.
TEST(BufferResidency, MeshPoolShapedUploadStaysMappable)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(device.get());

    BufferDesc desc{};
    desc.size = 4 * 1024 * 1024;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    desc.memoryUsage = BufferMemoryUsage::UploadDeviceLocalPreferred;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "ResidencyTest_MeshPoolShaped";
    const BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const VkMemoryPropertyFlags resolved = vk->GetBufferResolvedMemoryProperties(buffer);
    const IDevice::BufferMemoryResidency residency = device->GetBufferMemoryResidency(buffer);
    const auto& topology = device->GetCapabilities().memoryTopology;

    // Recorded, not asserted: whether this lands device-local is a property of
    // the machine the test runs on. The gate's decision is asserted against the
    // topology below, which is the part that must agree everywhere.
    std::cout << "[ residency ] mesh-pool-shaped Upload resolved flags=0x" << std::hex << resolved
              << std::dec << " deviceLocal=" << residency.deviceLocal
              << " hostVisible=" << residency.hostVisible
              << " hostCoherent=" << residency.hostCoherent
              << " heapIndex=" << residency.heapIndex
              << " eligible=" << DeviceLocalMemoryIsFullyHostWritable(topology) << std::endl;

    EXPECT_NE(resolved & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0u)
        << "mesh pool resolved to non-host-visible memory; every mapped write would fault";

    // The gate is an observation, so it is checked against one: on an eligible
    // device the pool must actually be device-local, not merely asked to be.
    if (residency.reported && DeviceLocalMemoryIsFullyHostWritable(topology))
    {
        EXPECT_TRUE(residency.deviceLocal)
            << "device reports a fully host-writable device-local heap, but the pool did not land in it";
    }

    void* mapped = device->MapBuffer(buffer);
    ASSERT_NE(mapped, nullptr);
    const std::vector<uint8_t> pattern(desc.size, 0x5A);
    std::memcpy(mapped, pattern.data(), pattern.size());
    device->UnmapBuffer(buffer);

    device->DestroyBuffer(buffer);
}

// Transfer-only staging must not be pulled into a device heap by the change
// above — its bytes are copied out once.
TEST(BufferResidency, StagingUploadStaysHostVisible)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(device.get());

    BufferDesc desc{};
    desc.size = 4096;
    desc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "ResidencyTest_Staging";
    const BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const VkMemoryPropertyFlags resolved = vk->GetBufferResolvedMemoryProperties(buffer);
    EXPECT_NE(resolved & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0u);

    device->DestroyBuffer(buffer);
}

// Readback buffers are read by the CPU, so they must land in a cached type
// wherever the device exposes one. Skipped on devices that expose none
// (HOST_CACHED is not universal — VMA's own comment cites Raspberry Pi).
TEST(BufferResidency, ReadbackResolvesToAHostCachedTypeWhenOneExists)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(device.get());

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(vk->GetVkPhysicalDevice(), &memProps);
    bool deviceHasCachedType = false;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        const VkMemoryPropertyFlags flags = memProps.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0)
        {
            deviceHasCachedType = true;
            break;
        }
    }
    if (!deviceHasCachedType)
        GTEST_SKIP() << "Device exposes no host-cached memory type";

    BufferDesc desc{};
    desc.size = 256 * 1024;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                 static_cast<uint32_t>(BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Readback;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "ResidencyTest_Readback";
    const BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const VkMemoryPropertyFlags resolved = vk->GetBufferResolvedMemoryProperties(buffer);
    // Coherence is reported, not asserted: a host-cached type need not be
    // coherent, and MapBuffer invalidates before every read. It is recorded
    // because UnmapBuffer does NOT flush, so a CPU *write* into a Readback
    // buffer relies on the resolved type being coherent.
    std::cout << "[ residency ] Readback resolved flags=0x" << std::hex << resolved << std::dec
              << " hostCached=" << ((resolved & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0)
              << " hostCoherent=" << ((resolved & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
              << std::endl;
    EXPECT_NE(resolved & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0u);
    EXPECT_NE(resolved & VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0u)
        << "Readback landed in uncached memory; the CPU-side copy reads write-combined bytes";

    device->DestroyBuffer(buffer);
}

// Reports the resolved type for a GPU-read Upload buffer. On a discrete part
// with a usable BAR the DEVICE_LOCAL bit is the point of the change; a part
// without one legitimately reports system RAM, so this records rather than
// asserts residency and only enforces the mappability invariant.
TEST(BufferResidency, ReportsResolvedUploadResidency)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(device.get());

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vk->GetVkPhysicalDevice(), &props);

    BufferDesc desc{};
    desc.size = 1024 * 1024;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "ResidencyTest_UploadResidency";
    const BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const VkMemoryPropertyFlags resolved = vk->GetBufferResolvedMemoryProperties(buffer);
    std::cout << "[ residency ] " << props.deviceName << " deviceType=" << props.deviceType
              << " GPU-read Upload resolved flags=0x" << std::hex << resolved << std::dec
              << " deviceLocal=" << ((resolved & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
              << " hostVisible=" << ((resolved & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
              << " hostCached=" << ((resolved & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0)
              << std::endl;

    EXPECT_NE(resolved & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0u);

    device->DestroyBuffer(buffer);
}
