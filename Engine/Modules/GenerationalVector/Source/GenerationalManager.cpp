#include "GenerationalVector/GenerationalManager.h"
#include <algorithm>

namespace GenerationalVector {

Handle GenerationalManager::Create() {
    Handle::IndexType index = AllocateSlot();
    Handle::GenerationType generation = m_Generations[index].Generation;
    
    m_Generations[index].Alive = true;
    ++m_AliveCount;
    
    return Handle(index, generation);
}

void GenerationalManager::Destroy(Handle handle) {
    if (!IsAlive(handle)) {
        return; // Handle is already invalid
    }
    
    Handle::IndexType index = handle.Index();
    
    // Mark as dead and increment generation
    m_Generations[index].Alive = false;
    ++m_Generations[index].Generation;
    
    // Add to free list
    FreeSlot(index);
    --m_AliveCount;
}

Handle GenerationalManager::HandleAt(Handle::IndexType index) const {
    if (index >= m_Generations.size() || !m_Generations[index].Alive) {
        return Handle{};
    }
    return Handle(index, m_Generations[index].Generation);
}

bool GenerationalManager::IsAlive(Handle handle) const {
    if (!handle.IsValid()) {
        return false;
    }
    
    Handle::IndexType index = handle.Index();
    if (index >= m_Generations.size()) {
        return false;
    }
    
    const SlotInfo& slot = m_Generations[index];
    return slot.Alive && slot.Generation == handle.Generation();
}

void GenerationalManager::Reserve(size_t capacity) {
    m_Generations.reserve(capacity);
    m_FreeList.reserve(capacity);
}

void GenerationalManager::Clear() {
    m_Generations.clear();
    m_FreeList.clear();
    m_AliveCount = 0;
}

Handle::IndexType GenerationalManager::AllocateSlot() {
    Handle::IndexType index;
    
    if (!m_FreeList.empty()) {
        // Reuse a freed slot
        index = m_FreeList.back();
        m_FreeList.pop_back();
    } else {
        // Allocate a new slot
        index = static_cast<Handle::IndexType>(m_Generations.size());
        m_Generations.emplace_back();
    }
    
    return index;
}

void GenerationalManager::FreeSlot(Handle::IndexType index) {
    m_FreeList.push_back(index);
}

} // namespace GenerationalVector
