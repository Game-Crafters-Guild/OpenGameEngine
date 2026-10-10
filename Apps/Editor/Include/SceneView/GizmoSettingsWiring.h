#pragma once

namespace GameEngine
{

class SceneViewController;
class SettingsPanel;

namespace Editor
{
namespace SceneTools
{

// Helper to wire SettingsPanel gizmo callbacks to TransformTool.
// This keeps feature-specific wiring out of EditorApplication.cpp.
namespace GizmoSettingsWiring
{
    // Wire all gizmo-related callbacks from SettingsPanel to the TransformTool
    // owned by the given SceneViewController.
    // Both parameters must be non-null.
    void WireGizmoSettings(SettingsPanel* settingsPanel,
                           SceneViewController* sceneController);

} // namespace GizmoSettingsWiring

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
