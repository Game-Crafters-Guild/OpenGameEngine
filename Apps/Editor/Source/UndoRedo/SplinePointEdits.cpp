#include "UndoRedo/SplinePointEdits.h"

#include <limits>

namespace GameEngine::Editor
{

SplinePointRenumbering SplinePointRenumbering::Inserted(uint32 countBefore, uint32 index,
                                                        uint32 insertedCount, bool closedAfter)
{
    SplinePointRenumbering renumbering;
    renumbering.OldToNew.resize(countBefore);
    for (uint32 i = 0; i < countBefore; ++i)
        renumbering.OldToNew[i] = i < index ? i : i + insertedCount;
    renumbering.PointCountAfter = countBefore + insertedCount;
    renumbering.ClosedAfter = closedAfter;
    return renumbering;
}

SplinePointRenumbering SplinePointRenumbering::Removed(uint32 countBefore,
                                                       std::span<const uint32> removed,
                                                       bool closedAfter)
{
    SplinePointRenumbering renumbering;
    renumbering.OldToNew.assign(countBefore, 0u);
    for (const uint32 index : removed)
    {
        if (index < countBefore)
            renumbering.OldToNew[index] = kRemoved;
    }
    uint32 next = 0;
    for (uint32& target : renumbering.OldToNew)
    {
        if (target != kRemoved)
            target = next++;
    }
    renumbering.PointCountAfter = next;
    renumbering.ClosedAfter = closedAfter;
    return renumbering;
}

SplinePointRenumbering SplinePointRenumbering::Nearest(std::span<const Mathematics::Vector3> before,
                                                       std::span<const Mathematics::Vector3> after,
                                                       bool closedAfter)
{
    SplinePointRenumbering renumbering;
    renumbering.OldToNew.assign(before.size(), kRemoved);
    renumbering.PointCountAfter = static_cast<uint32>(after.size());
    renumbering.ClosedAfter = closedAfter;
    for (size_t i = 0; i < before.size(); ++i)
    {
        float32 nearest = std::numeric_limits<float32>::max();
        for (uint32 k = 0; k < after.size(); ++k)
        {
            const Mathematics::Vector3 d = after[k] - before[i];
            const float32 distanceSquared = Mathematics::Vector3::Dot(d, d);
            if (distanceSquared < nearest)
            {
                nearest = distanceSquared;
                renumbering.OldToNew[i] = k;
            }
        }
    }
    return renumbering;
}

Event<const SplinePointEdit&>& SplinePointEdited()
{
    static Event<const SplinePointEdit&> edited;
    return edited;
}

} // namespace GameEngine::Editor
