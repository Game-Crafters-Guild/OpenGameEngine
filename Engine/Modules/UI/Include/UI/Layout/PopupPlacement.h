#pragma once

#include <algorithm>

namespace GameEngine::UI::Layout
{

struct PopupRect
{
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;
};

struct PopupPosition
{
    float X = 0.0f;
    float Y = 0.0f;
};

inline PopupPosition ClampPopupToViewport(const PopupRect& viewport,
                                          float desiredX,
                                          float desiredY,
                                          float popupWidth,
                                          float popupHeight,
                                          float margin = 0.0f)
{
    const float minX = viewport.X + margin;
    const float minY = viewport.Y + margin;
    const float maxX = std::max(minX, viewport.X + viewport.Width - margin - popupWidth);
    const float maxY = std::max(minY, viewport.Y + viewport.Height - margin - popupHeight);
    return {
        std::clamp(desiredX, minX, maxX),
        std::clamp(desiredY, minY, maxY),
    };
}

// alignInset raises the popup so its FIRST ROW (which sits below the panel's
// border + top padding) top-aligns with the anchor row, the way a native
// submenu aligns to its parent item. Pass the panel's border + top-padding
// height; 0 keeps the panel edge on the anchor's top.
inline PopupPosition PlaceSubmenuInViewport(const PopupRect& viewport,
                                            const PopupRect& anchor,
                                            float popupWidth,
                                            float popupHeight,
                                            float margin = 0.0f,
                                            float alignInset = 0.0f)
{
    const float viewportRight = viewport.X + viewport.Width - margin;
    const float preferredRightX = anchor.X + anchor.Width;
    const float preferredLeftX = anchor.X - popupWidth;

    float desiredX = preferredRightX;
    if (preferredRightX + popupWidth > viewportRight &&
        preferredLeftX >= viewport.X + margin)
    {
        desiredX = preferredLeftX;
    }

    return ClampPopupToViewport(
        viewport, desiredX, anchor.Y - alignInset, popupWidth, popupHeight, margin);
}

} // namespace GameEngine::UI::Layout
