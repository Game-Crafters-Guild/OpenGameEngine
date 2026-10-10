#pragma once

#include <functional>

namespace GameEngine
{
class SceneViewController;

namespace Editor
{

/// Installs the editor's entity-link actions (EditorUI::SetEditorEntityLinkActions) for
/// surfaces with no selection of their own: a link's name selects its entity through the
/// Scene View's pick, its frame glyph also frames it, and hovering it highlights the entity
/// as a panel hover. `sceneView` returns the main Scene View, or null when there is none.
void InstallSceneViewEntityLinks(std::function<SceneViewController*()> sceneView);

/// Removes them (the editor is going away).
void UninstallSceneViewEntityLinks();

} // namespace Editor
} // namespace GameEngine
