#pragma once

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

namespace GameEngine
{

bool IsInterpolable(StylePropertyId id);
bool IsTransitionable(StylePropertyId id);
StyleValue ReadProperty(const ResolvedStyle& rs, StylePropertyId id);
void WriteProperty(ResolvedStyle& rs, StylePropertyId id, const StyleValue& v);
StyleValue InterpolateProperty(StylePropertyId id, const StyleValue& a, const StyleValue& b, float t);

} // namespace GameEngine
