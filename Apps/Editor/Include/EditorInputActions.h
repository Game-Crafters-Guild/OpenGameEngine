#pragma once

#include "Input/KeyCodes.h"
#include "Input/InputSystem.h"

namespace GameEngine {
namespace EditorInput {

// Global editor context (window-agnostic editor shortcuts).
inline constexpr Input::ContextId kEditorGlobalContext = Input::HashInput("Editor.Global");

// Global editor actions (tied to the active Scene View for now).
inline constexpr Input::ActionId kEditorToggleGizmos   = Input::HashInput("Editor.Global.ToggleGizmos");
inline constexpr Input::ActionId kEditorFrameSelection = Input::HashInput("Editor.Global.FrameSelection");
inline constexpr Input::ActionId kEditorFrameAll       = Input::HashInput("Editor.Global.FrameAll");
inline constexpr Input::ActionId kEditorSaveScene      = Input::HashInput("Editor.Global.SaveScene");
inline constexpr Input::ActionId kEditorSaveSceneAs    = Input::HashInput("Editor.Global.SaveSceneAs");
inline constexpr Input::ActionId kEditorUniversalSearch = Input::HashInput("Editor.Global.UniversalSearch");
inline constexpr Input::ActionId kEditorTogglePlayMode = Input::HashInput("Editor.Global.TogglePlayMode");
// Discard Change Review (Escape). Does not leave play — that is Ctrl/Cmd+P,
// the toolbar Stop button, or Escape while play is fullscreen (which has no
// toolbar). During play this fires only when the running game claims neither
// Escape nor the chrome above it: the game's sink answers first.
inline constexpr Input::ActionId kEditorDiscardPlayReview =
    Input::HashInput("Editor.Global.DiscardPlayReview");
inline constexpr Input::ActionId kEditorToggleWindowFullscreen =
    Input::HashInput("Editor.Global.ToggleWindowFullscreen");

#ifdef __APPLE__
inline constexpr int kUniversalSearchDefaultMods = Input::kModSuper;
#else
inline constexpr int kUniversalSearchDefaultMods = Input::kModControl;
#endif

inline void RegisterUniversalSearchShortcut(Input::InputSystem& input)
{
    Input::ActionBinding binding{Input::DeviceType::Keyboard, Input::kKeyCode_K, 1.0f};
    binding.requiredMods = kUniversalSearchDefaultMods;
    binding.forbiddenMods = Input::kModShortcutMask & ~binding.requiredMods;
    input.RegisterAction(kEditorGlobalContext,
                         {kEditorUniversalSearch, {binding}, false, /*hostReserved=*/true});
}

// Context for all Scene View-related input.
inline constexpr Input::ContextId kSceneViewContext = Input::HashInput("Editor.SceneView");

// Context for Game View input (enabled only while playing and Game View is focused).
inline constexpr Input::ContextId kGameViewContext = Input::HashInput("Editor.GameView");

// Scene View movement actions (WASD + Q/E while RMB look is active).
inline constexpr Input::ActionId kSceneViewMoveForward  = Input::HashInput("Editor.SceneView.MoveForward");
inline constexpr Input::ActionId kSceneViewMoveBackward = Input::HashInput("Editor.SceneView.MoveBackward");
inline constexpr Input::ActionId kSceneViewMoveLeft     = Input::HashInput("Editor.SceneView.MoveLeft");
inline constexpr Input::ActionId kSceneViewMoveRight    = Input::HashInput("Editor.SceneView.MoveRight");
inline constexpr Input::ActionId kSceneViewMoveUp       = Input::HashInput("Editor.SceneView.MoveUp");
inline constexpr Input::ActionId kSceneViewMoveDown     = Input::HashInput("Editor.SceneView.MoveDown");
inline constexpr Input::ActionId kSceneViewMoveFaster    = Input::HashInput("Editor.SceneView.MoveFaster");

// Scene View camera look axes (driven by mouse delta while viewport captures RMB/MMB).
inline constexpr Input::ActionId kSceneViewLookYaw   = Input::HashInput("Editor.SceneView.Look.Yaw");
inline constexpr Input::ActionId kSceneViewLookPitch = Input::HashInput("Editor.SceneView.Look.Pitch");

/// Register every default editor action/shortcut, apply user shortcut overrides
/// from preferences, and push the editor input contexts.
void RegisterEditorInputActions(Input::InputSystem& input);

} // namespace EditorInput
} // namespace GameEngine
