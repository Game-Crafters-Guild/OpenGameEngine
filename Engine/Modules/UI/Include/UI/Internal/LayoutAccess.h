#pragma once

#include "UI/UIElement.h"
#include "UI/UIStyle.h"

namespace GameEngine
{

// Layout-solver / widget-internal write of the committed layout cache.
// Not part of the public UIElement API — gameplay and tools set position
// through Overrides() / UI::Layout::SetAbsolutePosition so Yoga produces
// the rect. This accessor exists so the solve, post-solve override patch,
// and a few widget-owned thumbs/ghosts can write the cache they own.
struct UILayoutAccess
{
    static void SetLastLayoutRect(UIElement& el, float x, float y, float w, float h)
    {
        el.m_LastX = x;
        el.m_LastY = y;
        el.m_LastW = w;
        el.m_LastH = h;
    }

    static void SetLayoutPadding(UIElement& el, const Box4& padding)
    {
        el.m_LayoutPadding = padding;
    }
};

} // namespace GameEngine
