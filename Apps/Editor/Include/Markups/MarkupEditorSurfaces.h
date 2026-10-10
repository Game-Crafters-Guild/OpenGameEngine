#pragma once

#include "Markups/MarkupEditorBridge.h"

#include <memory>

namespace GameEngine::Editor
{
class EditorChangeNotifications;

// The editor's mark-ups: the bridge (the system clock, the open scene's file from
// `scenePath`, installed as TryGet's; the project's state is loaded once the engine knows
// the project, LoadProjectState) and what it registers with the editor's registries: the
// Scene View tool strip entry with its Activity badge, the Mark-ups panel, the
// Markup inspector section and the Scene View's labels (MarkupLabelOverlay).
std::unique_ptr<MarkupEditorBridge> CreateMarkupEditorBridge(EditorChangeNotifications& notifications,
                                                             MarkupEditorBridge::ScenePath scenePath);

} // namespace GameEngine::Editor
