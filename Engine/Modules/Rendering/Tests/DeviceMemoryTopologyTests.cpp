// What the backend reports about a device's memory arrangement.
//
// Two layers, mirroring BufferResidencyTests:
//   * The derivation is a pure function over VkPhysicalDeviceMemoryProperties,
//     so its table is asserted on every platform without a GPU.
//   * Whether a real device populates the capability at all is a property of
//     the running device, so that assertion runs against a real VkDevice and
//     skips when none can be created.

#include "Source/Vulkan/VulkanBufferResidency.h"
#include "Source/Vulkan/VulkanMemoryTopology.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

namespace
{

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;

constexpr VkMemoryPropertyFlags kDeviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr VkMemoryPropertyFlags kHostCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags kHostCached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

// Builder for synthetic memory properties. Heaps are added first; each memory
// type names the heap it belongs to by index.
class MemoryPropertiesBuilder
{
  public:
    MemoryPropertiesBuilder& Heap(uint64_t sizeBytes, VkMemoryHeapFlags flags)
    {
        m_Properties.memoryHeaps[m_Properties.memoryHeapCount].size = sizeBytes;
        m_Properties.memoryHeaps[m_Properties.memoryHeapCount].flags = flags;
        ++m_Properties.memoryHeapCount;
        return *this;
    }

    MemoryPropertiesBuilder& Type(uint32_t heapIndex, VkMemoryPropertyFlags flags)
    {
        m_Properties.memoryTypes[m_Properties.memoryTypeCount].heapIndex = heapIndex;
        m_Properties.memoryTypes[m_Properties.memoryTypeCount].propertyFlags = flags;
        ++m_Properties.memoryTypeCount;
        return *this;
    }

    const VkPhysicalDeviceMemoryProperties& Build() const { return m_Properties; }

  private:
    VkPhysicalDeviceMemoryProperties m_Properties{};
};

// A discrete GPU without resizable BAR: a 256 MiB CPU-writable window carved
// out of the VRAM heap, exposed as its own heap by the driver.
MemoryPropertiesBuilder ClassicBarLayout()
{
    MemoryPropertiesBuilder builder;
    builder.Heap(8 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)  // 0: VRAM
        .Heap(32 * kGiB, 0)                                  // 1: system RAM
        .Heap(256 * kMiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);  // 2: BAR window
    builder.Type(0, kDeviceLocal)
        .Type(1, kHostVisible | kHostCoherent)
        .Type(1, kHostVisible | kHostCoherent | kHostCached)
        .Type(2, kDeviceLocal | kHostVisible | kHostCoherent);
    return builder;
}

// ---------------------------------------------------------------------------
// Real driver heap layouts, transcribed from driver source rather than guessed.
//
// The two open drivers CARVE the CPU-visible aperture OUT of the VRAM total and
// report the remainder as a second device-local heap; NVIDIA's reports show the
// aperture OVERLAPPING a full-size VRAM heap instead. The gate must land on the
// same answer for the same hardware under either declaration, which is why its
// denominator is the device-local TOTAL and not the largest device-local heap.
// ---------------------------------------------------------------------------

// Mesa RADV, radv_physical_device_init_mem_types (src/amd/vulkan/radv_physical_device.c):
//   visible = MIN2(vram_total, vram_vis)
//   vram    = vram_total - MIN2(vram_total, vram_vis)      <- carve-out
//   the invisible VRAM heap is emitted only if vram > 0 && vram * 9 >= visible
//   DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT types point at the VRAM_VIS heap.
MemoryPropertiesBuilder RadvCarveOut(uint64_t vramTotal, uint64_t visible, uint64_t gtt = 32 * kGiB)
{
    const uint64_t invisible = vramTotal - std::min(vramTotal, visible);
    visible = std::min(vramTotal, visible);

    MemoryPropertiesBuilder builder;
    int vramIndex = -1;
    int gttIndex = -1;
    int visIndex = -1;
    uint32_t next = 0;

    if (invisible > 0 && invisible * 9 >= visible)
    {
        builder.Heap(invisible, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        vramIndex = static_cast<int>(next++);
    }
    if (gtt > 0)
    {
        builder.Heap(gtt, 0);
        gttIndex = static_cast<int>(next++);
    }
    if (visible > 0)
    {
        builder.Heap(visible, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        visIndex = static_cast<int>(next++);
    }

    if (vramIndex >= 0 || visIndex >= 0)
    {
        const uint32_t deviceOnly = static_cast<uint32_t>(vramIndex >= 0 ? vramIndex : visIndex);
        builder.Type(deviceOnly, kDeviceLocal).Type(deviceOnly, kDeviceLocal);
    }
    if (gttIndex >= 0)
        builder.Type(static_cast<uint32_t>(gttIndex), kHostVisible | kHostCoherent);
    if (visIndex >= 0)
    {
        const uint32_t vis = static_cast<uint32_t>(visIndex);
        builder.Type(vis, kDeviceLocal | kHostVisible | kHostCoherent)
            .Type(vis, kDeviceLocal | kHostVisible | kHostCoherent);
    }
    if (gttIndex >= 0)
        builder.Type(static_cast<uint32_t>(gttIndex), kHostVisible | kHostCoherent | kHostCached);
    return builder;
}

// Intel ANV, anv_physical_device_init_heaps + anv_i915_physical_device_init_memory_types:
//   heap 0 = vram_non_mappable.size (or vram_mappable when nothing is unmappable)
//   heap 1 = system RAM, heap 2 = vram_mappable (only when non-mappable > 0)
//   the DEVICE_LOCAL|HOST_VISIBLE type points at heap 2 when a small BAR exists.
MemoryPropertiesBuilder AnvCarveOut(uint64_t vramTotal, uint64_t mappable, uint64_t sys = 32 * kGiB)
{
    mappable = std::min(vramTotal, mappable);
    const uint64_t nonMappable = vramTotal - mappable;

    MemoryPropertiesBuilder builder;
    builder.Heap(nonMappable != 0 ? nonMappable : mappable, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        .Heap(sys, 0);
    if (nonMappable > 0)
        builder.Heap(mappable, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);

    builder.Type(0, kDeviceLocal)
        .Type(1, kHostVisible | kHostCoherent | kHostCached)
        .Type(nonMappable > 0 ? 2u : 0u, kDeviceLocal | kHostVisible | kHostCoherent);
    return builder;
}

// NVIDIA's reports (vulkan.gpuinfo.org, quoted in the design doc: RTX 3080 shows
// 10053.0 + 214.0 MiB where 10053 is already the full card) expose the aperture
// as an ADDITIONAL device-local heap alongside a full-size VRAM heap, so the two
// device-local heaps overlap rather than partition.
MemoryPropertiesBuilder NvidiaOverlapBar(uint64_t vramTotal, uint64_t aperture, uint64_t sys = 32 * kGiB)
{
    MemoryPropertiesBuilder builder;
    builder.Heap(vramTotal, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT).Heap(sys, 0);
    builder.Type(0, kDeviceLocal).Type(1, kHostVisible | kHostCoherent).Type(
        1, kHostVisible | kHostCoherent | kHostCached);
    if (aperture < vramTotal)
    {
        builder.Heap(aperture, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        builder.Type(2, kDeviceLocal | kHostVisible | kHostCoherent);
    }
    else
    {
        builder.Type(0, kDeviceLocal | kHostVisible | kHostCoherent);
    }
    return builder;
}

bool GateFor(const MemoryPropertiesBuilder& builder, VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
{
    const std::optional<DeviceMemoryTopology> topology = DeriveMemoryTopology(builder.Build(), type);
    return DeviceLocalMemoryIsFullyHostWritable(topology);
}

void SetEnvHeadless()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
}

// Null when no Vulkan device can be created (no ICD, no adapter, CI without a
// GPU). Callers GTEST_SKIP rather than fail — the pure layer above still runs.
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

TEST(DeviceMemoryTopology, DeviceLocalTotalIgnoresNonDeviceLocalHeaps)
{
    // System RAM is the biggest heap present and must not be reported as the
    // device-local total — that number is a VRAM budget's denominator. Both
    // device-local heaps count, including the BAR window: the total is the
    // card, and the driver's choice to declare it in two pieces is not a fact
    // about the hardware.
    const auto props = ClassicBarLayout().Build();
    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.deviceLocalHeapBytesTotal, 8 * kGiB + 256 * kMiB);
}

TEST(DeviceMemoryTopology, ClassicBarReportsTheSmallWindow)
{
    const auto props = ClassicBarLayout().Build();
    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, 256 * kMiB);
    EXPECT_FALSE(topology.isUnifiedMemory);
}

TEST(DeviceMemoryTopology, ResizableBarWindowMatchesTheDeviceHeap)
{
    // With ReBAR the whole VRAM heap is host-visible, and there is no separate
    // window heap. The two numbers coinciding is the signal the gate reads.
    MemoryPropertiesBuilder builder;
    builder.Heap(16 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT).Heap(32 * kGiB, 0);
    builder.Type(0, kDeviceLocal)
        .Type(0, kDeviceLocal | kHostVisible | kHostCoherent)
        .Type(1, kHostVisible | kHostCoherent);
    const auto props = builder.Build();

    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.deviceLocalHeapBytesTotal, 16 * kGiB);
    EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, 16 * kGiB);
}

TEST(DeviceMemoryTopology, HeapSharedBySeveralTypesIsNotMultiplied)
{
    // The failure this pins: accumulating per matching TYPE rather than per
    // distinct HEAP. Three qualifying types share heap 0, so a per-type sum
    // reports three times the real window and makes a classic BAR look like
    // ReBAR.
    MemoryPropertiesBuilder builder;
    builder.Heap(4 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT).Heap(16 * kGiB, 0);
    builder.Type(0, kDeviceLocal | kHostVisible)
        .Type(0, kDeviceLocal | kHostVisible | kHostCoherent)
        .Type(0, kDeviceLocal | kHostVisible | kHostCoherent | kHostCached)
        .Type(1, kHostVisible | kHostCoherent);
    const auto props = builder.Build();

    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, 4 * kGiB);
}

TEST(DeviceMemoryTopology, SeparateHostVisibleHeapsAreNotAddedTogether)
{
    // The window is what ONE allocation can land in, so two 2 GiB host-visible
    // device-local heaps are a 2 GiB window, not a 4 GiB one. Summing them here
    // would report saturation on a card whose largest usable window is half its
    // VRAM.
    MemoryPropertiesBuilder builder;
    builder.Heap(2 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        .Heap(2 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        .Heap(16 * kGiB, 0);
    builder.Type(0, kDeviceLocal | kHostVisible | kHostCoherent)
        .Type(1, kDeviceLocal | kHostVisible | kHostCoherent)
        .Type(2, kHostVisible | kHostCoherent);
    const auto props = builder.Build();

    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, 2 * kGiB);
    EXPECT_EQ(topology.deviceLocalHeapBytesTotal, 4 * kGiB);
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(topology));
}

TEST(DeviceMemoryTopology, DeviceLocalWithoutHostVisibleIsNotAWindow)
{
    // A device-local heap no memory type exposes to the CPU contributes
    // nothing: a mapped write cannot reach it.
    MemoryPropertiesBuilder builder;
    builder.Heap(8 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT).Heap(16 * kGiB, 0);
    builder.Type(0, kDeviceLocal).Type(1, kHostVisible | kHostCoherent);
    const auto props = builder.Build();

    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.deviceLocalHeapBytesTotal, 8 * kGiB);
    EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, 0u);
}

TEST(DeviceMemoryTopology, UnifiedCoversMoreThanIntegratedGpus)
{
    // Wider than the integrated-GPU test the buffer residency policy uses:
    // reporting only INTEGRATED_GPU here would let a CPU or paravirtualized
    // device be treated as if staging a copy bought it something.
    const auto props = ClassicBarLayout().Build();

    EXPECT_TRUE(DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU).isUnifiedMemory);
    EXPECT_TRUE(DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_CPU).isUnifiedMemory);
    EXPECT_TRUE(DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU).isUnifiedMemory);
    EXPECT_FALSE(DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU).isUnifiedMemory);
    EXPECT_FALSE(DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_OTHER).isUnifiedMemory);
}

TEST(DeviceMemoryTopology, EmptyPropertiesReportZeroRatherThanReadingOutOfRange)
{
    const VkPhysicalDeviceMemoryProperties props{};
    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.deviceLocalHeapBytesTotal, 0u);
    EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, 0u);
}

// ===========================================================================
// The gate against the heap declarations real drivers produce.
//
// The same physical hardware must get the same verdict whether its driver
// carves the aperture out of the VRAM total (RADV, ANV) or declares it as an
// extra overlapping heap (NVIDIA). Comparing the aperture against the LARGEST
// device-local heap does not have that property: on a carve-out declaration it
// silently reduces to "aperture >= 50% of VRAM".
// ===========================================================================

// The shape a ReBAR-capable machine produces once Above-4G Decoding / Resizable
// BAR is turned off in firmware, on the vendor whose driver overlaps its heaps.
// This is what this project's own hardware will report after that BIOS flip.
TEST(DeviceMemoryTopologyIneligibleArm, NvidiaOverlappedApertureIsRefusedAtEveryObservedSize)
{
    // 214 / 246 / 256 MiB are the three aperture sizes the 460-report survey
    // observed, against small and large cards alike. No absolute size is safe,
    // so all nine combinations must refuse.
    for (const uint64_t aperture : {214 * kMiB, 246 * kMiB, 256 * kMiB})
    {
        for (const uint64_t vram : {1024 * kMiB, 2048 * kMiB, 16376 * kMiB})
        {
            const auto builder = NvidiaOverlapBar(vram, aperture);
            const DeviceMemoryTopology topology =
                DeriveMemoryTopology(builder.Build(), VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

            // The overlapping aperture is counted into the total as declared.
            // Double-counting it can only make the gate refuse earlier, which is
            // the safe direction and needs no per-vendor special case.
            EXPECT_EQ(topology.deviceLocalHeapBytesTotal, vram + aperture)
                << "aperture " << aperture << " vram " << vram;
            EXPECT_EQ(topology.largestHostVisibleDeviceLocalHeapBytes, aperture)
                << "aperture " << aperture << " vram " << vram;
            EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(topology))
                << "aperture " << aperture << " vram " << vram;
        }
    }
}

// Same machine with the BIOS option back on: one device-local heap, the
// host-visible type on it, gate eligible. The control that proves the pin above
// could have gone the other way.
TEST(DeviceMemoryTopologyIneligibleArm, NvidiaFullyResizedBarIsEligible)
{
    EXPECT_TRUE(GateFor(NvidiaOverlapBar(16376 * kMiB, 16376 * kMiB)));
}

// A backend that never walks its heaps (D3D12 today) must land on the
// ineligible arm through the disengaged optional, not through a zero compare.
TEST(DeviceMemoryTopologyIneligibleArm, DisengagedTopologyIsRefused)
{
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(std::nullopt));

    // And a device that reports a topology of all zeroes is refused by the
    // explicit deviceLocalHeapBytesTotal == 0 arm, not by the comparison.
    DeviceMemoryTopology zeroed{};
    EXPECT_FALSE(DeviceLocalMemoryIsFullyHostWritable(zeroed));
}

// A partially-resized BAR leaves device-local memory the window cannot reach,
// so it is refused — on the carve-out declaration as well as the overlapping
// one. Against the largest device-local heap this held only on overlap drivers;
// on a carve-out driver every step at or above half the card was granted.
TEST(DeviceMemoryTopologyCarveOut, PartiallyResizedBarIsRefusedOnEveryDeclaration)
{
    for (const uint64_t aperture : {512 * kMiB, 1024 * kMiB, 2048 * kMiB, 4096 * kMiB})
        EXPECT_FALSE(GateFor(NvidiaOverlapBar(8192 * kMiB, aperture)))
            << "overlap driver, aperture " << aperture;

    EXPECT_FALSE(GateFor(RadvCarveOut(8192 * kMiB, 512 * kMiB)));
    EXPECT_FALSE(GateFor(RadvCarveOut(8192 * kMiB, 2048 * kMiB)));

    // The three that the largest-heap denominator granted: half or more of the
    // card is host-writable, but the rest of it is not.
    EXPECT_FALSE(GateFor(RadvCarveOut(8192 * kMiB, 4096 * kMiB)));
    EXPECT_FALSE(GateFor(RadvCarveOut(12288 * kMiB, 8192 * kMiB)));
    EXPECT_FALSE(GateFor(AnvCarveOut(8192 * kMiB, 4096 * kMiB)));
}

// The sub-population the gate exists to refuse: a genuinely scarce ~256 MiB
// aperture that per-frame rings also grow into. Refusal must not depend on the
// card being large enough for the unmappable remainder to dominate — the
// 512 MiB case is a classic BAR and was granted while it did not.
TEST(DeviceMemoryTopologyCarveOut, ScarceApertureIsRefusedAtEveryCardSize)
{
    EXPECT_FALSE(GateFor(RadvCarveOut(2048 * kMiB, 256 * kMiB)));
    EXPECT_FALSE(GateFor(RadvCarveOut(1024 * kMiB, 256 * kMiB)));
    EXPECT_FALSE(GateFor(RadvCarveOut(512 * kMiB, 256 * kMiB)));
    EXPECT_FALSE(GateFor(AnvCarveOut(512 * kMiB, 256 * kMiB)));
}

// A carve-out driver with the aperture over the whole card declares ONE
// device-local heap and is eligible, matching the overlap driver's answer for
// the same hardware. This is the pin that the fix did not simply make the gate
// refuse everything.
TEST(DeviceMemoryTopologyCarveOut, FullyMappableCardIsEligibleOnCarveOutDriversToo)
{
    EXPECT_TRUE(GateFor(RadvCarveOut(8192 * kMiB, 8192 * kMiB)));
    EXPECT_TRUE(GateFor(AnvCarveOut(8192 * kMiB, 8192 * kMiB)));
}

// RADV drops the invisible heap entirely when the remainder is under a ninth of
// the aperture, so a device can DECLARE less device-local memory than it
// physically has. The gate is over the declaration, and that is the strongest
// available scope: the suppressed bytes are not allocatable through any Vulkan
// path either, so no allocation the engine could make would land in them.
TEST(DeviceMemoryTopologyCarveOut, GateIsScopedToTheDeclaredHeaps)
{
    const auto builder = RadvCarveOut(8192 * kMiB, 7680 * kMiB); // 512 MiB invisible
    const DeviceMemoryTopology topology =
        DeriveMemoryTopology(builder.Build(), VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

    EXPECT_EQ(topology.deviceLocalHeapBytesTotal, 7680 * kMiB)
        << "the invisible 512 MiB is absent from the heap declaration entirely";
    EXPECT_TRUE(DeviceLocalMemoryIsFullyHostWritable(topology));
}

// TYPE_OTHER is not unified, so it reaches the heap comparison rather than
// short-circuiting — the gate outcome, not just the unified flag, is pinned.
TEST(DeviceMemoryTopologyCarveOut, UnknownDeviceTypeStillAnswersTheGate)
{
    EXPECT_FALSE(GateFor(ClassicBarLayout(), VK_PHYSICAL_DEVICE_TYPE_OTHER));
    EXPECT_TRUE(GateFor(NvidiaOverlapBar(8192 * kMiB, 8192 * kMiB), VK_PHYSICAL_DEVICE_TYPE_OTHER));
}

TEST(DeviceMemoryTopology, CapabilityIsUnreportedUntilABackendPopulatesIt)
{
    // The distinction the optional exists to preserve: a default-constructed
    // capability set says "nobody answered", never "this device has no
    // device-local memory". A backend that cannot walk its heaps must leave it
    // this way rather than filling in zeroes.
    const RenderingDeviceCapabilities caps{};

    EXPECT_FALSE(caps.memoryTopology.has_value());
}

TEST(DeviceMemoryTopology, RunningDeviceReportsItsTopology)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const RenderingDeviceCapabilities& caps = device->GetCapabilities();
    ASSERT_TRUE(caps.memoryTopology.has_value())
        << "the Vulkan backend must report memory topology";

    // Any Vulkan device has at least one device-local heap, so a zero here is
    // the backend failing to look rather than a device without VRAM.
    EXPECT_GT(caps.memoryTopology->deviceLocalHeapBytesTotal, 0u);
    // A host-visible device-local heap is itself device-local, so the widest
    // window can never exceed the total. A violation means the two numbers were
    // derived from different heap sets.
    EXPECT_LE(caps.memoryTopology->largestHostVisibleDeviceLocalHeapBytes,
              caps.memoryTopology->deviceLocalHeapBytesTotal);

    // The acceptance instrument for a hardware run: whatever this device is, the
    // gate must state a verdict and a reason for it, and the reason must be the
    // one that matches the verdict.
    const char* reason = DescribeResidencyGateDecision(caps.memoryTopology);
    ASSERT_NE(reason, nullptr);
    EXPECT_STRNE(reason, "");
    std::cout << "[ gate ] " << (DeviceLocalMemoryIsFullyHostWritable(caps.memoryTopology)
                                     ? "ELIGIBLE"
                                     : "NOT ELIGIBLE")
              << " - " << reason << std::endl;
}

// The gate-decision line is the acceptance instrument for the ineligible arm on
// real hardware (ReBAR turned off in firmware), so its wording is pinned rather
// than left to drift. Every branch must produce a distinct, non-empty reason,
// and the two that a reader could confuse must not be interchangeable.
TEST(DeviceMemoryTopology, GateDecisionReasonNamesTheBranchItCameFrom)
{
    const auto reasonFor = [](const std::optional<DeviceMemoryTopology>& topology)
    { return std::string(DescribeResidencyGateDecision(topology)); };

    EXPECT_EQ(reasonFor(std::nullopt), "this backend does not report memory topology");

    DeviceMemoryTopology unified{};
    unified.isUnifiedMemory = true;
    unified.deviceLocalHeapBytesTotal = 8 * kGiB;
    unified.largestHostVisibleDeviceLocalHeapBytes = 8 * kGiB;
    EXPECT_EQ(reasonFor(unified), "unified memory - no separate device-local heap to prefer");

    // A device that declares no device-local memory is refused for that reason,
    // not described as having a "smaller, separate aperture" it does not have.
    EXPECT_EQ(reasonFor(DeviceMemoryTopology{}), "the device declares no device-local memory");

    DeviceMemoryTopology eligible{};
    eligible.deviceLocalHeapBytesTotal = 16 * kGiB;
    eligible.largestHostVisibleDeviceLocalHeapBytes = 16 * kGiB;
    ASSERT_TRUE(DeviceLocalMemoryIsFullyHostWritable(eligible));
    EXPECT_EQ(reasonFor(eligible),
              "the host-visible device-local window covers all declared device-local memory");

    DeviceMemoryTopology classicBar{};
    classicBar.deviceLocalHeapBytesTotal = 8 * kGiB;
    classicBar.largestHostVisibleDeviceLocalHeapBytes = 256 * kMiB;
    ASSERT_FALSE(DeviceLocalMemoryIsFullyHostWritable(classicBar));
    EXPECT_EQ(reasonFor(classicBar),
              "the host-visible device-local window is a smaller, separate aperture");
}

TEST(DeviceMemoryTopology, RunningDeviceReportsWhereABufferLanded)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    BufferDesc desc{};
    desc.size = 64 * 1024;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "DeviceMemoryTopologyTests.Observed";
    BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const IDevice::BufferMemoryResidency residency = device->GetBufferMemoryResidency(buffer);
    EXPECT_TRUE(residency.reported);
    // An Upload buffer is mappable in every branch of the residency policy, so
    // this is the one property that holds regardless of where it landed.
    EXPECT_TRUE(residency.hostVisible);
    EXPECT_GT(residency.heapSizeBytes, 0u);

    device->DestroyBuffer(buffer);
}

TEST(DeviceMemoryTopology, UnknownBufferIsUnreportedNotZero)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const IDevice::BufferMemoryResidency residency =
        device->GetBufferMemoryResidency(BufferHandle{});
    EXPECT_FALSE(residency.reported);
}
