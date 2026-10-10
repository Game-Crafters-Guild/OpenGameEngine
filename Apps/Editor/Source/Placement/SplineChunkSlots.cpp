#include "Placement/SplineChunkSlots.h"

#include <algorithm>

namespace GameEngine::Editor
{

std::vector<std::pair<uint32, uint32>> CarveChunkRanges(std::span<const float32> runDistance,
                                                        float32 chunkLengthMetres,
                                                        uint32 maxChunks)
{
    std::vector<std::pair<uint32, uint32>> ranges;
    uint32 first = 0;
    for (uint32 i = 1; i < runDistance.size(); ++i)
    {
        const bool longEnough = (runDistance[i] - runDistance[first]) >= chunkLengthMetres;
        if (longEnough && ranges.size() + 1u < maxChunks)
        {
            ranges.emplace_back(first, i);
            first = i;
        }
    }
    if (first + 1u < runDistance.size())
        ranges.emplace_back(first, static_cast<uint32>(runDistance.size() - 1u));
    return ranges;
}

ChunkSlotPlan ReconcileChunkSlots(std::span<const uint8> slotHasLiveEntity,
                                  std::span<const uint8> chunkHasGeometry)
{
    ChunkSlotPlan plan;
    plan.ChunkCount = static_cast<uint32>(chunkHasGeometry.size());
    const size_t slotCount = std::max(slotHasLiveEntity.size(), chunkHasGeometry.size());
    plan.Slots.resize(slotCount, ChunkSlotAction::Retire);
    for (size_t c = 0; c < chunkHasGeometry.size(); ++c)
    {
        // An empty stretch keeps Retire on ITS OWN slot — the hole — so every
        // later chunk stays bound to its own key and entity.
        if (!chunkHasGeometry[c])
            continue;
        const bool live = c < slotHasLiveEntity.size() && slotHasLiveEntity[c] != 0u;
        plan.Slots[c] = live ? ChunkSlotAction::Update : ChunkSlotAction::Create;
    }
    return plan;
}

} // namespace GameEngine::Editor
