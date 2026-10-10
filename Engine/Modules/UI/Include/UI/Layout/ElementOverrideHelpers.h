#pragma once

#include "Mathematics/Rect.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <optional>
#include <string>

namespace GameEngine::UI::Layout
{

namespace Detail
{

inline bool TryReadPx(const StyleOverrides& overrides, const StyleProp<StyleLength>& prop, float& outValue)
{
    const std::optional<StyleLength> value = overrides.Get(prop);
    if (!value.has_value() || !value->IsPx())
        return false;
    outValue = value->Value;
    return true;
}

} // namespace Detail

/// The element's resolved horizontal padding and border, in logical px: what a control is
/// wider than its content box.
inline float HorizontalInsetPx(const UIElement& element)
{
    const auto& layout = element.GetResolvedStyle().Layout;
    return layout.Padding.Left + layout.Padding.Right + layout.BorderWidth.Left + layout.BorderWidth.Right;
}

inline bool TryGetAbsolutePosition(const UIElement& element, Mathematics::Rect& outRect)
{
    const std::optional<PositionType> position = element.Overrides().Get(Style::Position);
    if (!position.has_value() || *position != PositionType::Absolute)
        return false;

    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    if (!Detail::TryReadPx(element.Overrides(), Style::PositionLeft, left) ||
        !Detail::TryReadPx(element.Overrides(), Style::PositionTop, top) ||
        !Detail::TryReadPx(element.Overrides(), Style::Width, width) ||
        !Detail::TryReadPx(element.Overrides(), Style::Height, height))
    {
        return false;
    }

    outRect = Mathematics::Rect{left, top, width, height};
    return true;
}

// positionOnlyFastPath: pure moves skip layout dirt entirely, relying on
// ApplyLayoutOverrideRects to patch committed rects post-solve every frame.
// Only virtualization-managed movers (recycled rows/cells repositioned per
// scroll tick) may pass true: they are guaranteed px-sized + absolute (so the
// override-rect registry patches them) and their binds raise their own style
// dirt. Everything else (popups, headers, outlines) keeps layout dirt so the
// Yoga style stays in sync and hit-testing never sees a stale rect between
// the solve and FinalizeSolve.
inline bool SetAbsolutePosition(UIElement& element, const Mathematics::Rect& rect,
                                bool positionOnlyFastPath = false)
{
    const Mathematics::Rect clamped{
        rect.X,
        rect.Y,
        std::max(0.0f, rect.Width),
        std::max(0.0f, rect.Height),
    };

    Mathematics::Rect current{};
    const bool hadRect = TryGetAbsolutePosition(element, current);
    if (hadRect && current == clamped)
        return false;
    const bool sizeChanged = !hadRect || current.Width != clamped.Width || current.Height != clamped.Height;

    if (positionOnlyFastPath && !sizeChanged)
    {
        element.Overrides().SetPositionOnlyPx(clamped.X, clamped.Y);
        element.MarkDirty(UIElement::VisualDirty);
        return true;
    }

    element.Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(clamped.X))
        .Set(Style::PositionTop, StyleLength::Px(clamped.Y))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(clamped.Width))
        .Set(Style::Height, StyleLength::Px(clamped.Height));

    element.MarkDirty(UIElement::VisualDirty);
    return true;
}

inline bool ClearAbsolutePosition(UIElement& element)
{
    const bool hasAbsoluteKeys =
        element.Overrides().Has(Style::Position.id) ||
        element.Overrides().Has(Style::PositionLeft.id) ||
        element.Overrides().Has(Style::PositionTop.id) ||
        element.Overrides().Has(Style::PositionRight.id) ||
        element.Overrides().Has(Style::PositionBottom.id) ||
        element.Overrides().Has(Style::Width.id) ||
        element.Overrides().Has(Style::Height.id);
    if (!hasAbsoluteKeys)
        return false;

    element.Overrides().Reset(Style::Position);
    element.Overrides().Reset(Style::PositionLeft);
    element.Overrides().Reset(Style::PositionTop);
    element.Overrides().Reset(Style::PositionRight);
    element.Overrides().Reset(Style::PositionBottom);
    element.Overrides().Reset(Style::Width);
    element.Overrides().Reset(Style::Height);
    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline bool SetElementInvisible(UIElement& element, bool invisible)
{
    if (invisible)
    {
        const std::optional<bool> visibility = element.Overrides().Get(Style::Visibility);
        const std::optional<bool> pointerEvents = element.Overrides().Get(Style::PointerEvents);
        if (visibility.has_value() && !visibility.value() && pointerEvents.has_value() && !pointerEvents.value())
            return false;

        element.Overrides().Set(Style::Visibility, false);
        element.Overrides().Set(Style::PointerEvents, false);
    }
    else
    {
        const bool hadVisibilityOverride = element.Overrides().Has(Style::Visibility.id);
        const bool hadPointerEventsOverride = element.Overrides().Has(Style::PointerEvents.id);
        if (!hadVisibilityOverride && !hadPointerEventsOverride)
            return false;

        element.Overrides().Reset(Style::Visibility);
        element.Overrides().Reset(Style::PointerEvents);
    }

    // `visibility` inherits through the cascade; without `StyleDirty`, descendants keep a
    // stale `visible` flag while the row suppresses its own primitives — children still draw.
    element.MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
    return true;
}

inline bool SetElementHidden(UIElement& element, bool hidden)
{
    if (hidden)
    {
        const std::optional<DisplayMode> currentDisplay = element.Overrides().Get(Style::Display);
        if (currentDisplay.has_value() && currentDisplay.value() == DisplayMode::None)
            return false;

        element.Overrides().Set(Style::Display, DisplayMode::None);
    }
    else
    {
        if (!element.Overrides().Has(Style::Display.id))
            return false;
        element.Overrides().Reset(Style::Display);
    }

    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline bool SetForcedHeight(UIElement& element, int heightPx)
{
    const float clampedHeight = static_cast<float>(std::max(0, heightPx));
    const std::optional<StyleLength> currentHeight = element.Overrides().Get(Style::Height);
    if (currentHeight.has_value() && currentHeight->IsPx() && currentHeight->Value == clampedHeight)
        return false;

    element.Overrides().Set(Style::Height, StyleLength::Px(clampedHeight));
    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline bool ClearForcedHeight(UIElement& element)
{
    if (!element.Overrides().Has(Style::Height.id))
        return false;

    Mathematics::Rect absoluteRect{};
    if (!TryGetAbsolutePosition(element, absoluteRect))
        element.Overrides().Reset(Style::Height);
    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline void SetBackgroundPath(UIElement& element, const std::string& path)
{
    BackgroundImageSource source{};
    source.Kind = BackgroundImageSource::SourceKind::Path;
    source.Value = path;
    element.Overrides()
        .Set(Style::BackgroundImage, source)
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true})
        .Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

inline void SetBackgroundResourceName(UIElement& element, const std::string& resourceName)
{
    BackgroundImageSource source{};
    source.Kind = BackgroundImageSource::SourceKind::ResourceName;
    source.Value = resourceName;
    element.Overrides()
        .Set(Style::BackgroundImage, source)
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true})
        .Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

inline void ClearBackgroundOverride(UIElement& element)
{
    BackgroundImageSource source{};
    source.Kind = BackgroundImageSource::SourceKind::ResourceName;
    source.Value.clear();
    element.Overrides()
        .Set(Style::BackgroundImage, source)
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true})
        .Set(Style::BackgroundTint, 0x00000000u);
    element.MarkDirty(UIElement::VisualDirty);
}

inline void DisableBackgroundOverride(UIElement& element)
{
    element.Overrides().Reset(Style::BackgroundImage);
    element.Overrides().Reset(Style::BackgroundRepeatProp);
    element.Overrides().Reset(Style::BackgroundSize);
    element.Overrides().Reset(Style::BackgroundPosition);
    element.Overrides().Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

} // namespace GameEngine::UI::Layout

