#include "Rendering/Core/RenderGraph/RGUploadRing.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Rendering::RenderGraph
{

namespace
{
constexpr uint64_t kMinOverflowBlockBytes = 64 * 1024;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + (a - 1)) & ~(a - 1); }

BufferDesc UploadDesc(uint64_t size)
{
    BufferDesc d;
    d.size = size;
    d.usage = RGUploadRing::kBufferUsage;
    d.memoryUsage = BufferMemoryUsage::Upload;
    d.flags = BufferCreateFlags::PersistentlyMapped;
    d.debugName = "RenderGraph.UploadRing";
    return d;
}
} // namespace

void RGUploadRing::CreateBlock(Block& b, uint64_t capacity)
{
    b.Capacity = capacity;
    b.Offset = 0;
    b.Buffer = m_Device->CreateBuffer(UploadDesc(capacity));
    b.Mapped = m_Device->MapBuffer(b.Buffer);
}

RGUploadRing::RGUploadRing(IDevice* device, uint32_t framesInFlight, uint64_t initialSlotCapacityBytes)
    : m_Device(device)
{
    const uint64_t cap = initialSlotCapacityBytes ? initialSlotCapacityBytes : 256;
    const uint32_t n = framesInFlight ? framesInFlight : 1;
    m_Slots.resize(n);
    for (Slot& s : m_Slots)
        CreateBlock(s, cap);
    m_HighWater = cap;
}

RGUploadRing::~RGUploadRing()
{
    for (Slot& s : m_Slots)
    {
        if (s.Buffer.IsValid())
            m_Device->DestroyBuffer(s.Buffer);
        for (Block& b : s.Overflow)
            m_Device->DestroyBuffer(b.Buffer);
    }
}

void RGUploadRing::BeginFrame(uint64_t frameIndex)
{
    m_CurSlot = static_cast<uint32_t>(frameIndex % m_Slots.size());
    Slot& s = m_Slots[m_CurSlot];
    // This slot's previous frame is retired (the ring's frames-in-flight
    // contract), so its overflow blocks are GPU-idle: destroy them and fold the
    // demand into the main block by growing to the high-water mark.
    for (Block& b : s.Overflow)
        m_Device->DestroyBuffer(b.Buffer);
    s.Overflow.clear();
    if (s.Capacity < m_HighWater)
    {
        m_Device->DestroyBuffer(s.Buffer);
        CreateBlock(s, m_HighWater);
    }
    s.Offset = 0;
    m_FrameBytes = 0;
    m_FrameDemand = 0;
}

void RGUploadRing::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;
    for (Slot& s : m_Slots)
    {
        // Overflow blocks are per-frame scratch; forget the dead ones (no
        // DestroyBuffer — a stale Destroy is a generational no-op, and the VMA
        // memory is already freed). Next BeginFrame/Allocate re-chains on demand.
        s.Overflow.clear();
        // CreateBlock overwrites Buffer/Mapped/Capacity/Offset with a live buffer +
        // fresh map. Recreate at the high-water so BeginFrame does not immediately
        // grow-and-recreate the slot again.
        CreateBlock(s, std::max(s.Capacity, m_HighWater));
    }
    m_CurSlot = 0;
    m_FrameBytes = 0;
    m_FrameDemand = 0;
}

RGUploadRing::Alloc RGUploadRing::Allocate(uint64_t size, uint64_t align)
{
    assert(align != 0 && (align & (align - 1)) == 0 && "RGUploadRing: align must be a power of two");
    Slot& s = m_Slots[m_CurSlot];

    auto tryBlock = [&](Block& b) -> Alloc
    {
        const uint64_t off = AlignUp(b.Offset, align);
        const uint64_t end = off + size;
        if (end > b.Capacity || b.Mapped == nullptr)
            return Alloc{};
        b.Offset = end;
        m_FrameBytes += size;
        // The block stays mapped for its lifetime, so the allocation is what
        // declares the bytes the caller is about to write.
        m_Device->FlushMappedRange(b.Buffer, static_cast<size_t>(off), static_cast<size_t>(size));
        return Alloc{b.Buffer, off, static_cast<uint8_t*>(b.Mapped) + off};
    };

    Alloc a = tryBlock(s);
    if (!a.Valid() && !s.Overflow.empty())
        a = tryBlock(s.Overflow.back());
    if (!a.Valid())
    {
        // Chain a new overflow block; the allocation is served immediately and
        // the frame's data stays fully valid.
        Block b;
        CreateBlock(b, std::max<uint64_t>(size + align, kMinOverflowBlockBytes));
        if (b.Mapped == nullptr)
        {
            // Creation/mapping failed: don't chain a dead block that every
            // subsequent Allocate would skip while piling more behind it.
            if (b.Buffer.IsValid())
                m_Device->DestroyBuffer(b.Buffer);
            return Alloc{};
        }
        s.Overflow.push_back(b);
        a = tryBlock(s.Overflow.back());
    }

    // High-water = the SIMULATED single-block demand including alignment padding
    // (summing per-block offsets drops the padding an allocation would need at
    // its converged position, so straddling workloads would never converge and
    // the slot would churn create/map/destroy every cycle).
    const uint64_t alignedOff = AlignUp(m_FrameDemand, align);
    m_FrameDemand = alignedOff + size;
    m_HighWater = std::max(m_HighWater, m_FrameDemand);
    return a;
}

} // namespace GameEngine::Rendering::RenderGraph
