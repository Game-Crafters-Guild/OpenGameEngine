// One item resize step on an editor tree. The Hierarchy and the Assets tree share it, so both
// trees answer the gesture the same way: wheel up grows the icons one pixel per wheel detent,
// within each tree's own range.

#include <gtest/gtest.h>

#include "Editor/EditorTreeTitleIconVars.h"

using GameEngine::EditorTreeIconSizeAfterResizeGesture;
using GameEngine::kMaxEditorAssetsTreeIconSizePx;
using GameEngine::kMaxEditorHierarchyTreeIconSizePx;
using GameEngine::kMinEditorTreeIconSizePx;

namespace
{
// The UI delivers a wheel detent as ~30 px of scroll; wheel up is negative.
constexpr float kWheelUpOneDetent = -30.0f;
constexpr float kWheelDownOneDetent = 30.0f;
} // namespace

TEST(EditorTreeResizeGesture, WheelUpGrowsAndWheelDownShrinksOnePixelPerDetent)
{
    EXPECT_FLOAT_EQ(EditorTreeIconSizeAfterResizeGesture(20.0f, kWheelUpOneDetent,
                                                         kMaxEditorHierarchyTreeIconSizePx),
                    21.0f);
    EXPECT_FLOAT_EQ(EditorTreeIconSizeAfterResizeGesture(20.0f, kWheelDownOneDetent,
                                                         kMaxEditorHierarchyTreeIconSizePx),
                    19.0f);
}

TEST(EditorTreeResizeGesture, StaysInsideEachTreeRange)
{
    EXPECT_FLOAT_EQ(EditorTreeIconSizeAfterResizeGesture(kMinEditorTreeIconSizePx, kWheelDownOneDetent,
                                                         kMaxEditorHierarchyTreeIconSizePx),
                    kMinEditorTreeIconSizePx);
    EXPECT_FLOAT_EQ(EditorTreeIconSizeAfterResizeGesture(kMaxEditorHierarchyTreeIconSizePx, kWheelUpOneDetent,
                                                         kMaxEditorHierarchyTreeIconSizePx),
                    kMaxEditorHierarchyTreeIconSizePx);
    EXPECT_FLOAT_EQ(EditorTreeIconSizeAfterResizeGesture(kMaxEditorAssetsTreeIconSizePx, kWheelUpOneDetent,
                                                         kMaxEditorAssetsTreeIconSizePx),
                    kMaxEditorAssetsTreeIconSizePx)
        << "the Assets tree stops at its own, smaller maximum";
}
