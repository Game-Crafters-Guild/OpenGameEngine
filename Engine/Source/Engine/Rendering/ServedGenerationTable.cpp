#include "Engine/Rendering/ServedGenerationTable.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

void ServedGenerationTable::BeginFrame(uint32_t materialIndexCount)
{
    ++m_Frame;
    if (materialIndexCount <= m_MaterialIndexCount)
        return;
    // Grow geometrically so a scene streaming materials in one at a time does
    // not reallocate every frame. Old contents are not carried over: they are
    // stamped with earlier frames and would read as unset anyway.
    const uint32_t grown = std::max(materialIndexCount, m_MaterialIndexCount * 2u);
    const size_t slotCount = static_cast<size_t>(grown) * static_cast<size_t>(ServedPassClass::Count);
    m_Slots = std::make_unique<std::atomic<uint64_t>[]>(slotCount);
    for (size_t i = 0; i < slotCount; ++i)
        m_Slots[i].store(0u, std::memory_order_relaxed);
    m_MaterialIndexCount = grown;
}

void ServedGenerationTable::Record(uint32_t materialIndex, ServedPassClass passClass,
                                   uint32_t generation)
{
    if (materialIndex >= m_MaterialIndexCount || passClass >= ServedPassClass::Count)
        return;
    std::atomic<uint64_t>& slot = m_Slots[SlotIndex(materialIndex, passClass)];
    uint64_t observed = slot.load(std::memory_order_relaxed);
    for (;;)
    {
        uint64_t desired;
        if (FrameOf(observed) != m_Frame)
            desired = Pack(m_Frame, generation);
        else if (GenerationOf(observed) == generation || GenerationOf(observed) == kConflict)
            return;
        else
            desired = Pack(m_Frame, kConflict);
        // Another worker may have written this slot between the load and here;
        // re-evaluate against what it wrote rather than overwrite it.
        if (slot.compare_exchange_weak(observed, desired, std::memory_order_relaxed,
                                       std::memory_order_relaxed))
            return;
    }
}

std::optional<uint32_t> ServedGenerationTable::Get(uint32_t materialIndex,
                                                   ServedPassClass passClass) const
{
    if (materialIndex >= m_MaterialIndexCount || passClass >= ServedPassClass::Count)
        return std::nullopt;
    const uint64_t packed =
        m_Slots[SlotIndex(materialIndex, passClass)].load(std::memory_order_relaxed);
    if (FrameOf(packed) != m_Frame)
        return std::nullopt;
    return GenerationOf(packed);
}

} // namespace GameEngine::Engine::Renderer
