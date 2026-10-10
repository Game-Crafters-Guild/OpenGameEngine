#pragma once

#include "Types/Types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine::PageStreaming
{

/// "Not resident": the slot of a key that holds none.
inline constexpr uint32 kNoResidentSlot = 0xFFFFFFFFu;

/// A key -> slot lookup result. The generation lets a stale index resolve to "not resident".
struct ResidentSlotRef
{
    uint32 Slot = kNoResidentSlot;
    uint32 Generation = 0;
    bool IsResident() const { return Slot != kNoResidentSlot; }
};

/// Generational key <-> slot residency pool over a fixed number of slots of a GPU cache texture:
/// the terrain atlas keys it by tile, the page cache by page address.
///
/// Eviction vs frames in flight: a slot's texels are not frame-versioned, so a frame in flight may
/// still sample a slot several frames after the CPU reassigns it. A released or evicted slot is
/// quarantined for framesInFlight frames before another key can take it, as descriptor recycling
/// defers reuse, so no in-flight draw samples a new key's texels through an old table entry (the
/// one-frame wrong-terrain flash). framesInFlight 0 disables the quarantine, which only the
/// disable-and-fail discriminator uses.
///
/// Eviction is driven by the residency update (keys leaving the wanted set Release; a nearer key
/// may evict a farther one in Acquire), never as a side effect of a lookup. Acquire places a key
/// only from the free list, so an over-subscribed pool degrades to "the farthest keys go without"
/// rather than thrashing a slot between two keys inside one frames-in-flight window.
template <typename Key, typename KeyHash>
class ResidentSlotPool
{
public:
    /// slotCount fixed slots; framesInFlight sets the quarantine depth (the renderer's swapchain
    /// image count).
    void Initialize(uint32 slotCount, uint32 framesInFlight);

    /// World-transition sweep: free every slot and clear the quarantine. Bumps every live slot's
    /// generation so any reference held across the seam resolves to "not resident".
    void Reset();

    /// Advance the frame clock and return slots whose quarantine expired to the free list. Resets
    /// the per-frame counters. Call once at the start of each residency update.
    void BeginFrame(uint64 frameIndex);

    /// The slot holding `key`, or a non-resident reference.
    ResidentSlotRef Find(const Key& key) const;

    /// Ensure `key` is resident. Returns its slot when it already is (and marks it touched this
    /// frame, pinning it against eviction). Otherwise assigns a free slot; with none free it evicts
    /// the farthest live slot that is untouched this frame and farther than priorityDistanceSq. The
    /// evicted slot is quarantined, so `key` stays without a slot until it clears.
    ResidentSlotRef Acquire(const Key& key, float32 priorityDistanceSq);

    /// Mark a resident key wanted this frame (pinned against eviction, its distance refreshed).
    void Touch(const Key& key, float32 priorityDistanceSq);

    /// Release a key's slot. A mismatched generation is refused. The slot is quarantined, not freed
    /// at once. True when a live slot was released.
    bool Release(const Key& key, uint32 generation);

    /// Per-frame event counts: zero on a frame with no residency change (a parked camera).
    uint32 AssignsThisFrame() const { return m_AssignsThisFrame; }
    uint32 EvictionsThisFrame() const { return m_EvictionsThisFrame; }

    uint32 ResidentCount() const { return m_ResidentCount; }
    uint32 SlotCount() const { return static_cast<uint32>(m_Slots.size()); }
    uint32 FreeCount() const { return static_cast<uint32>(m_FreeList.size()); }
    uint32 QuarantinedCount() const;
    uint32 FramesInFlight() const { return m_FramesInFlight; }

    /// The slot `key` occupies, or kNoResidentSlot; ignores generations.
    uint32 SlotOf(const Key& key) const;

    /// The key a live slot holds; meaningful only for a slot SlotOf returned.
    const Key& KeyOf(uint32 slot) const { return m_Slots[slot].Owner; }

private:
    enum class SlotState : uint8
    {
        Free,        // available for immediate Acquire
        Live,        // holds a resident key
        Quarantined, // freed but not yet reusable (frames-in-flight guard)
    };

    struct Slot
    {
        Key Owner{};
        uint32 Generation = 0;
        SlotState State = SlotState::Free;
        float32 LastPriorityDistanceSq = 0.0f;
        uint64 LastTouchFrame = 0;
        uint64 QuarantineUntilFrame = 0;
    };

    void QuarantineSlot(uint32 slotIndex);

    std::vector<Slot> m_Slots;
    std::vector<uint32> m_FreeList;
    std::unordered_map<Key, uint32, KeyHash> m_KeyToSlot;

    uint32 m_FramesInFlight = 0;
    uint64 m_CurrentFrame = 0;
    uint32 m_ResidentCount = 0;
    uint32 m_AssignsThisFrame = 0;
    uint32 m_EvictionsThisFrame = 0;
};

} // namespace GameEngine::PageStreaming

#include "PageStreaming/ResidentSlotPool.inl"
