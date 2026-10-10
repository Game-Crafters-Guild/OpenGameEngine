#pragma once

#include "UI/UIStyle.h"

#include <cstdint>

namespace GameEngine
{

// Compile-time typed property key. Carries the expected value type T and the
// StylePropertyId at compile time.
enum class StyleImpact : uint8_t
{
    Visual = 0,
    Layout
};

template <typename T>
struct StyleProp
{
    StylePropertyId id;

    // Derived from the canonical classification (GetStylePropertyImpact) so
    // the typed override path can never drift from it — a per-declaration
    // impact tag here had rotted (line-height/word-break/font-family tagged
    // visual, z-index tagged layout).
    constexpr StyleImpact Impact() const
    {
        return GetStylePropertyImpact(id).Layout ? StyleImpact::Layout : StyleImpact::Visual;
    }
};

} // namespace GameEngine
