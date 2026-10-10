// Leaf-dep stubs for EditorTests.
//
// SelectionTool::OnPointerEvent calls SceneViewController::OnEntityPicked, which lives in
// SceneViewController.cpp with UI-panel deps. EditorTests exercise the tool math, not the
// controller, so no-op stubs at link time are sufficient. The picking entry points are NOT
// stubbed — ScenePickTerrainInclusionTests compiles the real MeshPickingService in and
// exercises them.

#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "SceneViewController.h"
#include "UI/Controls/ScenePostFxMenu.h"
#include "Mathematics/Vector3.h"

namespace GameEngine
{
    void SceneViewController::OnEntityPicked(ECS::EntityHandle, bool, bool, bool) {}
    void SceneViewController::FrameOrigin() {}
    void SceneViewController::SetHoverEntity(ECS::EntityHandle, bool, HoverEntitySource) {}
}

namespace GameEngine
{
    // Stub for the lock-icon scene-pick blocker. Tests don't exercise
    // hierarchy lock state, so always-pickable is sufficient.
    bool SceneViewController::IsEntityPickable(GameEngine::ECS::EntityHandle) const
    {
        return true;
    }

    // Stub for the click-through pick (real impl in SceneViewController.cpp
    // with live-picking deps). Tests don't exercise scene-view clicks.
    Editor::SceneTools::ScenePickOutcome SceneViewController::PickViaClickThrough(
        const Editor::SceneTools::ScenePointerEvent&, bool)
    {
        return {};
    }

    // The out-of-line members SceneViewToolbar's click handlers and menu-item
    // actions reference. SceneViewToolbarWiringTests exercises which handler is
    // bound, never a live controller and never an open menu, so the toolbar's
    // controller stays null and these are unreachable.
    void SceneViewController::TogglePostProcessing() {}
    void SceneViewController::Toggle2DMode() {}
    void SceneViewController::SetCameraFrameGuide(bool) {}
    void SceneViewController::RefreshPixelPerfect2D() {}

    // The Mark-up tool returns its view to the transform tool after a placement; the
    // mark-up tests drive PlaceMarkup, never a live view.
    void SceneViewController::SetActiveTool(ToolKind) {}
    // ...and hands a new or edited region to the spline tool; the tests drive PlaceRegion and
    // the inspector's rows, never a live view.
    bool SceneViewController::SetActiveRegisteredTool(std::string_view) { return false; }

    // SetCameraPose's out-of-line steps, behind the Mark-ups panel's frame: the mark-up
    // tests check the pose the frame computes, never a live view.
    void SceneViewController::Sync2DModeToTransformTool() {}
    void SceneViewController::UpdateOrbit(bool) {}
    // The mark-up labels read the camera through it; their tests hand Collect a camera.
    void SceneViewController::PopulatePointerCameraState(Editor::SceneTools::ScenePointerEvent&) const {}
}

namespace GameEngine::Editor
{
    // The Mark-up tool's play-mode notice (real impl in SceneViewNoticeOverlay.cpp, a view
    // overlay); the mark-up tests read the refusal instead.
    void ShowSceneViewNotice(std::string) {}
}

namespace GameEngine::Editor
{
    // Stub for the post-process menu's item builder (real impl in
    // ScenePostFxMenu.cpp, which walks the ECS world). The wiring tests never
    // open the menu the toolbar declares with it.
    std::vector<ContextMenuManipulator::Item> BuildScenePostFxMenuItems(
        SceneViewController*, SceneViewToolbar*, std::function<void()>)
    {
        return {};
    }
}
