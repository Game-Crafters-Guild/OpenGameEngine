#pragma once

// The definitions of ResidentSlotPool (ResidentSlotPool.h).

namespace GameEngine::PageStreaming
{

template <typename Key, typename KeyHash>
void ResidentSlotPool<Key, KeyHash>::Initialize(uint32 slotCount, uint32 framesInFlight)
{
    m_Slots.assign(slotCount, Slot{});
    m_FreeList.clear();
    m_FreeList.reserve(slotCount);
    // Hand out low indices first (deterministic packing for the seam oracles).
    for (uint32 i = slotCount; i > 0; --i)
        m_FreeList.push_back(i - 1);
    m_KeyToSlot.clear();
    m_FramesInFlight = framesInFlight;
    m_CurrentFrame = 0;
    m_ResidentCount = 0;
    m_AssignsThisFrame = 0;
    m_EvictionsThisFrame = 0;
}

template <typename Key, typename KeyHash>
void ResidentSlotPool<Key, KeyHash>::Reset()
{
    m_FreeList.clear();
    m_FreeList.reserve(m_Slots.size());
    for (uint32 i = static_cast<uint32>(m_Slots.size()); i > 0; --i)
    {
        Slot& s = m_Slots[i - 1];
        if (s.State != SlotState::Free)
            ++s.Generation; // stale any handle held across the sweep
        s.State = SlotState::Free;
        s.Owner = {};
        s.LastTouchFrame = 0;
        s.QuarantineUntilFrame = 0;
        m_FreeList.push_back(i - 1);
    }
    m_KeyToSlot.clear();
    m_ResidentCount = 0;
    m_AssignsThisFrame = 0;
    m_EvictionsThisFrame = 0;
}

template <typename Key, typename KeyHash>
void ResidentSlotPool<Key, KeyHash>::BeginFrame(uint64 frameIndex)
{
    m_CurrentFrame = frameIndex;
    m_AssignsThisFrame = 0;
    m_EvictionsThisFrame = 0;

    for (uint32 i = 0; i < m_Slots.size(); ++i)
    {
        Slot& s = m_Slots[i];
        if (s.State == SlotState::Quarantined && frameIndex >= s.QuarantineUntilFrame)
        {
            s.State = SlotState::Free;
            m_FreeList.push_back(i);
        }
    }
}

template <typename Key, typename KeyHash>
ResidentSlotRef ResidentSlotPool<Key, KeyHash>::Find(const Key& key) const
{
    auto it = m_KeyToSlot.find(key);
    if (it == m_KeyToSlot.end())
        return {};
    const Slot& s = m_Slots[it->second];
    return ResidentSlotRef{it->second, s.Generation};
}

template <typename Key, typename KeyHash>
uint32 ResidentSlotPool<Key, KeyHash>::SlotOf(const Key& key) const
{
    auto it = m_KeyToSlot.find(key);
    return it == m_KeyToSlot.end() ? kNoResidentSlot : it->second;
}

template <typename Key, typename KeyHash>
void ResidentSlotPool<Key, KeyHash>::QuarantineSlot(uint32 slotIndex)
{
    Slot& s = m_Slots[slotIndex];
    if (s.State != SlotState::Live)
        return;
    m_KeyToSlot.erase(s.Owner);
    ++s.Generation;
    --m_ResidentCount;
    if (m_FramesInFlight == 0)
    {
        // Quarantine disabled (discriminator): the slot is immediately reusable,
        // which is exactly the wrong-terrain hazard the eviction-churn oracle catches.
        s.State = SlotState::Free;
        m_FreeList.push_back(slotIndex);
    }
    else
    {
        s.State = SlotState::Quarantined;
        s.QuarantineUntilFrame = m_CurrentFrame + m_FramesInFlight;
    }
}

template <typename Key, typename KeyHash>
void ResidentSlotPool<Key, KeyHash>::Touch(const Key& key, float32 priorityDistanceSq)
{
    auto it = m_KeyToSlot.find(key);
    if (it == m_KeyToSlot.end())
        return;
    Slot& s = m_Slots[it->second];
    s.LastTouchFrame = m_CurrentFrame;
    s.LastPriorityDistanceSq = priorityDistanceSq;
}

template <typename Key, typename KeyHash>
ResidentSlotRef ResidentSlotPool<Key, KeyHash>::Acquire(const Key& key, float32 priorityDistanceSq)
{
    if (auto it = m_KeyToSlot.find(key); it != m_KeyToSlot.end())
    {
        Slot& s = m_Slots[it->second];
        s.LastTouchFrame = m_CurrentFrame;
        s.LastPriorityDistanceSq = priorityDistanceSq;
        return ResidentSlotRef{it->second, s.Generation};
    }

    // Assign a free slot if one is available.
    if (!m_FreeList.empty())
    {
        const uint32 slotIndex = m_FreeList.back();
        m_FreeList.pop_back();
        Slot& s = m_Slots[slotIndex];
        s.Owner = key;
        s.State = SlotState::Live;
        s.LastTouchFrame = m_CurrentFrame;
        s.LastPriorityDistanceSq = priorityDistanceSq;
        m_KeyToSlot[key] = slotIndex;
        ++m_ResidentCount;
        ++m_AssignsThisFrame;
        return ResidentSlotRef{slotIndex, s.Generation};
    }

    // Pool full: evict the farthest live slot that is untouched this frame and
    // strictly farther than the requester (a nearer key reclaims a farther
    // one). The evicted slot is quarantined, so the requester gets no slot THIS
    // frame — it renders coarse until the slot clears quarantine and a later
    // Acquire places it. Refusing same-frame reuse is the Risk 2 guarantee.
    uint32 victim = kNoResidentSlot;
    float32 victimDistSq = priorityDistanceSq;
    for (uint32 i = 0; i < m_Slots.size(); ++i)
    {
        const Slot& s = m_Slots[i];
        if (s.State != SlotState::Live || s.LastTouchFrame == m_CurrentFrame)
            continue;
        if (s.LastPriorityDistanceSq > victimDistSq)
        {
            victim = i;
            victimDistSq = s.LastPriorityDistanceSq;
        }
    }
    if (victim != kNoResidentSlot)
    {
        QuarantineSlot(victim);
        ++m_EvictionsThisFrame;
    }
    return {}; // not resident this frame
}

template <typename Key, typename KeyHash>
uint32 ResidentSlotPool<Key, KeyHash>::QuarantinedCount() const
{
    uint32 n = 0;
    for (const Slot& s : m_Slots)
        if (s.State == SlotState::Quarantined)
            ++n;
    return n;
}

template <typename Key, typename KeyHash>
bool ResidentSlotPool<Key, KeyHash>::Release(const Key& key, uint32 generation)
{
    auto it = m_KeyToSlot.find(key);
    if (it == m_KeyToSlot.end())
        return false;
    const uint32 slotIndex = it->second;
    if (m_Slots[slotIndex].Generation != generation)
        return false; // stale handle — refuse (the current owner keeps its slot)
    QuarantineSlot(slotIndex);
    return true;
}

} // namespace GameEngine::PageStreaming
