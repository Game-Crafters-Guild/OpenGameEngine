#pragma once

#include <cmath>

namespace GameEngine
{
/** Key times (seconds) closer than this name the same key in selection and hit tests. */
inline constexpr float kKeySelectionEpsilon = 1e-4f;

inline bool KeyTimesMatch(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= kKeySelectionEpsilon;
}
} // namespace GameEngine
