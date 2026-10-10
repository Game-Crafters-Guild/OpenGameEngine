#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

// Anchor arrow for popups/popovers: fills its layout rect with a filled, softly
// rounded triangle whose apex points up at the anchor control (same primitive
// the tooltip overlay draws). Fill color comes from the theme's .popover-arrow
// border-color rule — the slider's active-color carrier idiom, chosen because a
// background-color would make the base painter draw a rect behind the triangle.
// Purely visual; never receives pointer events.
class PopoverArrow : public UIElement
{
public:
    PopoverArrow();

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float W, float H) override;
};

} // namespace GameEngine
