#include "Placement/FenceSpanOverrides.h"

#include <limits>

namespace GameEngine::Editor
{
namespace
{

using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

bool Names(const SplineSpanOverride& entry, uint32 point, uint32 ordinal)
{
    return entry.Kind != SplineSpanOverrideKind::None && entry.PointIndex == point &&
           entry.SpanOrdinal == ordinal;
}

} // namespace

const SplineSpanOverride* FindSpanOverride(const Components::SplineFence& recipe, uint32 point,
                                           uint32 ordinal)
{
    for (const SplineSpanOverride& entry : recipe.Overrides)
    {
        if (Names(entry, point, ordinal))
            return &entry;
    }
    return nullptr;
}

uint32 CountSpanOverrides(const Components::SplineFence& recipe)
{
    uint32 count = 0;
    for (const SplineSpanOverride& entry : recipe.Overrides)
        count += entry.Kind != SplineSpanOverrideKind::None ? 1u : 0u;
    return count;
}

bool SetSpanOverride(Components::SplineFence& recipe, uint32 point, uint32 ordinal,
                     SplineSpanOverrideKind kind, uint8 slot)
{
    // The table stores the ordinal in 16 bits; a span past that is not one a
    // fence of 4096 pieces can have.
    if (ordinal > std::numeric_limits<uint16>::max())
        return false;

    if (kind == SplineSpanOverrideKind::None)
    {
        for (SplineSpanOverride& entry : recipe.Overrides)
        {
            if (Names(entry, point, ordinal))
                entry = SplineSpanOverride{};
        }
        return true;
    }

    SplineSpanOverride* target = nullptr;
    for (SplineSpanOverride& entry : recipe.Overrides)
    {
        if (Names(entry, point, ordinal))
        {
            target = &entry;
            break;
        }
    }
    if (!target)
    {
        for (SplineSpanOverride& entry : recipe.Overrides)
        {
            if (entry.Kind == SplineSpanOverrideKind::None)
            {
                target = &entry;
                break;
            }
        }
    }
    if (!target)
        return false;
    target->PointIndex = point;
    target->SpanOrdinal = static_cast<uint16>(ordinal);
    target->Kind = kind;
    target->PoolSlot = slot;
    return true;
}

} // namespace GameEngine::Editor
