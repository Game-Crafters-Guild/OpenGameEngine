#pragma once

#include "UI/ResolvedStyle.h"
#include "UI/StyleOverrides.h"
#include "UI/UIStyle.h"

namespace GameEngine
{

void ApplyOverridesToResolvedStyle(ResolvedStyle& rs, const StyleOverrides& overrides);

// Collapses `border-width` to zero on an element whose `border-style` is
// `none`, which is what CSS means by it: the computed value of border-width is
// 0 when the style is none or hidden (CSS Backgrounds 3 §4.3), not the declared
// length with a paint-time veto.
//
// It has to run after the whole cascade, not inside the per-property appliers:
// `border-style` and `border-width` arrive in declaration order, so neither one
// can decide the pair on its own.
//
// Resolving it here rather than gating at the Yoga push is the difference
// between fixing the box model and making two of its readers agree. BorderWidth
// is read independently by the Yoga inset, ComputeContentBox (glyphs, carets,
// pointer hit-testing), TextArea's wrap width, the clip inset and inner clip
// radii, and the painted ring; a gate at the push would leave every other
// reader still insetting by an invisible border.
void ResolveUsedBorderWidths(ResolvedStyle& rs);

} // namespace GameEngine
