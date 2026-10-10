#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
class UIManager;
} // namespace GameEngine

namespace GameEngine::Editor
{

// What a drop performed for the debug server came to.
struct DropOutcome
{
    // Why the request was refused before any drop: a file outside the asset sources or
    // missing, or a drag-and-drop session already in progress. Empty when the drop ran.
    std::string Refusal;
    // The target under the point accepted the payload, and its PerformDrop ran. PerformDrop
    // reports no result, so this does not say the drop succeeded.
    bool Accepted = false;
    // The target's refusal, or why no target was found; empty when accepted.
    std::string Reason;
    // The drop target element: its id, or "element#<instance id>" when it has none; empty
    // when no target was under the point.
    std::string TargetElement;
};

// Drops the asset files `paths` (absolute) at (x, y), in `ui`'s window coordinates (the
// logical pixels get_ui_tree reports), through its drag-and-drop manager: the pointer moves
// there, the hover resolves the drop target, and the session commits as a pointer drag ends,
// so the target's own CanDrop and PerformDrop run, as they do for a drop with the mouse.
//
// Refuses, before any drop, an empty `paths`, and a path that is not an existing file or
// folder under one of `sourceRoots` (the asset sources' roots) once links are resolved: a drop
// target may move what it is given, as the Assets panel does, so a path from elsewhere on the
// machine, or reached through a junction that leaves a source, would be moved into the project.
// Also refuses while a drag-and-drop session is in progress, which a drop would replace.
// Main thread, outside UI event dispatch.
DropOutcome PerformAssetFileDrop(UIManager& ui, const std::vector<std::filesystem::path>& paths,
                                 const std::vector<std::filesystem::path>& sourceRoots, float x, float y);

} // namespace GameEngine::Editor
