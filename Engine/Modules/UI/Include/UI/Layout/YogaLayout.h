#pragma once

#include "UI/ResolvedStyle.h"
#include <span>
#include <vector>

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

namespace GameEngine
{
namespace UILayout
{

#if !defined(GE_HAVE_YOGA) || !GE_HAVE_YOGA
// Forward-declare type to avoid including Yoga when unavailable
struct YGNode;
using YGNodeRef = YGNode*;
#endif

// Thin adapter for mapping ResolvedStyle to Yoga nodes
struct YogaAdapter
{
    static bool IsAvailable();
    static YGNodeRef CreateNode();
    static void DestroyNode(YGNodeRef node);
    static void ApplyStyle(YGNodeRef node, const ResolvedStyle& style);
    static void CalculateLayout(YGNodeRef root, float width, float height);
    static void MarkDirtyAndPropagate(YGNodeRef node);

    // The content scale every subsequent solve rounds against. Yoga rounds each
    // solved position and dimension onto a grid of 1 / pointScaleFactor LOGICAL
    // px; this maps the content scale onto Chrome's layout quantum, 1/64 of a
    // DEVICE pixel.
    //
    // The grid has to be finer than a logical pixel, because the values reaching
    // it are not whole logical pixels:
    //   - text is sized in DEVICE px — FontAtlas rounds ascent, descent and line
    //     gap to whole device pixels (ExactLineMetricsWithFace), as Blink does —
    //     so Roboto at font-size 16px and content scale 2 is a 43-device-px line
    //     box, i.e. 21.5 logical. Yoga's default factor of 1 rounds that to 22
    //     logical and the box comes out a device pixel too tall;
    //   - CSS lengths are not whole device pixels either. A 15px border plus
    //     padding at content scale 1.5 is 22.5 device px, and Chrome keeps the
    //     half (BorderPaintPrimitiveTests pins it), so a whole-DEVICE-pixel grid
    //     is also too coarse.
    //
    // Applies to the whole tree: the grid is read per node while rounding, so
    // nodes on different grids would round the levels of one tree
    // inconsistently. Set it before the solve, not during.
    static void SetContentScale(float contentScale);

    struct ChildWithOrder
    {
        YGNodeRef node;
        int order;
        size_t originalIndex;
    };
    // Stable: preserves originalIndex order when 'order' values are equal
    static void InsertChildrenSortedByOrder(YGNodeRef parent, std::span<const ChildWithOrder> children);
};

// MT-4.0 cascade seam guard. UIManager marks the cascade-compute region
// (ResolveCascadeForElement + FinalizeElementStyle) active while it runs;
// the YogaAdapter mutators assert they are never invoked inside it. The seam
// keeps the compute stage Yoga-free so it stays a pure, side-effect-free
// element-local cascade (collect / compute / apply separation) — this catches a
// regression the moment it is introduced. thread_local so the state is
// per-thread. Compiles to a no-op outside true Debug builds (matches
// UIManager::AssertUiThread's _DEBUG gating).
void SetCascadeComputeActive(bool active);
bool CascadeComputeActive();

} // namespace UILayout
} // namespace GameEngine
