#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace UI
{

// SimpleSlotAllocator — fixed-size index pool. Each Allocate() returns a
// single slot; Free() returns it. No size classes, no growth heuristics —
// used for clip slots and any other resource where every element gets at
// most one slot.
//
// Underlying storage is the caller's; the allocator only tracks indices.
class SimpleSlotAllocator
{
  public:
    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

    SimpleSlotAllocator() = default;

    uint32_t Allocate()
    {
        if (!m_FreeList.empty())
        {
            const uint32_t s = m_FreeList.back();
            m_FreeList.pop_back();
            ++m_UsedSlots;
            return s;
        }
        const uint32_t s = m_TotalSlots++;
        ++m_UsedSlots;
        return s;
    }

    void Free(uint32_t slot)
    {
        m_FreeList.push_back(slot);
        --m_UsedSlots;
    }

    size_t GetUsedSlots() const  { return m_UsedSlots; }
    size_t GetTotalSlots() const { return m_TotalSlots; }
    size_t GetFreeListSize() const { return m_FreeList.size(); }

  private:
    std::vector<uint32_t> m_FreeList;
    uint32_t m_TotalSlots = 0;
    uint32_t m_UsedSlots  = 0;
};

} // namespace UI
} // namespace GameEngine
