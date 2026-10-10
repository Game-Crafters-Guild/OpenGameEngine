#pragma once

namespace GameEngine
{
class UIElement;

constexpr float kMinEditorTreeIconSizePx = 16.0f;
constexpr float kMaxEditorHierarchyTreeIconSizePx = 64.0f;
constexpr float kMaxEditorAssetsTreeIconSizePx = 32.0f;
constexpr float kMinEditorTreeRowHeightPx = 16.0f;
constexpr float kMaxEditorTreeRowHeightPx = 80.0f;

/** Sets --ui_tree_icon_* custom properties on \p scope so `.tree-title` folder icons match TreeView icon size. */
void ApplyTreeTitleIconLayoutVars(UIElement* scope, float iconSizePx);

/** Row height derived from icon size when Settings keeps icon and row in sync (hierarchy up to 64 px icon). */
float DeriveEditorTreeRowHeightFromIconSize(float iconSizePx);

/** Tree icon size after one item resize step: wheel up (negative scrollY) grows it, one px
    per wheel detent, clamped to [kMinEditorTreeIconSizePx, maxIconSizePx]. */
float EditorTreeIconSizeAfterResizeGesture(float iconSizePx, float scrollY, float maxIconSizePx);
} // namespace GameEngine
