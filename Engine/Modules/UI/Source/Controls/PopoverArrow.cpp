#include "UI/Controls/PopoverArrow.h"

#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"

#include <cstdint>

namespace GameEngine
{

namespace
{
// Soft edge curve, matching the tooltip arrow's look (logical px). Kept small —
// SDF rounding insets the triangle's corners, visibly shrinking small arrows.
constexpr float kArrowEdgeRoundingLogicalPx = 1.5f;
// Neutral dark fallback when no .popover-arrow border-color is themed.
constexpr uint32_t kFallbackFillARGB = 0xFF252526u;
} // namespace

PopoverArrow::PopoverArrow()
{
    AddClass("popover-arrow");
    Overrides().Set(Style::PointerEvents, false);
}

void PopoverArrow::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                        float x, float y, float W, float H)
{
    if (W <= 0.0f || H <= 0.0f)
        return;

    const uint32_t styledARGB = style.Visual.BorderColor.Top;
    const uint32_t argb = (styledARGB >> 24) != 0 ? styledARGB : kFallbackFillARGB;
    ctx.Emit(UI::MakeTriangle(x + W * 0.5f, y,
                              x, y + H,
                              x + W, y + H,
                              UI::PackFromARGB(argb),
                              kArrowEdgeRoundingLogicalPx * ctx.ContentScale));
}

} // namespace GameEngine
