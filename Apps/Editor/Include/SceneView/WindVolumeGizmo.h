#pragma once

namespace GameEngine::Editor::SceneTools
{

// Register the Scene View gizmo of a local (non-global) wind volume with ComponentGizmoRegistry: a
// selected or hovered volume draws its shape as a wireframe, and its blend region as a dimmer outer
// shape. Called by the volume's inspector registration.
void RegisterWindVolumeGizmo();

} // namespace GameEngine::Editor::SceneTools
