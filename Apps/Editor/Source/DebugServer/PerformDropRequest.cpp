#include "DebugServer/PerformDropRequest.h"

#include "Assets/AssetPathKey.h"
#include "Editor/DragDropPayloads.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/Payload.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <string>
#include <system_error>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

// Compares the resolved forms, so a junction or symbolic link inside an asset source that
// points outside it does not count as inside: a drop target would move the files it reaches.
bool IsUnderAnySourceRoot(const std::filesystem::path& path, const std::vector<std::filesystem::path>& sourceRoots)
{
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return false;
    for (const std::filesystem::path& root : sourceRoots)
    {
        const std::filesystem::path resolvedRoot = std::filesystem::weakly_canonical(root, ec);
        if (!ec && IsUnderAssetDir(resolvedRoot, resolved))
            return true;
    }
    return false;
}

std::string RefusePath(const std::filesystem::path& path, const std::vector<std::filesystem::path>& sourceRoots)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return "No file or folder at " + path.generic_string() + ": pass an existing asset file";
    if (IsUnderAnySourceRoot(path, sourceRoots))
        return {};
    std::string roots;
    for (const std::filesystem::path& root : sourceRoots)
        roots += (roots.empty() ? "" : ", ") + root.generic_string();
    return path.generic_string() + " is outside the asset sources (" + roots +
           "): perform_drop drops files that are already in one of them, as a drag from the Assets "
           "panel does. Copy the file under one of them first.";
}

} // namespace

DropOutcome PerformAssetFileDrop(UIManager& ui, const std::vector<std::filesystem::path>& paths,
                                 const std::vector<std::filesystem::path>& sourceRoots, float x, float y)
{
    DropOutcome outcome;
    UI::Interaction::DragDropManager* dragDrop = ui.GetDragDropManager();
    if (!dragDrop)
    {
        outcome.Refusal = "the window has no drag-and-drop manager";
        return outcome;
    }
    if (dragDrop->IsDragging())
    {
        outcome.Refusal = "a drag-and-drop session is in progress in this window, and a drop would replace it: "
                          "retry once it ends";
        return outcome;
    }
    if (paths.empty())
    {
        outcome.Refusal = "no paths to drop: pass at least one asset file";
        return outcome;
    }
    AssetPathsDragPayload files;
    for (const std::filesystem::path& path : paths)
    {
        const std::filesystem::path normal = path.lexically_normal();
        outcome.Refusal = RefusePath(normal, sourceRoots);
        if (!outcome.Refusal.empty())
            return outcome;
        files.paths.push_back(normal);
    }
    files.displayLabel = files.paths.front().filename().string();
    UI::Interaction::DragPayload payload = UI::Interaction::DragPayload::Create(files);
    payload.DisplayLabel = files.displayLabel;
    payload.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;

    // As the native file drop does: settle the hover at the point, then hand the session
    // the hovered element to resolve its drop target from.
    ui.OnMouseMove(x, y);
    ui.Update(0.0f, /*interactive=*/true);
    dragDrop->BeginDrag(std::move(payload));
    dragDrop->UpdateHover(ui.GetHoveredElement(), x, y, /*mods=*/0);
    outcome.Accepted = dragDrop->GetCurrentFeedback().Allowed;
    if (UIElement* target = dragDrop->GetCurrentTargetElement())
        outcome.TargetElement =
            target->GetId().empty() ? "element#" + std::to_string(target->GetInstanceId()) : target->GetId();
    if (!outcome.Accepted)
    {
        outcome.Reason = dragDrop->GetCurrentFeedback().Reason;
        if (outcome.Reason.empty())
            outcome.Reason = outcome.TargetElement.empty() ? "no drop target under the point"
                                                           : "the drop target refused the payload";
    }
    dragDrop->CommitDrop(/*mods=*/0);
    return outcome;
}

} // namespace GameEngine::Editor
