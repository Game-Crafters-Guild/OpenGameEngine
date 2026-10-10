#pragma once

#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"

#include <cstdint>

namespace GameEngine::UI {

/// The fill every text control paints behind selected text, packed RGBA8 for a
/// UIPrimitive.
///
/// The colour is the element's `selection-color` and nothing else: it never
/// derives from the border, so a field whose border signals state (a neutral
/// rest, a focus accent, an error red) selects in the same colour as every other
/// field. Its alpha is scaled by the focus state: 0x66/0xFF while the control
/// holds the focus and 0x44/0xFF when it does not, so an opaque selection-color
/// fills at 0x66 focused and dims to 0x44 on blur, and an authored rgba() alpha
/// is kept in proportion.
[[nodiscard]] inline uint32_t PackedTextSelectionFill(const VisualStyle& style, bool focused)
{
    constexpr uint32_t kFocusedAlpha = 0x66u;
    constexpr uint32_t kUnfocusedAlpha = 0x44u;
    const uint32_t stateAlpha = focused ? kFocusedAlpha : kUnfocusedAlpha;
    const uint32_t alpha = ((style.SelectionColor >> 24) * stateAlpha + 127u) / 255u;
    return PackFromARGB((alpha << 24) | (style.SelectionColor & 0x00FFFFFFu));
}

} // namespace GameEngine::UI
