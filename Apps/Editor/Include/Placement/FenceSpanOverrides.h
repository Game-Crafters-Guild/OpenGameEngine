#pragma once

#include "Components/Spline/SplineFence.h"
#include "Types/Types.h"

namespace GameEngine::Editor
{

// The override a fence recipe carries for one span, or null: the first filled
// slot naming it, which is the one the layout applies.
const Components::SplineSpanOverride* FindSpanOverride(const Components::SplineFence& recipe,
                                                       uint32 point, uint32 ordinal);

// How many slots of the recipe's override table are filled.
uint32 CountSpanOverrides(const Components::SplineFence& recipe);

// Sets what one span draws: a gate or a pinned piece from `slot` of its pool,
// or, for None, the seeded wall again. The span's existing override is
// rewritten in place — every override naming it is cleared for None — and a
// new one takes the first empty slot. Returns false, with the recipe
// untouched, when a new override is asked for and every slot is filled.
bool SetSpanOverride(Components::SplineFence& recipe, uint32 point, uint32 ordinal,
                     Components::SplineSpanOverrideKind kind, uint8 slot);

} // namespace GameEngine::Editor
