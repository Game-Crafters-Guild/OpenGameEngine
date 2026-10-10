#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/ResolvedStyle.h"

namespace GameEngine
{

void Label::SetText(const std::string& t)
{
    if (m_Text == t)
        return;
    m_Text = t;

    // Impact inference (replaces the old SetTextVisualOnly special case —
    // callers no longer declare the impact, the system derives it): when
    // the resolved style pins BOTH axes to definite px sizes, Yoga takes
    // the measure-free fast path and never consults the text measure (the
    // same predicate the pre-measure collection uses), so new text can only
    // change this label's own primitive bytes — content-only, handled by
    // the render-side drain with no layout, resolve, or idle-gate impact.
    // Any content-driven axis re-measures, so it takes the layout path.
    // The rect>0 guard covers the not-yet-laid-out first set.
    const ResolvedStyle& rs = GetResolvedStyle();
    const bool fixedSize = rs.Layout.Width.IsPx() && rs.Layout.Height.IsPx();
    if (fixedSize && GetLayoutWidth() > 0.0f && GetLayoutHeight() > 0.0f)
    {
        MarkContentDirty();
        return;
    }

    for (UIElement* p = GetParent(); p; p = p->GetParent())
    {
        if (auto* sv = dynamic_cast<ScrollView*>(p))
        {
            // Text changes can affect horizontal overflow measurement.
            sv->MarkHorizontalMeasureDirty();
        }
    }

    // Marking LayoutDirty is sufficient: UIManager's layout-signature pass will detect whether this
    // change actually affects Yoga layout (e.g. intrinsic text measurement) and only then run a solve.
    // Avoid RequestRelayout() here; it forces a conservative "signatures for all nodes" pass next frame.
    MarkDirty(LayoutDirty | VisualDirty);
}

} // namespace GameEngine
