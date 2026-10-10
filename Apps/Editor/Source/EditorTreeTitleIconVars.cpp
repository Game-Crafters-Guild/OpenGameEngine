#include "Editor/EditorTreeTitleIconVars.h"

#include "Types/StringId.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GameEngine
{
namespace
{
constexpr float kTreeRowCompactIconEndPx = 20.0f;
constexpr float kTreeRowExtraAtMaxIconPx = 4.0f;
// The UI's wheel scroll is ~30 px per detent, so rounding this rate moves the icon one
// pixel per detent.
constexpr float kTreeIconPxPerScrollPx = 1.0f / 40.0f;

std::string FormatTreeIconPx(float px)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0fpx", std::round(px));
    return buffer;
}
} // namespace

void ApplyTreeTitleIconLayoutVars(UIElement* scope, float iconSizePx)
{
    if (!scope)
        return;
    const float clamped =
        std::clamp(iconSizePx, kMinEditorTreeIconSizePx, kMaxEditorHierarchyTreeIconSizePx);
    const float padding = clamped + 12.0f;
    const float paddingRtl = clamped + 4.0f;
    const float foldoutPx = TreeFoldoutGlyphSizePxForTreeIconSize(clamped);
    scope->Overrides().SetCustom(HashStringId("--ui_tree_icon_size"), FormatTreeIconPx(clamped));
    scope->Overrides().SetCustom(HashStringId("--ui_tree_foldout_size"), FormatTreeIconPx(foldoutPx));
    scope->Overrides().SetCustom(HashStringId("--ui_tree_icon_padding"), FormatTreeIconPx(padding));
    scope->Overrides().SetCustom(HashStringId("--ui_tree_icon_padding_rtl"), FormatTreeIconPx(paddingRtl));
    if (UIManager* ui = scope->GetOwnerManager())
        ui->MarkStyleDirtySubtree(scope);
}

float DeriveEditorTreeRowHeightFromIconSize(float iconSizePx)
{
    const float icon =
        std::clamp(iconSizePx, kMinEditorTreeIconSizePx, kMaxEditorHierarchyTreeIconSizePx);
    if (icon <= kTreeRowCompactIconEndPx)
        return std::clamp(icon, kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
    const float span = kMaxEditorHierarchyTreeIconSizePx - kTreeRowCompactIconEndPx;
    const float t = (icon - kTreeRowCompactIconEndPx) / span;
    const float extra = kTreeRowExtraAtMaxIconPx * t;
    return std::clamp(icon + extra, kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
}

float EditorTreeIconSizeAfterResizeGesture(float iconSizePx, float scrollY, float maxIconSizePx)
{
    const float resized = std::round(iconSizePx - scrollY * kTreeIconPxPerScrollPx);
    return std::clamp(resized, kMinEditorTreeIconSizePx, maxIconSizePx);
}
} // namespace GameEngine
