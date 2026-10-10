#pragma once

#include "InspectorRegistry.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewTools.h"
#include "UI/Interaction/ContextMenuManipulator.h"

#include <nlohmann/json.hpp>

#include <optional>

#include <span>
#include <vector>

namespace GameEngine
{
class SceneViewController;
namespace ECS
{
class World;
}
namespace Mathematics
{
struct Ray3D;
}
} // namespace GameEngine

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class MarkupEditorBridge;
class UndoRedoService;

// What the Mark-up tool places: a box or a sphere at a click, or a region drawn as an outline. The
// tool strip entry's right-click menu chooses it; every Scene View's tool shares the choice.
enum class MarkupToolShape
{
    Box,
    Sphere,
    Region,
};
MarkupToolShape GetMarkupToolShape();
void SetMarkupToolShape(MarkupToolShape shape);
// The tool strip entry's right-click menu: the three shapes, the chosen one checked.
std::vector<ContextMenuManipulator::Item> BuildMarkupToolMenuItems(const OpenColorPickerWindowFn& openColorPicker);

// The mark-up the tool places on `ground` (a box or a sphere as `shape`; status Requested, by the
// user, one undo step), through markup_create's path: its result, or a refusal while the editor
// plays.
nlohmann::json PlaceMarkup(MarkupEditorBridge& bridge, ECS::World& world, UndoRedoService* undo,
                           EditorChangeNotifications* notifications, const Mathematics::Vector3& ground,
                           MarkupToolShape shape = MarkupToolShape::Box);
// The region the tool draws over `knots` (world x, z, in order around the area; status Requested,
// by the user, one undo step), through markup_create's path: its result, or a refusal.
nlohmann::json PlaceRegion(MarkupEditorBridge& bridge, ECS::World& world, UndoRedoService* undo,
                           EditorChangeNotifications* notifications, std::span<const Mathematics::Vector2> knots);

// Where a Region-shape press, drag or click on `ray` lands: the terrain under the pointer (the
// terrain only: a region's knots are ground, so a road, a house or a mesh whose picking is still
// being prepared neither takes the click nor drops it), else the ground plane y = 0; nullopt for
// a ray that meets neither (the sky).
std::optional<Mathematics::Vector3> ResolveMarkupRegionGround(const Mathematics::Ray3D& ray, ECS::World& world);

// A lasso stroke (the pointer's ground samples, at least 1 m apart: CaptureSplineStroke) as a
// region's knots: the loop closed from its last sample back to its first, simplified by
// Douglas-Peucker at 1 % of its bounding diagonal (0.5 to 5 m), the tolerance doubled until at
// most MarkupECS::kMaxRegionKnots remain. So a 200 m lasso keeps tens of knots, not hundreds.
std::vector<Mathematics::Vector2> SimplifyLassoStroke(std::span<const Mathematics::Vector3> stroke);

// The Scene View's mark-up tool, entered from the tool strip. Box and Sphere: a click on the world
// places one there, selects it and returns to the transform tool, whose handles move and size it.
// Region: a drag paints a lasso, closed where it is released; clicks place knots instead, closed by
// Enter or a click on the first knot; Escape cancels. An outline that crosses itself is refused
// with a notice. The new region is selected and the spline tool shows its knots. One undo step
// per mark-up; a mark-up is always placed at the scene root, never inside a blueprint instance.
class MarkupTool final : public SceneTools::ISceneTool, public SceneTools::IGizmo
{
public:
    MarkupTool(SceneViewController& owner, MarkupEditorBridge& bridge);

    const char* GetName() const override { return "Mark-up"; }
    void OnPointerEvent(const SceneTools::ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneTools::SceneKeyEvent& event) override;
    void OnDeactivated() override { CancelRegion(); }
    void GatherGizmos(SceneTools::GizmoCollector& collector) override;
    // The outline being drawn: the lasso's samples, or the clicked knots and the pointer.
    void Render(SceneTools::GizmoRenderContext& context) override;

private:
    void PlaceAt(const Mathematics::Vector3& ground);
    // Region mode's pointer: a press starts a stroke, a drag paints it, a release ends it (a
    // lasso) or places a knot (a click).
    void DrawRegion(const SceneTools::ScenePointerEvent& event, const Mathematics::Vector3& ground);
    // Whether `point` is within a click of the first knot on screen.
    bool NearFirstKnot(const SceneTools::ScenePointerEvent& event, const Mathematics::Vector3& point) const;
    // Makes the region over `knots`; a crossing outline is refused with a notice.
    void FinishRegion(const std::vector<Mathematics::Vector2>& knots);
    void CancelRegion();

    SceneViewController& m_Owner;
    MarkupEditorBridge& m_Bridge;
    std::vector<Mathematics::Vector3> m_Stroke; // the lasso's samples while the button is down
    std::vector<Mathematics::Vector3> m_Knots;  // knots placed by clicks
    Mathematics::Vector3 m_Pointer{};           // the pointer on the ground, for the preview
    bool m_Pressed = false;
    bool m_Dragged = false;
};

} // namespace GameEngine::Editor
