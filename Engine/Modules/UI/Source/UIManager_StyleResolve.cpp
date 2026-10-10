#include "UI/UIManager.h"
#include "UIManager_Internal.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"

namespace GameEngine
{

namespace
{

// Propagate inheritable CSS properties from parent to child where the child
// did not explicitly set them (has* == false). fontFamily uses shared_ptr,
// so the != comparison is a pointer check; the assignment avoids an atomic
// ref-count increment when the pointers already match.
//
// font-size is not here: it is the one inherited property whose DECLARED form
// can also need the parent's value (a percentage scales it), and an override
// applied further down this function can introduce one, so ResolveFontSize runs
// after the overrides instead and covers the plain-inherit case too.
void InheritProperties(VisualStyle& child, const VisualStyle& parent)
{
    if (!child.HasColor)      child.Color = parent.Color;
    if (!child.HasFontWeight) child.FontWeight = parent.FontWeight;
    if (!child.HasFontStyle)  child.FontStyle = parent.FontStyle;
    if (!child.HasFontVariant) child.FontVariant = parent.FontVariant;
    if (!child.HasFontFamily && child.FontFamily != parent.FontFamily)
        child.FontFamily = parent.FontFamily;
    if (!child.HasTextAlign)  child.TextAlign = parent.TextAlign;
    if (!child.HasWordBreak)  child.WordBreak = parent.WordBreak;
    if (!child.HasOverflowWrap) child.OverflowWrap = parent.OverflowWrap;
    if (!child.HasWhiteSpace) child.WhiteSpace = parent.WhiteSpace;
    if (!child.HasLineHeight) child.LineHeight = parent.LineHeight;
    if (!child.HasLetterSpacing) child.LetterSpacing = parent.LetterSpacing;
    if (!child.HasCursor)     child.Cursor = parent.Cursor;
    if (!child.HasVisibility) child.Visible = parent.Visible;
    if (!child.HasPointerEvents) child.PointerEvents = parent.PointerEvents;
}

void ResolveRecursive(UIElement* el, const VisualStyle* parentVisual)
{
    if (!el)
        return;

    ResolvedStyle& rs = el->GetMutableResolvedStyle();

    if (parentVisual)
        InheritProperties(rs.Visual, *parentVisual);

    // Late-frame override safety net. BuildYogaRecursive now applies all dirty
    // overrides unconditionally (not gated by needCascade). This block catches
    // overrides set between the last BuildYogaRecursive and ResolveStyles
    // (e.g., during event dispatch or late callbacks that don't trigger a tree
    // rebuild). Kept until proven no longer needed by runtime observation.
    if (el->Overrides().IsVisualDirty() || el->Overrides().IsLayoutDirty())
    {
        ApplyOverridesToResolvedStyle(rs, el->Overrides());
    }

    // Both the inherit case and the percentage case, against the parent's
    // computed size — the root has no parent and uses the initial value. Runs
    // after the overrides because one of them may have just declared a
    // percentage, and it is idempotent, so re-running it on an already-resolved
    // style is a no-op rather than a compounding scale.
    ResolveFontSize(rs.Visual, parentVisual ? parentVisual->FontSize : kInitialFontSizePx);

    rs.Visual.Opacity = rs.Visual.LocalOpacity;
    el->Overrides().ClearDirty();

    const VisualStyle* thisVisual = &rs.Visual;

    for (auto& child : el->GetChildren())
        ResolveRecursive(child.get(), thisVisual);

    if (UIElement* tgt = el->GetMountTarget())
        ResolveRecursive(tgt, thisVisual);
}

} // namespace

void UIManager::ResolveStyles()
{
    if (!m_Root)
        return;
    // Full-tree walk — only worth doing when something could have changed a
    // resolved style since the last one: any dirty mark, structural change,
    // or heavy Update pass (which force-sets the flag before its tail call).
    // exchange(false) up front so marks raised DURING the walk (e.g. from
    // another thread) re-arm it for the next frame instead of being lost;
    // acquire pairs with the marking side's release store.
    if (!m_ResolvedStylesDirty.exchange(false, std::memory_order_acquire))
        return;
    ResolveRecursive(m_Root.get(), nullptr);
}

const ResolvedStyle* UIManager::TryGetResolvedStyleFor(const UIElement* el) const
{
    if (!el)
        return nullptr;
    return &el->GetResolvedStyle();
}

} // namespace GameEngine

// ApplyOverridesToResolvedStyle -> StyleApplier.cpp
// RefreshDynamicStyleAnalysis -> UIManager_StyleAnalysis.cpp
