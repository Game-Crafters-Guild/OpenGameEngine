#pragma once

#include "SceneViewTools.h"
#include "SceneViewGizmos.h"
#include "SelectionTool.h"
#include "UndoRedo/UndoRedoService.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "SplineECS/SplineService.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace GameEngine::Editor { class EditorChangeNotifications; }
namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor::SceneTools
{
class TransformTool;

// One selected spline control. The entity is part of the key so selected point
// indices from different splines do not alias each other.
struct SplineControlSelection
{
    ECS::EntityHandle Entity{};
    int32 PointIndex = -1;
    int32 Kind = 0; // 1 = knot, 2 = handle in, 3 = handle out
};

// Per-spline selection state for the editor's currently selected spline entity.
// Shared between SplineTool (which writes it on pointer events) and
// SplineSceneGizmo (which reads it to highlight the selected knot/handle).
// Kind: 0 = none, 1 = knot, 2 = handle in, 3 = handle out.
struct SplineSelection
{
    ECS::EntityHandle Entity{};
    int32 PointIndex = -1;
    int32 Kind = 0;
    // Multi-selection produced by a marquee/lasso drag while the tool is in
    // selection mode. Contains knot indices for the same Entity. Empty when
    // only the single (Kind/PointIndex) selection is active.
    std::vector<int32> KnotIndices;
    // Cross-spline control selection. This is used for shift/ctrl picking
    // controls on multiple selected splines and for marquee selections.
    std::vector<SplineControlSelection> Controls;
};


// Hover state: tracks which knot/handle the cursor is over (0 = nothing).
// SplineSceneGizmo reads this to draw a highlight ring under the cursor.
struct SplineHover
{
    ECS::EntityHandle Entity{};
    int32 PointIndex = -1;
    int32 Kind = 0; // 0=none, 1=knot, 2=handleIn, 3=handleOut
};

class SplineInteractionState
{
public:
    explicit SplineInteractionState(ECS::World& world);
    SplineSelection& Selection();
    SplineHover& Hover();
    ECS::World& World() const { return m_World; }

private:
    void Refresh();
    ECS::World& m_World;
    uint64 m_Generation;
    SplineSelection m_Selection;
    SplineHover m_Hover;
};

std::shared_ptr<SplineInteractionState> AcquireSplineInteractionState(ECS::World& world);

// Matches translate-gizmo scaling: world scale grows with camera distance so
// radii/thickness track a stable on-screen size. Returns 1 when disabled or
// when camera position is unavailable.
//
// When smartDistanceScalingEnabled is true (and constant screen is on), applies
// a sqrt falloff vs. pure linear distance so handles stay readable at very
// close zoom without growing unbounded when far away.
float ComputeSplineScreenScale(const GizmoRenderContext& context,
                               const Mathematics::Vector3& worldPos,
                               bool constantScreenSizeEnabled,
                               bool smartDistanceScalingEnabled);

// Pick-radius scale for a knot or handle at worldPosition: ComputeSplineScreenScale's
// perspective path with the ray origin as the camera, so the pick area matches the
// drawn marker. Pass the renderer's constant screen size and smart distance
// scaling settings.
float32 PickScreenScale(const GizmoRay& ray,
                        const Mathematics::Vector3& worldPosition,
                        bool constantScreenSizeEnabled,
                        bool smartDistanceScalingEnabled);

// Plane normal for knot/handle wire circles: world +Y in 3D; camera-facing in editor 2D view.
Mathematics::Vector3 SplineCirclePlaneNormal(const GizmoRenderContext& context,
                                             const Mathematics::Vector3& worldPos);

// Knot or handle marker in the shape SplineEditorSettings selects: a solid sphere
// or cube half `radius` in size, or a DrawThickWireCircle ring of `radius` around
// SplineCirclePlaneNormal(context, pos). With a ringBatch, a ring is appended to it
// as triangles (AppendThickWireCircleTriangles) for the caller to submit in `color`.
void DrawSplineControlMarker(GizmoRenderContext& context,
                             const Mathematics::Vector3& pos,
                             float32 radius,
                             const Color& color,
                             float32 thickness,
                             bool constantScreen,
                             bool smartDistance,
                             std::vector<Mathematics::Vector3>* ringBatch);

// Gizmo that renders the active spline-tool stroke preview (stroke-in-progress
// and/or freshly clicked points that have not yet been committed to an entity).
class SplineGizmo : public IGizmo
{
public:
    void SetPoints(const std::vector<Mathematics::Vector3>& points, float32 radius = 2.0f);
    void SetSelectedIndex(int32 index) { m_SelectedIndex = index; }
    void SetActive(bool active) { m_Active = active; }
    bool IsActive() const { return m_Active; }
    void SetBrushMode(bool brush) { m_BrushMode = brush; }

    // When active, the gizmo renders a "will close loop on release" preview:
    // a dashed segment from the last point back to the first and a highlight
    // ring around the start. SplineTool turns this on mid-stroke whenever the
    // current cursor is within the close-loop tolerance of the stroke start.
    void SetCloseLoopHint(bool active) { m_CloseLoopHint = active; }

    // Marquee preview drawn while the user is mid drag-select, through a
    // MarqueeGizmo in its default colour and thickness. shape: 0 = rect,
    // 1 = lasso. The lasso polyline / rect corners are in world space and lie
    // on the click plane chosen at marquee start.
    void SetMarquee(bool active,
                    int32 shape,
                    const Mathematics::Vector3& start,
                    const Mathematics::Vector3& current,
                    const std::vector<Mathematics::Vector3>& lasso,
                    const Mathematics::Vector3& planeU,
                    const Mathematics::Vector3& planeV);

    // IGizmo
    void Render(GizmoRenderContext& context) override;
    GizmoHitResult HitTest(const GizmoRay& ray) override;

private:
    std::vector<Mathematics::Vector3> m_Points;
    float32 m_Radius = 2.0f;
    int32 m_SelectedIndex = -1;
    bool m_Active = false;
    bool m_BrushMode = false;
    bool m_CloseLoopHint = false;

    MarqueeGizmo m_Marquee;
};

// Scene tool for creating and editing splines.
//
// Two modes:
//   Click mode (default): left-click to place control points one at a time.
//   Brush mode (Shift+drag): paint a stroke on geometry, then simplify into a
//                             spline using Douglas-Peucker reduction.
//
// The tool raycasts against terrain and scene geometry to find hit positions.
// Press Escape to cancel the current stroke. Delete/Backspace removes the
// selected control points (routed in from the scene-view shortcut handler,
// which owns those keys — see DeleteSelectedControlPoints).
class SplineTool : public ISceneTool
{
public:
    explicit SplineTool(ECS::World& world);

    const char* GetName() const override { return "SplineTool"; }
    void OnActivated() override;
    void OnDeactivated() override;
    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& event) override;
    void GatherGizmos(GizmoCollector& collector) override;
    bool DeleteSelection() override { return DeleteSelectedControlPoints(); }

    void SetUndoRedoService(UndoRedoService* undo) { m_UndoRedo = undo; }
    void SetChangeNotifications(Editor::EditorChangeNotifications* n) { m_ChangeNotifications = n; }
    void SetTransformTool(TransformTool* tool) { m_TransformTool = tool; }
    // The world position of the selected knot or handle (the last one picked); false
    // when no control is selected. Reads the shared spline selection, so it needs no tool.
    static bool GetActiveControlWorldPosition(ECS::World& world, Mathematics::Vector3& outWorldPosition);

    // Erase every selected knot as one undo step and move the selection to a
    // surviving neighbour.
    //
    // Returns true when a knot selection was present, which means Delete has
    // been dealt with and the caller must NOT fall through to deleting the
    // entity. That holds even when nothing could be erased — a spline at its
    // two-point floor refuses the removal, and silently destroying the whole
    // entity instead is the surprise this path exists to prevent. Returns
    // false only when no knot is selected at all.
    bool DeleteSelectedControlPoints();

    // Selection query: returns the currently-selected entity in the scene view
    // (or an invalid handle). The tool uses this to target an existing spline
    // for editing — knot and handle dragging operate on whichever spline is
    // selected when the tool is active.
    void SetSelectionQuery(std::function<ECS::EntityHandle()> query)
    {
        m_SelectionQuery = std::move(query);
    }

    void SetSelectionListQuery(std::function<std::vector<ECS::EntityHandle>()> query)
    {
        m_SelectionListQuery = std::move(query);
    }

    void SetOnSplineCreated(std::function<void(ECS::EntityHandle)> callback)
    {
        m_OnSplineCreated = std::move(callback);
    }

    // Simplification tolerance for brush mode (world units).
    void SetSimplifyTolerance(float32 tolerance) { m_SimplifyTolerance = tolerance; }
    float32 GetSimplifyTolerance() const { return m_SimplifyTolerance; }

    // Minimum distance between consecutive brush samples (world units).
    void SetBrushMinDistance(float32 dist) { m_BrushMinDistance = dist; }

    // Post-draw smoothing iteration count (applied after Douglas-Peucker
    // simplification in brush mode). 0 disables smoothing.
    void SetSmoothingIterations(int32 iterations) { m_SmoothingIterations = iterations; }
    int32 GetSmoothingIterations() const { return m_SmoothingIterations; }

private:
    std::shared_ptr<SplineInteractionState> m_State;
    // Raycast against terrain heightfield and scene meshes.
    // When hitEntityId is non-null, receives the ECS entity id of the mesh hit
    // (0 if the hit came from terrain or the fallback plane).
    bool RaycastScene(const GizmoRay& ray,
                      Mathematics::Vector3& outHit,
                      uint32* hitEntityId = nullptr);

    // Raycast restricted to a single pre-known entity. Used by sticky-mesh
    // stroke tracking so the cursor stays glued to the current mesh even when
    // it drifts off-silhouette. Returns false if the entity is gone or the
    // ray doesn't actually intersect its geometry.
    bool RaycastEntityMesh(const GizmoRay& ray,
                           uint32 entityId,
                           Mathematics::Vector3& outHit);

    // Douglas-Peucker polyline simplification.
    static void SimplifyPolyline(const std::vector<Mathematics::Vector3>& input,
                                 float32 tolerance,
                                 std::vector<Mathematics::Vector3>& output);

    // Chaikin-style smoothing pass for a polyline (averaging). Each iteration
    // roughly doubles the point count while rounding off sharp corners.
    static void SmoothPolyline(std::vector<Mathematics::Vector3>& points,
                               int32 iterations);

    // Drop a vertical ray from above each authored knot and snap it to whatever
    // surface the ray hits. This keeps simplify tolerance in control of the
    // final knot count.
    void SnapPointsToSurface(std::vector<Mathematics::Vector3>& points);

    // Finalize the current stroke: create an entity with SplineComponent.
    void FinalizeSpline();

    // Cancel the current stroke without creating anything.
    void CancelStroke();

    // Edit-mode drag handlers (operate on the currently-selected spline
    // entity). Return true when they consume the pointer event.
    void UpdateHover(const ScenePointerEvent& event);
    bool TryBeginEditDrag(const ScenePointerEvent& event,
                          bool hit,
                          const Mathematics::Vector3& hitPos);
    bool UpdateEditDrag(const ScenePointerEvent& event,
                        bool hit,
                        const Mathematics::Vector3& hitPos);

    // Auto-connect: if the stroke's first or last point is within tolerance
    // of an existing spline's endpoint, merge the stroke into that spline
    // instead of creating a new entity. Returns true if the stroke was
    // consumed by an existing spline.
    bool TryExtendExistingSpline(const std::vector<Mathematics::Vector3>& stroke,
                                 float32 toleranceSq);

    // Marquee drag (rectangle or lasso). Active while a spline is selected,
    // the user is not interacting with a knot/handle, and Shift is not held.
    bool BeginMarquee(const ScenePointerEvent& event,
                      bool hit,
                      const Mathematics::Vector3& hitPos);
    void UpdateMarquee(const ScenePointerEvent& event,
                       bool hit,
                       const Mathematics::Vector3& hitPos);
    void FinishMarquee();
    void CancelMarquee();

    void SyncTransformGizmoTarget();
    void NotifySplineSelectionChanged(ECS::World* world, ECS::EntityHandle entity);
    void NotifyEditedSplineControls(ECS::World* world, Editor::EditorChangeNotifications::ChangeKind kind);
    void BeginSelectedControlTransformEdit(const char* editName);
    void ApplySelectedControlTransformDelta(const Mathematics::Vector3& deltaWorld);
    void CommitSelectedControlTransformEdit();
    void ClearSelectedControlTransformEdit();

    UndoRedoService* m_UndoRedo = nullptr;
    Editor::EditorChangeNotifications* m_ChangeNotifications = nullptr;

    UndoRedoService::InteractiveEdit m_SplineDragUndo{};
    std::function<ECS::EntityHandle()> m_SelectionQuery;
    std::function<std::vector<ECS::EntityHandle>()> m_SelectionListQuery;
    std::function<void(ECS::EntityHandle)> m_OnSplineCreated;
    TransformTool* m_TransformTool = nullptr;

    // Active stroke data.
    std::vector<Mathematics::Vector3> m_RawStrokePoints;     // Raw brush samples
    std::vector<Mathematics::Vector3> m_SimplifiedPoints;    // After Douglas-Peucker
    std::vector<Mathematics::Vector3> m_ClickPoints;         // Click-mode points
    int32 m_SelectedPointIndex = -1;

    bool m_IsDrawingBrush = false;  // Currently in brush drag
    bool m_DragMoved = false;       // Did the drag produce any Move events
    bool m_HasStroke = false;       // Has uncommitted points
    // Latched during the stroke whenever a move sample came within the
    // close-loop tolerance of the stroke start. Checked at finalize so the
    // actual closure matches what the live preview indicator showed, even if
    // the cursor drifted a pixel away on release.
    bool m_StrokeCloseLoopLatched = false;

    // Sticky-mesh state: during an active brush stroke we remember the first
    // entity whose surface the user hit. As long as StickToMesh is enabled we
    // prefer raycasts against that entity, so drifting the cursor off the
    // silhouette keeps the stroke on the target mesh instead of snapping to
    // whatever's behind it. Cleared at stroke start / cancel.
    uint32 m_StickyEntityId = 0;

    // Hover state. Updated on mouse move when not dragging. The gizmo
    // renderer uses this to highlight the knot/handle under the cursor.
    // kind: 0=none, 1=knot, 2=handleIn, 3=handleOut.
    int32 m_HoverKind = 0;
    int32 m_HoverPointIndex = -1;

    // Edit drag state. When non-zero kind, the tool is dragging a knot or
    // bezier handle of the selected spline entity instead of authoring new
    // points. kind: 0=none, 1=knot, 2=handleIn, 3=handleOut.
    int32 m_EditDragKind = 0;
    int32 m_EditDragPointIndex = -1;
    ECS::EntityHandle m_EditDragEntity{};

    struct EditDragControl
    {
        ECS::EntityHandle Entity{};
        SplineECS::SplineHandle Handle{};
        int32 PointIndex = -1;
        int32 Kind = 0;
        Mathematics::Vector3 StartPosition{0.0f, 0.0f, 0.0f};
        Mathematics::Vector3 StartTangentIn{0.0f, 0.0f, 0.0f};
        Mathematics::Vector3 StartTangentOut{0.0f, 0.0f, 0.0f};
        Mathematics::Vector3 StartWorldPosition{0.0f, 0.0f, 0.0f};
        Mathematics::Vector3 StartWorldHandle{0.0f, 0.0f, 0.0f};
        Mathematics::Matrix4x4 WorldMatrix{};
        Mathematics::Matrix4x4 InverseWorldMatrix{};
        bool HasTransform = false;
    };
    std::vector<EditDragControl> m_EditDragControls;
    Mathematics::Vector3 m_EditDragAnchorWorld{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_EditDragAccumulatedWorldDelta{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_EditDragPlaneOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_EditDragPlaneNormal{0.0f, 0.0f, 1.0f};
    Mathematics::Vector3 m_EditDragPointerOffsetWorld{0.0f, 0.0f, 0.0f};
    float32 m_EditDragStartViewX = 0.0f;
    float32 m_EditDragStartViewY = 0.0f;
    bool m_EditDragHasPlane = false;
    bool m_EditDragAwaitingMotion = false;

    // Marquee selection state. Active while the user is dragging out a
    // rectangle or lasso to multi-select knots on the currently selected
    // spline entity. Points are stored on the click-plane (the world plane
    // perpendicular to view at the click position) so they render directly
    // and the inclusion test runs in plane coordinates.
    bool m_MarqueeActive = false;
    Mathematics::Vector3 m_MarqueeStart{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_MarqueeCurrent{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_MarqueePlaneU{1.0f, 0.0f, 0.0f}; // plane basis
    Mathematics::Vector3 m_MarqueePlaneV{0.0f, 0.0f, 1.0f};
    Mathematics::Vector3 m_MarqueePlaneNormal{0.0f, 1.0f, 0.0f};
    std::vector<Mathematics::Vector3> m_MarqueeLassoPoints;

    float32 m_SimplifyTolerance = 0.25f; // Douglas-Peucker tolerance (world units)
    float32 m_BrushMinDistance = 1.0f;   // Min distance between brush samples
    int32 m_SmoothingIterations = 0;     // Chaikin smoothing passes after simplify

    SplineGizmo m_Gizmo;
};

} // namespace GameEngine::Editor::SceneTools
