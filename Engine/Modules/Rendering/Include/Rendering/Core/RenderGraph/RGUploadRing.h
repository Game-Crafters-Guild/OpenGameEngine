#pragma once

// One per-frame host-visible upload ring (framesInFlight slots), over the engine's
// real IDevice. ALL CPU-written per-frame data (camera UBOs, params, instance
// scratch) sub-allocates an {offset, ptr} from the current frame's slot —
// replacing the old per-resource perFrame=true N-copy multiplication + canonical-ID
// map with a single ring. Each slot is a persistently-mapped Upload buffer;
// BeginFrame selects slot = frameIndex % framesInFlight and rewinds it, so a slot
// is reused only once its frame's GPU work is complete (caller's frames-in-flight
// contract). Slots auto-grow (between frames) to the observed high-water mark.
//
// CONTRACT: one ring serves exactly ONE RGFrame stream. RGFrame::BeginFrame
// rotates + rewinds the slot, so a second RGFrame sharing the ring would
// rewind LIVE allocations mid-frame — every declaration-time write (camera
// UBOs, view params, per-view offsets) the first frame already handed to its
// passes would be stomped. Multi-window = one ring per window's frame stream
// (the pools may be shared; the ring may not).

#include "Rendering/Core/Device.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

class RGUploadRing
{
  public:
    // Usage every ring buffer is created with. Vertex is load-bearing for the
    // editor overlay passes (7e): gizmo line/tri vertices live in ring allocs
    // and SetVertexBuffer has no offset parameter, so callers fold the alloc
    // offset into firstVertex — requiring a stride that divides the
    // (power-of-two) alloc alignment.
    static constexpr uint32_t kBufferUsage =
        static_cast<uint32_t>(BufferUsage::Uniform) | static_cast<uint32_t>(BufferUsage::Storage) |
        static_cast<uint32_t>(BufferUsage::Vertex) | static_cast<uint32_t>(BufferUsage::TransferSrc);

    struct Alloc
    {
        BufferHandle Buffer{};
        uint64_t Offset = 0;
        void* Ptr = nullptr;
        bool Valid() const { return Ptr != nullptr; }
    };

    RGUploadRing(IDevice* device, uint32_t framesInFlight, uint64_t initialSlotCapacityBytes);
    ~RGUploadRing();
    RGUploadRing(const RGUploadRing&) = delete;
    RGUploadRing& operator=(const RGUploadRing&) = delete;

    // Select + rewind the slot for this frame (growing it to the high-water mark
    // first, before any Allocate this frame, so pointers never move mid-frame).
    void BeginFrame(uint64_t frameIndex);

    // After an in-place device rebuild every slot/overflow GPU buffer is freed and its
    // persistent map is gone, yet the cached BufferHandle still reads IsValid() (the
    // wholesale VMA/pool teardown does not bump per-handle generations) and the base
    // pointer dangles into freed VMA memory. Re-create each slot's main block against
    // the live device with a fresh map so Allocate() never hands out an Alloc whose
    // Ptr is a use-after-free. Called from RGFrame::BeginFrame on a rebuild-generation
    // change, next to the pool drops.
    void ReprovisionAfterDeviceRebuild();

    // Sub-allocate `size` bytes at `align`, returning the device buffer + offset +
    // mapped pointer. NEVER drops an allocation: when the main slot is full, the
    // bytes come from a chained overflow block created on demand (that frame's
    // data is fully valid); the total demand feeds the high-water mark so the
    // main slot converges to the full size the next time this slot comes around.
    // Overflow blocks are destroyed on the slot's next BeginFrame — the same
    // frames-in-flight guarantee that makes the slot itself reusable.
    Alloc Allocate(uint64_t size, uint64_t align = 256);

    uint32_t FramesInFlight() const { return static_cast<uint32_t>(m_Slots.size()); }
    uint32_t CurrentSlot() const { return m_CurSlot; }
    uint64_t SlotCapacity() const { return m_Slots.empty() ? 0 : m_Slots[m_CurSlot].Capacity; }
    uint64_t BytesUsedThisFrame() const { return m_FrameBytes; }
    size_t OverflowBlocksThisFrame() const
    {
        return m_Slots.empty() ? 0 : m_Slots[m_CurSlot].Overflow.size();
    }

  private:
    struct Block
    {
        BufferHandle Buffer{};
        uint64_t Capacity = 0;
        uint64_t Offset = 0;
        void* Mapped = nullptr; // persistently-mapped base pointer
    };
    struct Slot : Block
    {
        std::vector<Block> Overflow; // chained blocks for frames that outgrow Capacity
    };

    void CreateBlock(Block& b, uint64_t capacity);

    IDevice* m_Device = nullptr;
    std::vector<Slot> m_Slots;
    uint32_t m_CurSlot = 0;
    uint64_t m_HighWater = 0;
    uint64_t m_FrameBytes = 0;
    // Simulated single-block demand INCLUDING alignment padding — the number the
    // main block must grow to so the same workload fits without overflowing.
    uint64_t m_FrameDemand = 0;
};

} // namespace GameEngine::Rendering::RenderGraph
