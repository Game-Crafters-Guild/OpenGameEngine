#include "UI/Layout/YogaLayout.h"
#include <algorithm>
#include <cassert>

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/node/Node.h>
#endif

namespace GameEngine { namespace UILayout {

// MT-4.0 seam guard state. thread_local so the state is per-thread; the assert
// is only present in true Debug builds (see header note).
#ifdef _DEBUG
namespace { thread_local bool g_CascadeComputeActive = false; }
void SetCascadeComputeActive(bool active) { g_CascadeComputeActive = active; }
bool CascadeComputeActive() { return g_CascadeComputeActive; }
#else
void SetCascadeComputeActive(bool) {}
bool CascadeComputeActive() { return false; }
#endif

// Called by every YogaAdapter mutator. In Debug it asserts the cascade-compute
// region is not active, catching a seam violation (Yoga mutation inside the
// compute stage, which must stay Yoga-free) at its source. No-op in release.
static void AssertNotCascadeCompute()
{
#ifdef _DEBUG
    assert(!g_CascadeComputeActive &&
           "Yoga mutation inside cascade-compute region (MT-4.0 seam violation)");
#endif
}

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
// `fallback` is what AlignItems::Auto and any unmapped value resolve to; it
// differs per property (align-items has no Auto, align-self defers with it).
static YGAlign ToYGAlign(AlignItems align, YGAlign fallback) {
    switch (align) {
        case AlignItems::Stretch:   return YGAlignStretch;
        case AlignItems::FlexStart: return YGAlignFlexStart;
        case AlignItems::Center:    return YGAlignCenter;
        case AlignItems::FlexEnd:   return YGAlignFlexEnd;
        case AlignItems::Baseline:  return YGAlignBaseline;
        case AlignItems::Auto:      return fallback;
    }
    return fallback;
}
#endif

bool YogaAdapter::IsAvailable() {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    return true;
#else
    return false;
#endif
}

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
// One config for every node in every tree. Yoga reads the pixel grid from the
// node being rounded, so a tree whose levels held different configs would round
// its own parents and children onto different grids. Owning a config is the
// only way to set the factor at all — YGConfigGetDefault() hands back a
// YGConfigConstRef.
//
// Deliberately leaked: nodes outlive any static destructor ordering we could
// rely on, and Yoga reads the config while freeing them.
static YGConfigRef SharedConfig()
{
    static YGConfigRef config = YGConfigNew();
    return config;
}
#endif

YGNodeRef YogaAdapter::CreateNode() {
    AssertNotCascadeCompute();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    return YGNodeNewWithConfig(SharedConfig());
#else
    return nullptr;
#endif
}

void YogaAdapter::SetContentScale(float contentScale) {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // Grid steps per DEVICE pixel. Chrome lays out in LayoutUnits of 1/64 of a
    // pixel and quantises every used length to them; matching the quantum is
    // what lets a value Chrome reports as 20.8 logical px survive the solve
    // instead of being rounded to a coordinate Chrome never had.
    constexpr float kGridStepsPerDevicePx = 64.0f;
    // A zero factor disables Yoga's rounding entirely; clamp so a bad platform
    // scale report cannot silently turn the grid off.
    const float factor = std::max(0.01f, contentScale) * kGridStepsPerDevicePx;
    YGConfigRef config = SharedConfig();
    // Yoga's per-node layout cache is not keyed on the config, so a subtree that
    // skips re-solving keeps values rounded on whatever grid was in force when
    // it last ran. Writing only on a real change confines that to the frames
    // where the content scale actually moves — and those already force a full
    // relayout (UIManager::Update's contentScaleChanged).
    if (YGConfigGetPointScaleFactor(config) == factor)
        return;
    YGConfigSetPointScaleFactor(config, factor);
#else
    (void)contentScale;
#endif
}

void YogaAdapter::DestroyNode(YGNodeRef node) {
    AssertNotCascadeCompute();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (node) YGNodeFree(node);
#else
    (void)node;
#endif
}

void YogaAdapter::ApplyStyle(YGNodeRef node, const ResolvedStyle& style) {
    AssertNotCascadeCompute();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!node) return;

    const auto& lay = style.Layout;

    YGNodeStyleSetDisplay(node, lay.DisplayMode == DisplayMode::None ? YGDisplayNone : YGDisplayFlex);

    {
        YGDirection dir = YGDirectionInherit;
        switch (lay.Direction) {
            case Direction::LTR: dir = YGDirectionLTR; break;
            case Direction::RTL: dir = YGDirectionRTL; break;
            default: dir = YGDirectionInherit; break;
        }
        YGNodeStyleSetDirection(node, dir);
    }

    {
        YGPositionType pt = (lay.PositionType == PositionType::Absolute) ? YGPositionTypeAbsolute : YGPositionTypeRelative;
        YGNodeStyleSetPositionType(node, pt);
        auto applyEdge = [&](YGEdge edge, const StyleLength& len)
        {
            if (len.Unit == StyleLength::UnitType::Auto)
                YGNodeStyleSetPosition(node, edge, YGUndefined);
            else if (len.Unit == StyleLength::UnitType::Percent)
                YGNodeStyleSetPositionPercent(node, edge, len.Value);
            else
                YGNodeStyleSetPosition(node, edge, len.Value);
        };

        applyEdge(YGEdgeTop, lay.PositionTop);
        applyEdge(YGEdgeRight, lay.PositionRight);
        applyEdge(YGEdgeBottom, lay.PositionBottom);
        applyEdge(YGEdgeLeft, lay.PositionLeft);
    }

    FlexDirection effectiveDir = lay.FlexDirection;
    if (!lay.HasFlexDirection)
    {
        switch (lay.DisplayMode)
        {
            case DisplayMode::Block:
            case DisplayMode::Inline:
                effectiveDir = FlexDirection::Column;
                break;
            case DisplayMode::Flex:
                effectiveDir = FlexDirection::Row;
                break;
            default:
                break;
        }
    }
    YGNodeStyleSetFlexDirection(node, effectiveDir == FlexDirection::Row ? YGFlexDirectionRow : YGFlexDirectionColumn);

    YGNodeStyleSetFlexWrap(node, lay.FlexWrap ? YGWrapWrap : YGWrapNoWrap);

    // align-items has no Auto; align-self uses Auto to defer to the container.
    YGNodeStyleSetAlignItems(node, ToYGAlign(lay.AlignItems, YGAlignStretch));

    {
        YGAlign a = YGAlignStretch;
        switch (lay.AlignContent) {
            case AlignContent::FlexStart: a = YGAlignFlexStart; break;
            case AlignContent::Center: a = YGAlignCenter; break;
            case AlignContent::FlexEnd: a = YGAlignFlexEnd; break;
            case AlignContent::SpaceBetween: a = YGAlignSpaceBetween; break;
            case AlignContent::SpaceAround: a = YGAlignSpaceAround; break;
            case AlignContent::SpaceEvenly: a = YGAlignSpaceEvenly; break;
            default: a = YGAlignStretch; break;
        }
        YGNodeStyleSetAlignContent(node, a);
    }

    {
        YGJustify j = YGJustifyFlexStart;
        switch (lay.JustifyContent) {
            case JustifyContent::Center: j = YGJustifyCenter; break;
            case JustifyContent::FlexEnd: j = YGJustifyFlexEnd; break;
            case JustifyContent::SpaceBetween: j = YGJustifySpaceBetween; break;
            case JustifyContent::SpaceAround: j = YGJustifySpaceAround; break;
            case JustifyContent::SpaceEvenly: j = YGJustifySpaceEvenly; break;
            default: j = YGJustifyFlexStart; break;
        }
        YGNodeStyleSetJustifyContent(node, j);
    }

    YGNodeStyleSetAlignSelf(node, ToYGAlign(lay.AlignSelf, YGAlignAuto));

    YGNodeStyleSetFlexGrow(node, lay.FlexGrow);
    // The flex-container answer. A node whose container establishes block flow
    // takes 0 instead when nothing declared the property, but that is a parent
    // fact and this maps one style onto one node — the manager re-asserts it
    // right after every push (ApplyBlockFlowShrinkDefault).
    YGNodeStyleSetFlexShrink(node, lay.FlexShrink.value_or(kCssInitialFlexShrink));
    if (lay.FlexBasis.Unit == StyleLength::UnitType::Auto)
        YGNodeStyleSetFlexBasisAuto(node);
    else if (lay.FlexBasis.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetFlexBasisPercent(node, lay.FlexBasis.Value);
    else
        YGNodeStyleSetFlexBasis(node, lay.FlexBasis.Value);

    // `auto` is not a length: the edge absorbs the flex line's remaining free
    // space and, on the main axis, pre-empts justify-content. Yoga has a
    // dedicated setter for it — pushing the value would land the 0 the Auto
    // unit carries and lay the element out as `margin: 0`.
    auto applyMargin = [node](YGEdge edge, bool isAuto, bool isPercent, float value)
    {
        if (isAuto)
            YGNodeStyleSetMarginAuto(node, edge);
        else if (isPercent)
            YGNodeStyleSetMarginPercent(node, edge, value);
        else
            YGNodeStyleSetMargin(node, edge, value);
    };
    applyMargin(YGEdgeTop,    lay.MarginIsAuto.Top,    lay.MarginIsPercent.Top,    lay.Margin.Top);
    applyMargin(YGEdgeRight,  lay.MarginIsAuto.Right,  lay.MarginIsPercent.Right,  lay.Margin.Right);
    applyMargin(YGEdgeBottom, lay.MarginIsAuto.Bottom, lay.MarginIsPercent.Bottom, lay.Margin.Bottom);
    applyMargin(YGEdgeLeft,   lay.MarginIsAuto.Left,   lay.MarginIsPercent.Left,   lay.Margin.Left);

    if (lay.PaddingIsPercent.Top)    YGNodeStyleSetPaddingPercent(node, YGEdgeTop,    lay.Padding.Top);    else YGNodeStyleSetPadding(node, YGEdgeTop,    lay.Padding.Top);
    if (lay.PaddingIsPercent.Right)  YGNodeStyleSetPaddingPercent(node, YGEdgeRight,  lay.Padding.Right);  else YGNodeStyleSetPadding(node, YGEdgeRight,  lay.Padding.Right);
    if (lay.PaddingIsPercent.Bottom) YGNodeStyleSetPaddingPercent(node, YGEdgeBottom, lay.Padding.Bottom); else YGNodeStyleSetPadding(node, YGEdgeBottom, lay.Padding.Bottom);
    if (lay.PaddingIsPercent.Left)   YGNodeStyleSetPaddingPercent(node, YGEdgeLeft,   lay.Padding.Left);   else YGNodeStyleSetPadding(node, YGEdgeLeft,   lay.Padding.Left);

    // Border insets the content box on top of padding. CSS has no percentage
    // border-width, so these are always absolute lengths.
    YGNodeStyleSetBorder(node, YGEdgeTop,    lay.BorderWidth.Top);
    YGNodeStyleSetBorder(node, YGEdgeRight,  lay.BorderWidth.Right);
    YGNodeStyleSetBorder(node, YGEdgeBottom, lay.BorderWidth.Bottom);
    YGNodeStyleSetBorder(node, YGEdgeLeft,   lay.BorderWidth.Left);

    // A percentage gutter resolves against this element's own content box in the
    // gutter's axis (css-align-3 §8.1) — a size only Yoga knows — so the unit
    // travels to Yoga instead of being flattened to px in the cascade.
    if (lay.RowGapIsPercent)    YGNodeStyleSetGapPercent(node, YGGutterRow,    lay.RowGap);
    else                        YGNodeStyleSetGap(node,        YGGutterRow,    lay.RowGap);
    if (lay.ColumnGapIsPercent) YGNodeStyleSetGapPercent(node, YGGutterColumn, lay.ColumnGap);
    else                        YGNodeStyleSetGap(node,        YGGutterColumn, lay.ColumnGap);

    if (lay.Width.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetWidthPercent(node, lay.Width.Value);
    else if (lay.Width.Unit == StyleLength::UnitType::Px)
        YGNodeStyleSetWidth(node, lay.Width.Value);
    else
        YGNodeStyleSetWidthAuto(node);

    if (lay.Height.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetHeightPercent(node, lay.Height.Value);
    else if (lay.Height.Unit == StyleLength::UnitType::Px)
        YGNodeStyleSetHeight(node, lay.Height.Value);
    else
        YGNodeStyleSetHeightAuto(node);

    if (lay.MinWidth.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetMinWidthPercent(node, lay.MinWidth.Value);
    else if (lay.MinWidth.Unit == StyleLength::UnitType::Px)
        YGNodeStyleSetMinWidth(node, lay.MinWidth.Value);
    else
        YGNodeStyleSetMinWidth(node, YGUndefined);

    if (lay.MinHeight.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetMinHeightPercent(node, lay.MinHeight.Value);
    else if (lay.MinHeight.Unit == StyleLength::UnitType::Px)
        YGNodeStyleSetMinHeight(node, lay.MinHeight.Value);
    else
        YGNodeStyleSetMinHeight(node, YGUndefined);

    if (lay.MaxWidth.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetMaxWidthPercent(node, lay.MaxWidth.Value);
    else if (lay.MaxWidth.Unit == StyleLength::UnitType::Px)
        YGNodeStyleSetMaxWidth(node, lay.MaxWidth.Value);
    else
        YGNodeStyleSetMaxWidth(node, YGUndefined);

    if (lay.MaxHeight.Unit == StyleLength::UnitType::Percent)
        YGNodeStyleSetMaxHeightPercent(node, lay.MaxHeight.Value);
    else if (lay.MaxHeight.Unit == StyleLength::UnitType::Px)
        YGNodeStyleSetMaxHeight(node, lay.MaxHeight.Value);
    else
        YGNodeStyleSetMaxHeight(node, YGUndefined);

    if (lay.AspectRatio > 0.0f)
        YGNodeStyleSetAspectRatio(node, lay.AspectRatio);
    else
        YGNodeStyleSetAspectRatio(node, YGUndefined);
#else
    (void)node; (void)style;
#endif
}

void YogaAdapter::CalculateLayout(YGNodeRef root, float width, float height) {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!root) return;
    YGNodeCalculateLayout(root, width, height, YGDirectionLTR);
#else
    (void)root; (void)width; (void)height;
#endif
}

void YogaAdapter::MarkDirtyAndPropagate(YGNodeRef node) {
    AssertNotCascadeCompute();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (node) facebook::yoga::resolveRef(node)->markDirtyAndPropagate();
#else
    (void)node;
#endif
}

void YogaAdapter::InsertChildrenSortedByOrder(YGNodeRef parent, std::span<const ChildWithOrder> children) {
    AssertNotCascadeCompute();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!parent) return;
    // Parent/child relationships may be rebuilt across frames (retained Yoga nodes).
    // Rebuilding the child list for every node every frame is extremely expensive,
    // so we early-out when the current Yoga child list already matches.

    auto isSortedByOrderThenOriginal = [&](std::span<const ChildWithOrder> v) -> bool
    {
        for (size_t i = 1; i < v.size(); ++i)
        {
            const auto& a = v[i - 1];
            const auto& b = v[i];
            if (a.order > b.order)
                return false;
            if (a.order == b.order && a.originalIndex > b.originalIndex)
                return false;
        }
        return true;
    };

    std::span<const ChildWithOrder> desired = children;
    // PERF: avoid per-call allocations. This function is called for many nodes per frame.
    static thread_local std::vector<ChildWithOrder> sorted;
    if (!isSortedByOrderThenOriginal(children))
    {
        sorted.clear();
        sorted.reserve(children.size());
        sorted.insert(sorted.end(), children.begin(), children.end());
        std::stable_sort(sorted.begin(), sorted.end(), [](const ChildWithOrder& a, const ChildWithOrder& b) {
            if (a.order != b.order) return a.order < b.order;
            return a.originalIndex < b.originalIndex;
        });
        desired = sorted;
    }

    const uint32_t curCount = (uint32_t)YGNodeGetChildCount(parent);
    {
        // Fast compare without allocations: walk desired list, skipping null nodes.
        uint32_t wantIndex = 0;
        bool match = true;
        for (const auto& c : desired)
        {
            if (!c.node)
                continue;
            if (wantIndex >= curCount || YGNodeGetChild(parent, wantIndex) != c.node)
            {
                match = false;
                break;
            }
            ++wantIndex;
        }
        if (match && wantIndex == curCount)
            return;
    }

    // Fallback: rebuild parent->children set.
    while (YGNodeGetChildCount(parent) > 0)
    {
        YGNodeRef c = YGNodeGetChild(parent, 0);
        if (!c)
            break;
        YGNodeRemoveChild(parent, c);
    }

    uint32_t insertIndex = 0;
    for (const auto& c : desired)
    {
        if (!c.node)
            continue;
        YGNodeRef node = c.node;
        // Detach from any previous parent so a node never has multiple parents.
        if (YGNodeRef prev = YGNodeGetParent(node))
        {
            if (prev != parent)
                YGNodeRemoveChild(prev, node);
        }
        YGNodeInsertChild(parent, node, insertIndex++);
    }
#else
    (void)parent; (void)children;
#endif
}

}} // namespace GameEngine::UILayout
