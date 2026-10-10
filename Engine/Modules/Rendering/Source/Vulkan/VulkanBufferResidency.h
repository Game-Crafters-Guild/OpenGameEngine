#pragma once

#include <cstdint>
#include <optional>

#include "Rendering/Core/Device.h"

#ifdef _MSC_VER
#pragma warning(push)
// VMA is a 3rd-party header; MSVC /analyze emits noisy warnings inside it.
#pragma warning(disable : 6326 6386 6387)
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace GameEngine::Rendering
{

/**
 * @brief VMA allocation parameters for one buffer.
 *
 * Usage selects which branch of VMA's resolver runs; Flags carry the
 * host-access intent that steers it.
 */
struct BufferResidencyPolicy
{
    VmaMemoryUsage Usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationCreateFlags Flags = 0;
};

/**
 * @brief True when the GPU touches the buffer through something other than a
 *        transfer command.
 *
 * A buffer that declares no usage is created as a storage + uniform buffer, so
 * it counts as direct access. Mirrors VMA's own VmaBufferImageUsage::
 * ContainsDeviceAccess(), but reads the engine's declared usage rather than the
 * final VkBufferUsageFlags, which cannot answer the question: CreateBuffer
 * always adds TRANSFER_SRC | TRANSFER_DST, and adds SHADER_DEVICE_ADDRESS to
 * every buffer while the descriptor-buffer path is active, so VMA's own
 * predicate reads true for even a pure staging buffer there. That is harmless
 * inside VMA — the host preference this policy passes for staging still lands
 * on notPreferred |= DEVICE_LOCAL down both of VMA's branches — but a
 * vkUsage-based predicate *here* would classify staging as GPU-read and spend a
 * BAR window on bytes that are copied out once.
 */
inline bool BufferHasDirectDeviceAccess(uint32_t bufferUsage)
{
    constexpr uint32_t kTransferOnly = static_cast<uint32_t>(BufferUsage::TransferSrc) |
                                       static_cast<uint32_t>(BufferUsage::TransferDst);
    return bufferUsage == 0 || (bufferUsage & ~kTransferOnly) != 0;
}

/**
 * @brief True when one host-writable window covers every byte of device-local
 *        memory the device declares.
 *
 * This is the gate for BufferMemoryUsage::UploadDeviceLocalPreferred, and it is
 * a saturation test rather than a threshold: it asks whether the CPU-writable
 * device-local window *is* the device's VRAM, not whether it is "big enough".
 * There is no size constant here to guess, and that is the point — the question
 * a size constant would be answering is how much of a scarce aperture one
 * allocation class may spend, and that question only exists while the aperture
 * is scarce.
 *
 * Both sides of the comparison are chosen so that stays true on every heap
 * declaration a driver can produce, because drivers disagree about how to
 * declare the same hardware:
 *
 *  - The numerator is a MAX over host-visible device-local heaps: one
 *    allocation cannot span two heaps, so the widest single window is the most
 *    a mesh pool can ever be given.
 *  - The denominator is a SUM over device-local heaps. RADV and ANV CARVE the
 *    mappable aperture out of the VRAM total and declare the remainder as its
 *    own device-local heap; NVIDIA's reports declare the aperture as an extra
 *    heap overlapping a full-size VRAM heap. Compared against the largest heap
 *    instead of the total, the carve-out declaration would reduce this gate to
 *    "aperture >= half the card" — a threshold, with an invisible constant, that
 *    grants a genuinely scarce 256 MB BAR on any card small enough.
 *
 * The two real populations then sit on either side of saturation rather than
 * either side of a chosen cut:
 *
 *  - Resizable BAR / Smart Access Memory: the host-visible device-local type
 *    points at a heap that IS the device's device-local memory, so the
 *    comparison holds exactly. A host write competes for VRAM, which every
 *    device-local allocation already does, and VMA's next-best-type fallback
 *    handles exhaustion the same way it does for them.
 *  - A classic ~256 MB BAR: the host-visible device-local heap is a small
 *    fraction of the declared total, whichever way the driver declares it. The
 *    window is then a scarce shared aperture — per-frame ring buffers grow into
 *    it at runtime (FrameBufferAllocator recreates a ring to grow it), so a
 *    large write-once allocation class that filled it would demote a later
 *    per-frame ring to system RAM. Refusing the preference keeps those devices
 *    byte-identical.
 *
 * Conservative by construction, in both directions. A partially-resized BAR
 * (some firmware exposes 512 MB / 1 GB / 2 GB steps) leaves device-local memory
 * the window cannot reach and is refused on every declaration. An overlapping
 * declaration double-counts its aperture into the total and is refused a shade
 * earlier than strictly necessary; forgoing a win is the safe direction and
 * needs no per-vendor special case to stay safe.
 *
 * Scoped to what the device DECLARES, which is the strongest available scope:
 * RADV suppresses an invisible remainder under a ninth of the aperture, so such
 * a device declares slightly less device-local memory than it physically has —
 * and the suppressed bytes are not allocatable through any Vulkan path either.
 *
 * Disengaged topology is NOT eligible: a backend that does not walk its heaps
 * must keep today's behaviour, never be read as a device whose numbers happen
 * to compare equal at zero.
 */
inline bool DeviceLocalMemoryIsFullyHostWritable(const std::optional<DeviceMemoryTopology>& topology)
{
    if (!topology.has_value())
        return false;
    // Unified memory has one physical pool, so there is no device-local heap to
    // move bytes closer to — and the comparison below would read true for every
    // such device. Wider than the integrated-GPU test the Upload arm uses: a
    // CPU or paravirtualized device is unified too.
    if (topology->isUnifiedMemory)
        return false;
    if (topology->deviceLocalHeapBytesTotal == 0)
        return false;
    return topology->largestHostVisibleDeviceLocalHeapBytes >= topology->deviceLocalHeapBytesTotal;
}

/**
 * @brief The gate's decision in one clause, for the startup log.
 *
 * Lives beside the gate so the sentence a reader is asked to trust and the
 * predicate that produced it cannot drift apart, and so the wording is
 * assertable without capturing log output.
 */
inline const char* DescribeResidencyGateDecision(const std::optional<DeviceMemoryTopology>& topology)
{
    if (!topology.has_value())
        return "this backend does not report memory topology";
    if (topology->isUnifiedMemory)
        return "unified memory - no separate device-local heap to prefer";
    if (topology->deviceLocalHeapBytesTotal == 0)
        return "the device declares no device-local memory";
    if (DeviceLocalMemoryIsFullyHostWritable(topology))
        return "the host-visible device-local window covers all declared device-local memory";
    return "the host-visible device-local window is a smaller, separate aperture";
}

/**
 * @brief Map an engine memory class onto VMA's memory-type resolver.
 *
 * `Upload` states that the CPU writes the buffer. It does not state where the
 * bytes live — VMA decides that from the usage class plus the host-access
 * flags (vk_mem_alloc.h, FindMemoryPreferences):
 *
 *  - AUTO_PREFER_HOST + SEQUENTIAL_WRITE marks DEVICE_LOCAL *not preferred*,
 *    which removes BAR / resizable-BAR VRAM from the candidate set. On a
 *    discrete GPU the mapped write then always lands in system RAM and the GPU
 *    re-reads it across PCIe on every access.
 *  - Plain AUTO keeps HOST_VISIBLE *required* and marks DEVICE_LOCAL
 *    *preferred*, so the same buffer lands in BAR VRAM where one exists and
 *    falls back to system RAM where it does not. VMA retries the next-best
 *    memory type when an allocation fails, so a small BAR window fills and then
 *    spills instead of failing.
 *
 * Host-visibility is a hard requirement in every branch, so a buffer the engine
 * maps is always mappable. HOST_ACCESS_ALLOW_TRANSFER_INSTEAD is deliberately
 * never set: it demotes HOST_VISIBLE from required to preferred, and every
 * Upload caller in this engine maps.
 *
 * Two cases keep the host preference:
 *  - Transfer-only staging. Its bytes are copied out once, so BAR residency
 *    buys no repeat reads and spends a scarce heap.
 *  - Integrated GPUs. One physical memory pool, so preferring DEVICE_LOCAL
 *    cannot win bandwidth and can only steer the allocation into a smaller
 *    device-local carve-out where the driver exposes one. VMA applies its own
 *    integrated-GPU guard only on the ALLOW_TRANSFER_INSTEAD path, which this
 *    engine never takes, so the policy applies it here.
 *
 * `UploadDeviceLocalPreferred` is Upload plus a caller-declared shape
 * (write-once, GPU-re-read for many frames), and it is the one arm that reads
 * the device's memory topology. The device preference is granted only where
 * DeviceLocalMemoryIsFullyHostWritable holds; everywhere else — including every
 * backend that does not report a topology — it resolves exactly as Upload does
 * for the same usage bits. Both arms keep HOST_VISIBLE required, so the value
 * cannot cost a caller its mapping either way.
 */
inline BufferResidencyPolicy ResolveBufferResidencyPolicy(
    BufferMemoryUsage memoryUsage,
    uint32_t bufferUsage,
    bool isIntegratedGpu,
    const std::optional<DeviceMemoryTopology>& memoryTopology)
{
    BufferResidencyPolicy policy{};
    switch (memoryUsage)
    {
    case BufferMemoryUsage::Upload:
    {
        const bool preferDeviceHeap = BufferHasDirectDeviceAccess(bufferUsage) && !isIntegratedGpu;
        policy.Usage = preferDeviceHeap ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        policy.Flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    }
    case BufferMemoryUsage::UploadDeviceLocalPreferred:
        policy.Usage = DeviceLocalMemoryIsFullyHostWritable(memoryTopology)
                           ? VMA_MEMORY_USAGE_AUTO
                           : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        policy.Flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case BufferMemoryUsage::Readback:
        // HOST_ACCESS_RANDOM asks VMA for a HOST_CACHED type. The CPU reads
        // these buffers, and reading uncached write-combined memory is roughly
        // an order of magnitude slower than reading cached memory.
        policy.Usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        policy.Flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    case BufferMemoryUsage::DeviceLocal:
    case BufferMemoryUsage::Auto:
        policy.Usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        policy.Flags = 0;
        break;
    }
    return policy;
}

} // namespace GameEngine::Rendering
