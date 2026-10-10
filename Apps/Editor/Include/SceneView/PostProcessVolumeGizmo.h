#pragma once

namespace GameEngine::Editor::SceneTools
{

// Register the Scene View gizmo of a local (non-global) post process volume with
// ComponentGizmoRegistry: a selected or hovered volume draws its shape as a wireframe, and its blend
// region as a dimmer outer shape. Called by the volume's inspector registration.
void RegisterPostProcessVolumeGizmo();

} // namespace GameEngine::Editor::SceneTools
