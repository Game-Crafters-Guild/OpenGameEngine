#include "SceneView/SplineTool.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Name.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/SplinePointEdits.h"
#include "UndoRedo/SplineUndoHelpers.h"
#include "Scene/WorldSnapshotCommand.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "Logger/Logger.h"
#include "Mathematics/BezierCurve.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/VectorOps.h"
#include "Picking/MeshPickingService.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Spline/SplineUtility.h"
#include "SplineECS/SplineService.h"
#include "Types/ColorUtils.h"
#include "Core/Engine.h"
#include "Input/KeyCodes.h"
#include "SceneView/SplineOwnerQuery.h"
#include "SceneView/SplineStrokeCapture.h"
#include "SceneView/TransformTool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>

namespace GameEngine::Editor::SceneTools
{

using Mathematics::Vector3;

namespace
{
// Same basis as SceneViewTransformTool translate gizmo (constant screen size).
constexpr float kSplineScreenScaleRefAxisLength = 1.5f;
constexpr float kSplineScreenScaleBase            = 0.1875f;
constexpr float kAutoBezierTangentScale         = 1.0f / 6.0f;
constexpr float kBezierHandlePickRadiusScale    = 1.35f;

// Constant-screen-size scale for a gizmo `dist` world units from the camera
// (clamped to 1e-4), shared by drawing and picking so the two stay the same size.
float ScreenScaleAtDistance(float dist, bool smartDistanceScalingEnabled)
{
    if (dist < 1e-4f)
        dist = 1e-4f;

    const float linearScale =
        (dist * kSplineScreenScaleBase) / kSplineScreenScaleRefAxisLength;
    if (!smartDistanceScalingEnabled)
        return linearScale;

    return linearScale * std::sqrt(kGizmoSmartDistanceReference / dist);
}

} // namespace

SplineInteractionState::SplineInteractionState(ECS::World& world)
    : m_World(world), m_Generation(world.GetLifecycleResetGeneration())
{
}

void SplineInteractionState::Refresh()
{
    const uint64 generation = m_World.GetLifecycleResetGeneration();
    if (generation != m_Generation)
    {
        m_Generation = generation;
        m_Selection = {};
        m_Hover = {};
    }
}

SplineSelection& SplineInteractionState::Selection()
{
    Refresh();
    return m_Selection;
}

SplineHover& SplineInteractionState::Hover()
{
    Refresh();
    return m_Hover;
}

std::shared_ptr<SplineInteractionState> AcquireSplineInteractionState(ECS::World& world)
{
    static std::unordered_map<uint64, std::weak_ptr<SplineInteractionState>> states;
    std::erase_if(states, [](const auto& entry) { return entry.second.expired(); });
    auto& slot = states[world.GetWorldId()];
    if (auto state = slot.lock())
        return state;
    auto state = std::make_shared<SplineInteractionState>(world);
    slot = state;
    return state;
}

float ComputeSplineScreenScale(const GizmoRenderContext& context,
                               const Mathematics::Vector3& worldPos,
                               bool constantScreenSizeEnabled,
                               bool smartDistanceScalingEnabled)
{
    if (!constantScreenSizeEnabled || !context.HasCameraWorldPosition())
        return 1.0f;

    const float dist = context.HasOrthoHeight()
        ? context.GetOrthoHeight() * kOrthoEffectiveDistanceFactor
        : (*context.GetCameraWorldPosition() - worldPos).Length();
    return ScreenScaleAtDistance(dist, smartDistanceScalingEnabled);
}

Mathematics::Vector3 SplineCirclePlaneNormal(const GizmoRenderContext& context,
                                             const Mathematics::Vector3& worldPos)
{
    if (context.IsEditor2DMode() && context.HasCameraWorldPosition())
    {
        const Vector3 n = *context.GetCameraWorldPosition() - worldPos;
        const float lenSq = n.LengthSquared();
        if (lenSq > 1e-12f)
            return n * (1.0f / std::sqrt(lenSq));
    }
    return Vector3(0.0f, 1.0f, 0.0f);
}

void DrawSplineControlMarker(GizmoRenderContext& context,
                             const Mathematics::Vector3& pos,
                             float32 radius,
                             const Color& color,
                             float32 thickness,
                             bool constantScreen,
                             bool smartDistance,
                             std::vector<Mathematics::Vector3>* ringBatch)
{
    switch (Editor::SplineEditorSettings::Get().GetControlRenderShape())
    {
    case Editor::SplineControlRenderShape::Sphere:
        context.DrawSolidSphere(pos, radius * 0.5f, color);
        break;
    case Editor::SplineControlRenderShape::Cube:
    {
        const float halfExtent = radius * 0.5f;
        context.DrawSolidBox(pos, Vector3(halfExtent, halfExtent, halfExtent), color);
        break;
    }
    case Editor::SplineControlRenderShape::Ring:
    default:
    {
        const Vector3 ringNormal = SplineCirclePlaneNormal(context, pos);
        if (ringBatch)
            AppendThickWireCircleTriangles(context, pos, ringNormal, radius, thickness,
                                           constantScreen, smartDistance, *ringBatch);
        else
            DrawThickWireCircle(context, pos, ringNormal, radius, color, thickness,
                                constantScreen, smartDistance);
        break;
    }
    }
}

namespace
{

constexpr float32 kControlPointPickRadius = 1.5f;  // World-space pick radius for control points

bool RaycastPlaneY0(const GizmoRay& ray, Vector3& outHit)
{
    if (std::abs(ray.direction.y) < 1e-6f)
        return false;
    const float32 t = -ray.origin.y / ray.direction.y;
    if (t < 0.0f)
        return false;
    outHit = Vector3(ray.origin.x + ray.direction.x * t,
                0.0f,
                ray.origin.z + ray.direction.z * t);
    return true;
}

} // anonymous namespace

// ---- SplineGizmo ----

void SplineGizmo::SetPoints(const std::vector<Vector3>& points, float32 radius)
{
    m_Points = points;
    m_Radius = radius;
}

void SplineGizmo::SetMarquee(bool active,
                             int32 shape,
                             const Mathematics::Vector3& start,
                             const Mathematics::Vector3& current,
                             const std::vector<Mathematics::Vector3>& lasso,
                             const Mathematics::Vector3& planeU,
                             const Mathematics::Vector3& planeV)
{
    m_Marquee.SetActive(active);
    m_Marquee.SetShape(shape);
    m_Marquee.SetRect(start, current, planeU, planeV);
    m_Marquee.SetLasso(lasso);
}

namespace
{
// Compute Catmull-Rom style centered-difference tangent for a polyline point.
// Used as an initial estimate when promoting a raw polyline to Cubic Bezier.
Vector3 CenteredDiffTangent(const std::vector<Vector3>& pts, uint32 i)
{
    const uint32 n = static_cast<uint32>(pts.size());
    if (n < 2) return Vector3(0.0f, 0.0f, 0.0f);
    const Vector3& prev = pts[i > 0 ? i - 1 : 0];
    const Vector3& next = pts[i + 1 < n ? i + 1 : n - 1];
    Vector3 diff = next - prev;
    return diff * 0.5f;
}
} // namespace

void SplineGizmo::Render(GizmoRenderContext& context)
{
    if (!m_Active)
        return;

    const auto& settings = Editor::SplineEditorSettings::Get();
    const Color splineColor   = ColorUtils::UnpackArgb(settings.GetSplineColor());
    const Color brushColor    = ColorUtils::UnpackArgb(settings.GetBrushStrokeColor());
    const Color knotColor     = ColorUtils::UnpackArgb(settings.GetKnotColor());
    const Color selectedColor = ColorUtils::UnpackArgb(settings.GetKnotSelectedColor());
    const Color handleColor   = ColorUtils::UnpackArgb(settings.GetHandleColor());
    const float32 lineThickness = settings.GetSplineThickness();
    const float32 knotRadius = settings.GetKnotSize();
    const float32 handleRadius = settings.GetHandleSize();
    const float32 handleThickness = settings.GetHandleThickness();
    const float32 knotWireThickness = settings.GetKnotThickness();
    const Editor::SplineCurveType curveType = settings.GetCurveType();
    const bool linear = curveType == Editor::SplineCurveType::Linear;
    const bool bezier = curveType == Editor::SplineCurveType::CubicBezier;
    const bool constantScreen = settings.GetConstantScreenSize();
    const bool smartDist = settings.GetSmartDistanceScaling();
    const bool lineSmart = constantScreen && smartDist;

    // Marquee preview overlay. Drawn independently of the authoring stroke so
    // that it remains visible when the tool is in selection mode with no
    // uncommitted points.
    m_Marquee.Render(context);

    // Draw single point if only one exists.
    if (m_Points.size() == 1)
    {
        const Vector3 p0 = m_Points[0];
        const Vector3 up = SplineCirclePlaneNormal(context, p0);
        const float kr =
            knotRadius * ComputeSplineScreenScale(context, p0, constantScreen, smartDist);
        DrawThickWireCircle(context, p0, up, kr, knotColor, knotWireThickness, constantScreen,
                            lineSmart);
        return;
    }

    if (m_Points.size() < 2)
        return;

    const Color& lineColor = m_BrushMode ? brushColor : splineColor;
    constexpr uint32 kSegSubdivisions = 16; // smooth curve segments between control points
    const uint32 n = static_cast<uint32>(m_Points.size());

    // Draw spline curve through control points.
    for (uint32 seg = 0; seg + 1 < n; ++seg)
    {
        if (linear)
        {
            DrawThickLine(context, m_Points[seg], m_Points[seg + 1],
                          lineColor, lineThickness, constantScreen, lineSmart);
            continue;
        }

        Vector3 p0, p1, p2, p3;
        if (bezier)
        {
            // Preview-time Bezier: auto-compute tangents from neighbors so the
            // authoring stroke shows a plausible Bezier curve even though the
            // raw click points carry no explicit handles.
            const Vector3& kA = m_Points[seg];
            const Vector3& kB = m_Points[seg + 1];
            const Vector3 tA = CenteredDiffTangent(m_Points, seg) * kAutoBezierTangentScale;
            const Vector3 tB = CenteredDiffTangent(m_Points, seg + 1) * kAutoBezierTangentScale;
            p0 = kA;
            p1 = kA + tA;
            p2 = kB - tB;
            p3 = kB;
        }
        else // CatmullRom
        {
            p0 = m_Points[seg > 0 ? seg - 1 : 0];
            p1 = m_Points[seg];
            p2 = m_Points[seg + 1];
            p3 = m_Points[seg + 2 < n ? seg + 2 : n - 1];
        }

        Vector3 prev = bezier ? p0 : p1;
        for (uint32 s = 1; s <= kSegSubdivisions; ++s)
        {
            float32 t = static_cast<float32>(s) / static_cast<float32>(kSegSubdivisions);
            Vector3 curr = bezier ? Math::CubicBezier(p0, p1, p2, p3, t)
                             : Math::CatmullRom(p0, p1, p2, p3, t);

            DrawThickLine(context, prev, curr, lineColor, lineThickness, constantScreen, lineSmart);
            prev = curr;
        }
    }

    // Bezier: visualize auto-computed tangent handles during authoring.
    if (bezier)
    {
        for (uint32 i = 0; i < n; ++i)
        {
            const Vector3 t = CenteredDiffTangent(m_Points, i) * kAutoBezierTangentScale;
            const Vector3 kp = m_Points[i];
            if (i > 0)
            {
                const Vector3 hIn = kp - t;
                DrawThickLine(context, kp, hIn, handleColor, handleThickness, constantScreen, lineSmart);
                const float hrIn =
                    handleRadius * ComputeSplineScreenScale(context, hIn, constantScreen, smartDist);
                DrawSplineControlMarker(context, hIn, hrIn, handleColor, handleThickness, constantScreen,
                                        lineSmart, nullptr);
            }
            if (i + 1 < n)
            {
                const Vector3 hOut = kp + t;
                DrawThickLine(context, kp, hOut, handleColor, handleThickness, constantScreen, lineSmart);
                const float hrOut =
                    handleRadius * ComputeSplineScreenScale(context, hOut, constantScreen, smartDist);
                DrawSplineControlMarker(context, hOut, hrOut, handleColor, handleThickness, constantScreen,
                                        lineSmart, nullptr);
            }
        }
    }

    // Draw control point markers as small wireframe circles (no triangle fill
    // — avoids overflowing the gizmo triangle VB with many control points).
    for (size_t i = 0; i < m_Points.size(); ++i)
    {
        const Vector3 pos = m_Points[i];
        const Color& col = (static_cast<int32>(i) == m_SelectedIndex) ? selectedColor : knotColor;
        const float kr =
            knotRadius * 0.5f * ComputeSplineScreenScale(context, pos, constantScreen, smartDist);
        DrawSplineControlMarker(context, pos, kr, col, knotWireThickness, constantScreen, lineSmart, nullptr);
    }

    // Close-loop preview: when mid-stroke the cursor is within the close-loop
    // tolerance of the stroke start, draw a dashed segment from the last point
    // back to the first and emphasize the start knot with a larger ring so the
    // user sees that releasing here will snap into a closed loop.
    if (m_CloseLoopHint && m_BrushMode && m_Points.size() >= 3)
    {
        const Vector3 first = m_Points.front();
        const Vector3 last  = m_Points.back();
        constexpr uint32 kDashCount = 12;
        for (uint32 i = 0; i < kDashCount; ++i)
        {
            if ((i & 1u) != 0u)
                continue;
            const float32 t0 = static_cast<float32>(i) / static_cast<float32>(kDashCount);
            const float32 t1 = static_cast<float32>(i + 1) / static_cast<float32>(kDashCount);
            const Vector3 a = last + (first - last) * t0;
            const Vector3 b = last + (first - last) * t1;
            DrawThickLine(context, a, b, selectedColor, lineThickness * 1.25f, constantScreen, lineSmart);
        }
        const float ringR =
            knotRadius * 1.1f * ComputeSplineScreenScale(context, first, constantScreen, smartDist);
        const Vector3 planeN = SplineCirclePlaneNormal(context, first);
        DrawThickWireCircle(context, first, planeN, ringR, selectedColor,
                            knotWireThickness * 1.5f, constantScreen, lineSmart);
    }
}

GizmoHitResult SplineGizmo::HitTest(const GizmoRay& ray)
{
    GizmoHitResult result{};
    if (!m_Active || m_Points.empty())
        return result;

    // Test ray against each control point sphere.
    float32 bestDist = std::numeric_limits<float32>::max();
    int32 bestIdx = -1;

    for (size_t i = 0; i < m_Points.size(); ++i)
    {
        Vector3 toPoint = m_Points[i] - Vector3(ray.origin.x, ray.origin.y, ray.origin.z);
        Vector3 dir(ray.direction.x, ray.direction.y, ray.direction.z);
        float32 tProj = Vector3::Dot(toPoint, dir);
        if (tProj < 0.0f) continue;

        Vector3 closest = Vector3(ray.origin.x, ray.origin.y, ray.origin.z) + dir * tProj;
        Vector3 diff = closest - m_Points[i];
        float32 distSq = diff.LengthSquared();

        if (distSq < kControlPointPickRadius * kControlPointPickRadius && tProj < bestDist)
        {
            bestDist = tProj;
            bestIdx = static_cast<int32>(i);
        }
    }

    if (bestIdx >= 0)
    {
        result.hit = true;
        result.info.distance = bestDist;
        result.info.handleId = static_cast<uint32>(bestIdx);
    }

    return result;
}

// ---- SplineTool ----

SplineTool::SplineTool(ECS::World& world)
    : m_State(AcquireSplineInteractionState(world))
{
}

void SplineTool::OnActivated()
{
    m_Gizmo.SetActive(true);
    m_ClickPoints.clear();
    m_RawStrokePoints.clear();
    m_SimplifiedPoints.clear();
    m_IsDrawingBrush = false;
    m_HasStroke = false;
    m_SelectedPointIndex = -1;
}

void SplineTool::OnDeactivated()
{
    m_Gizmo.SetActive(false);
    if (m_TransformTool)
        m_TransformTool->ClearExternalPointTarget();
    CancelStroke();
    m_State->Selection() = SplineSelection{};
}


bool SplineTool::RaycastScene(const GizmoRay& ray, Vector3& outHit, uint32* hitEntityId)
{
    if (hitEntityId)
        *hitEntityId = 0;

    auto* world = &m_State->World();
    if (!world)
        return RaycastPlaneY0(ray, outHit);

    Mathematics::Ray3D mathRay;
    mathRay.origin    = {ray.origin.x,    ray.origin.y,    ray.origin.z};
    mathRay.direction = {ray.direction.x, ray.direction.y, ray.direction.z};

    const bool snapToMeshes = Editor::SplineEditorSettings::Get().GetSnapToMeshes();

    Editor::Picking::PickOptions options;
    options.IncludeMeshes     = snapToMeshes;
    options.IncludePrimitives = snapToMeshes;
    options.IncludeTerrain    = true;

    const auto pick = Editor::Picking::RaycastScene(mathRay, *world, options);
    if (pick.Hit)
    {
        outHit = pick.Best.WorldPosition;
        if (hitEntityId)
        {
            const bool canStickToHit =
                pick.Best.Kind == Editor::Picking::PickableKind::Mesh ||
                pick.Best.Kind == Editor::Picking::PickableKind::Primitive;
            *hitEntityId = (canStickToHit && pick.Best.Entity.IsValid()) ? pick.Best.Entity.id : 0u;
        }
        return true;
    }

    return RaycastPlaneY0(ray, outHit);
}

bool SplineTool::RaycastEntityMesh(const GizmoRay& ray, uint32 entityId, Vector3& outHit)
{
    if (entityId == 0)
        return false;

    auto* world = &m_State->World();
    if (!world)
        return false;

    Mathematics::Ray3D mathRay;
    mathRay.origin    = {ray.origin.x,    ray.origin.y,    ray.origin.z};
    mathRay.direction = {ray.direction.x, ray.direction.y, ray.direction.z};

    Editor::Picking::PickOptions options;
    options.RestrictToEntity = ECS::EntityHandle(entityId);

    const auto pick = Editor::Picking::RaycastScene(mathRay, *world, options);
    if (!pick.Hit)
        return false;

    outHit = pick.Best.WorldPosition;
    return true;
}

void SplineTool::OnPointerEvent(const ScenePointerEvent& event)
{
    if (event.phase == PointerPhase::Move && m_TransformTool)
    {
        SyncTransformGizmoTarget();
        m_TransformTool->OnPointerEvent(event);
    }

    const bool stickyEnabled = Editor::SplineEditorSettings::Get().GetStickToMesh();

    Vector3 hitPos;
    bool hit = false;
    uint32 hitEntity = 0;

    // While an active brush stroke has latched onto a sticky entity, raycast
    // exclusively against that entity so the stroke stays glued to the model
    // even when the cursor drifts off-silhouette. If we fell through to the
    // generic raycast here, a cursor off the mesh would snap straight down to
    // whatever's behind it (terrain / floor), which is the problem this option
    // fixes. A miss produces no sample and the stroke visibly pauses until the
    // cursor returns to the target mesh.
    if (stickyEnabled && m_IsDrawingBrush && m_StickyEntityId != 0)
    {
        hit = RaycastEntityMesh(event.ray, m_StickyEntityId, hitPos);
        if (hit)
            hitEntity = m_StickyEntityId;
    }
    else
    {
        hit = RaycastScene(event.ray, hitPos, &hitEntity);
    }

    // Update hover highlight on any mouse move (regardless of button).
    if (event.phase == PointerPhase::Move && m_EditDragKind == 0)
        UpdateHover(event);

    if (event.button != PointerButton::Left)
        return;

    // Marquee in progress: pump it directly so a drag-select stays consistent
    // even if the pointer leaves the world geometry mid-drag.
    if (m_MarqueeActive)
    {
        UpdateMarquee(event, hit, hitPos);
        if (event.phase == PointerPhase::Up)
        {
            FinishMarquee();
        }
        return;
    }

    // Edit mode: if a spline entity is selected, allow dragging knots and
    // bezier handles in place. A tool-level drag takes over when pointer-down
    // lands on a pickable handle; otherwise fall through to authoring below.
    if (TryBeginEditDrag(event, hit, hitPos))
        return;

    if (UpdateEditDrag(event, hit, hitPos))
        return;

    // Shift+click: add a single control point (precision mode).
    if (event.shift && event.phase == PointerPhase::Down && hit)
    {
        m_ClickPoints.push_back(hitPos);
        m_HasStroke = true;
        m_Gizmo.SetBrushMode(false);
        m_Gizmo.SetPoints(m_ClickPoints, Editor::SplineEditorSettings::Get().GetDefaultRadius());
        m_SelectedPointIndex = static_cast<int32>(m_ClickPoints.size() - 1);
        m_Gizmo.SetSelectedIndex(m_SelectedPointIndex);
        return;
    }

    // Default mode: click adds points, drag paints a brush stroke.
    // During drag: show the raw high-fidelity stroke for stable visual feedback.
    // On release: simplify with Douglas-Peucker to produce clean control points.
    if (event.phase == PointerPhase::Down && hit)
    {
        m_IsDrawingBrush = true;
        m_DragMoved = false;
        m_StrokeCloseLoopLatched = false;
        m_RawStrokePoints.clear();
        m_RawStrokePoints.push_back(hitPos);
        m_HasStroke = true;
        // Latch onto whatever entity the first sample hit so the rest of the
        // stroke prefers that surface. hitEntity is 0 for terrain / plane
        // fallback hits, in which case there's nothing to stick to.
        m_StickyEntityId = hitEntity;

        // Show existing points + the new start point.
        auto preview = m_ClickPoints;
        preview.push_back(hitPos);
        m_Gizmo.SetBrushMode(true);
        m_Gizmo.SetPoints(preview);
    }
    else if (event.phase == PointerPhase::Move && m_IsDrawingBrush && hit)
    {
        if (!m_RawStrokePoints.empty())
        {
            if (CaptureSplineStroke(m_RawStrokePoints, hitPos, m_BrushMinDistance))
            {
                m_DragMoved = true;

                // Show existing click points + the full raw stroke (no simplification).
                // This gives stable, smooth visual feedback while painting.
                auto preview = m_ClickPoints;
                preview.insert(preview.end(), m_RawStrokePoints.begin(), m_RawStrokePoints.end());
                m_Gizmo.SetBrushMode(true);
                m_Gizmo.SetPoints(preview);
            }

            // Live close-loop preview: mirror the finalize-time tolerance so
            // the visual hint and the actual snap behavior stay in sync. The
            // latch is sticky — once a sample enters the close-loop zone the
            // stroke is committed to closing even if the user releases a
            // fraction of a unit outside the ring.
            const float32 closeLoopTol =
                Editor::SplineEditorSettings::Get().GetCloseLoopTolerance();
            bool willClose = false;
            if (closeLoopTol > 0.0f && m_RawStrokePoints.size() >= 3)
            {
                const Vector3 d = hitPos - m_RawStrokePoints.front();
                willClose = d.LengthSquared() <= closeLoopTol * closeLoopTol;
                if (willClose)
                    m_StrokeCloseLoopLatched = true;
            }
            m_Gizmo.SetCloseLoopHint(willClose || m_StrokeCloseLoopLatched);
        }
    }
    else if (event.phase == PointerPhase::Up && m_IsDrawingBrush)
    {
        m_IsDrawingBrush = false;

        // Capture the final pointer position on release: close-loop detection
        // needs it when the user lifts near the stroke start.
        if (hit)
            EndSplineStroke(m_RawStrokePoints, hitPos);

        if (m_DragMoved && m_RawStrokePoints.size() >= 2)
        {
            // Simplify on release, optionally smooth, simplify again, then
            // create the entity. Surface conform snaps the simplified knots in
            // place instead of inserting extra authored points, so the
            // tolerance directly controls final knot count.
            // Pull tolerance / iterations from persisted settings so the menu
            // controls them without any intermediate wiring.
            const auto& s = Editor::SplineEditorSettings::Get();
            const float tolerance = s.GetSimplifyTolerance();
            const int iterations = s.GetSmoothingIterations();
            SimplifyPolyline(m_RawStrokePoints, tolerance, m_SimplifiedPoints);
            if (iterations > 0)
                SmoothPolyline(m_SimplifiedPoints, iterations);
            if (tolerance > 0.0f)
            {
                std::vector<Vector3> finalSimplified;
                SimplifyPolyline(m_SimplifiedPoints, tolerance, finalSimplified);
                m_SimplifiedPoints = std::move(finalSimplified);
            }
            if (s.GetConformToSurface())
                SnapPointsToSurface(m_SimplifiedPoints);
            m_ClickPoints = m_SimplifiedPoints;
            FinalizeSpline();
        }

        // Clear for the next stroke (next drag = new entity).
        m_Gizmo.SetBrushMode(false);
        m_Gizmo.SetCloseLoopHint(false);
        m_Gizmo.SetPoints({});
        m_ClickPoints.clear();
        m_RawStrokePoints.clear();
        m_StrokeCloseLoopLatched = false;
        m_StickyEntityId = 0;
    }
}

void SplineTool::OnKeyEvent(const SceneKeyEvent& event)
{
    if (!event.pressed)
        return;

    // Escape: cancel current stroke mid-drag. The Scene View sends the input layer's key codes.
    if (static_cast<int>(event.keyCode) == Input::kKeyCode_Escape)
    {
        CancelStroke();
        return;
    }
}

void SplineTool::GatherGizmos(GizmoCollector& collector)
{
    SyncTransformGizmoTarget();
    if (m_TransformTool)
        m_TransformTool->GatherGizmos(collector);
    collector.AddGizmo(&m_Gizmo);
}

bool SplineTool::GetActiveControlWorldPosition(ECS::World& boundWorld, Vector3& outWorldPosition)
{
    const auto state = AcquireSplineInteractionState(boundWorld);
    const SplineSelection& sel = state->Selection();
    SplineControlSelection active{};
    if (!sel.Controls.empty())
        active = sel.Controls.back();
    else
        active = {sel.Entity, sel.PointIndex, sel.Kind};

    if (!active.Entity.IsValid() || active.PointIndex < 0 || active.Kind == 0)
        return false;

    auto* world = &boundWorld;
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService || !world->IsValid(active.Entity))
        return false;

    const auto* comp = world->GetComponent<Components::SplineComponent>(active.Entity);
    if (!comp || !ECS::Entity(world, active.Entity).IsEnabled<Components::SplineComponent>())
        return false;

    SplineECS::SplineHandle handle(comp->SplineDataIndex, comp->SplineDataGeneration);
    const auto* data = splineService->GetSplineData(handle);
    if (!data || static_cast<uint32>(active.PointIndex) >= static_cast<uint32>(data->Points.size()))
        return false;

    const auto& point = data->Points[static_cast<uint32>(active.PointIndex)];
    Vector3 local = point.Position;
    if (active.Kind == 2)
        local = point.Position + point.TangentIn;
    else if (active.Kind == 3)
        local = point.Position + point.TangentOut;

    if (const auto* xf = world->GetComponent<Components::WorldTransform>(active.Entity))
    {
        Mathematics::Matrix4x4 worldM;
        worldM = Mathematics::Matrix4x4::FromColumnMajor(xf->matrix);
        outWorldPosition = worldM.TransformPoint(local);
    }
    else
    {
        outWorldPosition = local;
    }
    return true;
}

void SplineTool::SyncTransformGizmoTarget()
{
    if (!m_TransformTool)
        return;

    Vector3 pos;
    if (!GetActiveControlWorldPosition(m_State->World(), pos))
    {
        m_TransformTool->ClearExternalPointTarget();
        return;
    }

    m_TransformTool->SetExternalPointTarget({
        [this](Vector3& outWorldPosition) -> bool
        {
            return GetActiveControlWorldPosition(m_State->World(), outWorldPosition);
        },
        [this](const char* editName)
        {
            BeginSelectedControlTransformEdit(editName);
        },
        [this](const Vector3& deltaWorld)
        {
            ApplySelectedControlTransformDelta(deltaWorld);
        },
        [this]()
        {
            CommitSelectedControlTransformEdit();
        }
    });
}

void SplineTool::NotifySplineSelectionChanged(ECS::World* world, ECS::EntityHandle entity)
{
    if (!m_ChangeNotifications || !world || !entity.IsValid() || !world->IsValid(entity))
        return;

    m_ChangeNotifications->NotifyComponentChange<Components::SplineComponent>(
        world, entity, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
}

void SplineTool::NotifyEditedSplineControls(ECS::World* world,
                                            Editor::EditorChangeNotifications::ChangeKind kind)
{
    if (!m_ChangeNotifications || !world)
        return;

    std::vector<ECS::EntityHandle> notified;
    for (const auto& dc : m_EditDragControls)
    {
        if (!dc.Entity.IsValid() || !world->IsValid(dc.Entity))
            continue;
        if (std::find(notified.begin(), notified.end(), dc.Entity) != notified.end())
            continue;

        m_ChangeNotifications->NotifyComponentChange<Components::SplineComponent>(world, dc.Entity, kind);
        notified.push_back(dc.Entity);
    }
}

void SplineTool::BeginSelectedControlTransformEdit(const char* editName)
{
    if (m_EditDragKind != 0)
        return;

    auto* world = &m_State->World();
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService)
        return;

    SplineSelection& sel = m_State->Selection();
    if (sel.Controls.empty() && sel.Entity.IsValid() && sel.Kind != 0 && sel.PointIndex >= 0)
        sel.Controls.push_back({sel.Entity, sel.PointIndex, sel.Kind});
    if (sel.Controls.empty())
        return;

    const SplineControlSelection active = sel.Controls.back();
    m_EditDragKind = active.Kind;
    m_EditDragPointIndex = active.PointIndex;
    m_EditDragEntity = active.Entity;
    m_EditDragControls.clear();
    m_EditDragAnchorWorld = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragAccumulatedWorldDelta = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragPlaneOrigin = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragPlaneNormal = Vector3(0.0f, 0.0f, 1.0f);
    m_EditDragPointerOffsetWorld = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragHasPlane = false;

    std::vector<std::pair<ECS::EntityHandle, SplineECS::SplineHandle>> undoSplines;
    std::vector<ECS::EntityHandle> undoEntities;
    for (const auto& c : sel.Controls)
    {
        if (c.PointIndex < 0 || c.Kind == 0 || !c.Entity.IsValid() || !world->IsValid(c.Entity))
            continue;
        const auto* comp = world->GetComponent<Components::SplineComponent>(c.Entity);
        if (!comp || !ECS::Entity(world, c.Entity).IsEnabled<Components::SplineComponent>())
            continue;

        SplineECS::SplineHandle handle(comp->SplineDataIndex, comp->SplineDataGeneration);
        auto* data = splineService->GetSplineData(handle);
        if (!data || static_cast<uint32>(c.PointIndex) >= static_cast<uint32>(data->Points.size()))
            continue;

        EditDragControl dc{};
        dc.Entity = c.Entity;
        dc.Handle = handle;
        dc.PointIndex = c.PointIndex;
        dc.Kind = c.Kind;
        const auto& point = data->Points[static_cast<uint32>(c.PointIndex)];
        dc.StartPosition = point.Position;
        dc.StartTangentIn = point.TangentIn;
        dc.StartTangentOut = point.TangentOut;

        const auto* xf = world->GetComponent<Components::WorldTransform>(c.Entity);
        dc.HasTransform = xf != nullptr;
        if (xf)
        {
            dc.WorldMatrix = Mathematics::Matrix4x4::FromColumnMajor(xf->matrix);
            dc.InverseWorldMatrix = Mathematics::Inverse(dc.WorldMatrix);
        }
        dc.StartWorldPosition = dc.HasTransform ? dc.WorldMatrix.TransformPoint(point.Position) : point.Position;
        Vector3 localHandle = point.Position;
        if (c.Kind == 2)
            localHandle = point.Position + point.TangentIn;
        else if (c.Kind == 3)
            localHandle = point.Position + point.TangentOut;
        dc.StartWorldHandle = dc.HasTransform ? dc.WorldMatrix.TransformPoint(localHandle) : localHandle;
        if (c.Entity == active.Entity && c.Kind == active.Kind && c.PointIndex == active.PointIndex)
            m_EditDragAnchorWorld = c.Kind == 1 ? dc.StartWorldPosition : dc.StartWorldHandle;

        if (std::find(undoEntities.begin(), undoEntities.end(), c.Entity) == undoEntities.end())
        {
            undoEntities.push_back(c.Entity);
            undoSplines.emplace_back(c.Entity, handle);
        }
        m_EditDragControls.push_back(dc);
    }

    if (m_EditDragControls.empty())
    {
        ClearSelectedControlTransformEdit();
        return;
    }

    m_SplineDragUndo = {};
    if (m_UndoRedo && undoSplines.size() == 1u)
    {
        const char* label = (editName && *editName) ? editName : "Transform Spline Control";
        m_SplineDragUndo = m_UndoRedo->BeginInteractiveEdit(
            label,
            Editor::SplineUndo::MakeSplineEditableSnapshotTarget(
                splineService, undoSplines.front().second, world, undoSplines.front().first,
                m_ChangeNotifications, label));
    }
}

void SplineTool::ApplySelectedControlTransformDelta(const Vector3& deltaWorld)
{
    if (m_EditDragKind == 0 || m_EditDragControls.empty())
        return;

    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService)
        return;

    m_EditDragAccumulatedWorldDelta = m_EditDragAccumulatedWorldDelta + deltaWorld;
    const Vector3 delta = m_EditDragAccumulatedWorldDelta;
    std::vector<SplineECS::SplineHandle> rebuilt;
    auto apply = [&]()
    {
        for (const auto& dc : m_EditDragControls)
        {
            auto* data = splineService->GetSplineData(dc.Handle);
            if (!data || dc.PointIndex < 0 ||
                static_cast<uint32>(dc.PointIndex) >= static_cast<uint32>(data->Points.size()))
                continue;

            const uint32 i = static_cast<uint32>(dc.PointIndex);
            if (dc.Kind == 1)
            {
                const Vector3 worldPos = dc.StartWorldPosition + delta;
                const Vector3 localPos = dc.HasTransform ? dc.InverseWorldMatrix.TransformPoint(worldPos) : worldPos;
                data->SetPointPosition(i, localPos);
            }
            else
            {
                const Vector3 worldHandle = dc.StartWorldHandle + delta;
                const Vector3 localHandle =
                    dc.HasTransform ? dc.InverseWorldMatrix.TransformPoint(worldHandle) : worldHandle;
                const Vector3 offset = localHandle - data->Points[i].Position;
                Vector3 tangentIn = data->Points[i].TangentIn;
                Vector3 tangentOut = data->Points[i].TangentOut;
                if (dc.Kind == 2)
                {
                    tangentIn = offset;
                    tangentOut = Vector3(0.0f, 0.0f, 0.0f) - offset;
                }
                else
                {
                    tangentOut = offset;
                    tangentIn = Vector3(0.0f, 0.0f, 0.0f) - offset;
                }
                data->SetPointTangents(i, tangentIn, tangentOut);
            }

            if (std::find(rebuilt.begin(), rebuilt.end(), dc.Handle) == rebuilt.end())
            {
                splineService->RebuildCache(dc.Handle);
                rebuilt.push_back(dc.Handle);
            }
        }
    };

    if (m_SplineDragUndo)
        m_SplineDragUndo.Preview(apply);
    else
    {
        apply();
        auto* world = &m_State->World();
        NotifyEditedSplineControls(world, Editor::EditorChangeNotifications::ChangeKind::Preview);
    }
}

void SplineTool::CommitSelectedControlTransformEdit()
{
    if (m_SplineDragUndo)
        m_SplineDragUndo.Commit();
    m_SplineDragUndo = {};

    auto* world = &m_State->World();
    if (m_ChangeNotifications && world)
    {
        Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = world;
        e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
        m_ChangeNotifications->NotifyWorldStructureChanged(e);
    }

    ClearSelectedControlTransformEdit();
}

void SplineTool::ClearSelectedControlTransformEdit()
{
    m_EditDragKind = 0;
    m_EditDragPointIndex = -1;
    m_EditDragEntity = {};
    m_EditDragControls.clear();
    m_EditDragAccumulatedWorldDelta = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragPlaneOrigin = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragPlaneNormal = Vector3(0.0f, 0.0f, 1.0f);
    m_EditDragPointerOffsetWorld = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragHasPlane = false;
    m_EditDragAwaitingMotion = false;
}

void SplineTool::FinalizeSpline()
{
    const auto& points = m_ClickPoints;
    auto* world = &m_State->World();
    if (points.size() < 2 || !world)
    {
        CancelStroke();
        return;
    }

    // Create an entity with Transform + SplineComponent.
    // Users can add a TerrainModifierVolume or other components via the inspector.
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService)
    {
        CancelStroke();
        return;
    }

    // Map the editor's curve-type setting to the engine's SplineType enum.
    // Note the enums disagree on value order, so this cannot be a static_cast.
    Spline::SplineType engineType = Spline::SplineType::CatmullRom;
    switch (Editor::SplineEditorSettings::Get().GetCurveType())
    {
        case Editor::SplineCurveType::CatmullRom:  engineType = Spline::SplineType::CatmullRom;  break;
        case Editor::SplineCurveType::Linear:      engineType = Spline::SplineType::Linear;      break;
        case Editor::SplineCurveType::CubicBezier: engineType = Spline::SplineType::CubicBezier; break;
    }

    // Auto-connect to an existing spline if the stroke starts or ends near an
    // existing spline endpoint. "AutoConnect" setting gates this behavior so
    // users can turn it off when they want isolated strokes.
    const auto& splineSettings = Editor::SplineEditorSettings::Get();
    const bool autoConnect = splineSettings.GetAutoConnect();
    const float32 connectTol = splineSettings.GetAutoConnectTolerance();
    const float32 connectTolSq = connectTol * connectTol;
    const float32 closeLoopTol = splineSettings.GetCloseLoopTolerance();
    const float32 closeLoopTolSq = closeLoopTol * closeLoopTol;

    // Detect closed-loop authoring first: use the latched preview flag so
    // the actual closure matches exactly what the live indicator showed
    // during the stroke — independent of whether the release pointer event
    // happened to land inside the tolerance ring. Trim any tail points that
    // the user dragged through the close-loop zone so the loop is watertight.
    // Takes priority over auto-connect so a nearby existing spline endpoint
    // doesn't hijack the stroke and prevent closing.
    bool closed = false;
    std::vector<Vector3> pointsCopy = points;
    if (m_StrokeCloseLoopLatched && pointsCopy.size() >= 3)
    {
        const Vector3 first = pointsCopy.front();
        int32 trimTo = static_cast<int32>(pointsCopy.size());
        for (int32 i = static_cast<int32>(pointsCopy.size()) - 1; i >= 2; --i)
        {
            const Vector3 diff = pointsCopy[i] - first;
            if (diff.LengthSquared() <= closeLoopTolSq)
                trimTo = i;
            else
                break;
        }
        pointsCopy.resize(static_cast<size_t>(trimTo));
        closed = true;
    }

    if (!closed && autoConnect && TryExtendExistingSpline(points, connectTolSq))
    {
        CancelStroke();
        return;
    }

    const bool undoNewSplineEntity = static_cast<bool>(m_UndoRedo) && world;
    std::vector<uint8_t> worldUndoBefore;
    if (undoNewSplineEntity)
        worldUndoBefore = world->SerializeWorld();

    auto handle = splineService->CreateSpline(engineType, closed);
    auto* data = splineService->GetSplineData(handle);
    if (!data)
    {
        CancelStroke();
        return;
    }

    // Store points in local space (relative to entity origin at first point).
    // TerrainModifierSystem applies the entity's WorldTransform to bring them
    // back to world space, so moving/rotating/scaling the entity works correctly.
    const Vector3 origin = pointsCopy[0];
    const float32 defaultRadius = Editor::SplineEditorSettings::Get().GetDefaultRadius();
    for (const auto& pt : pointsCopy)
        data->AddPoint(pt - origin, defaultRadius);

    // For Bezier, seed reasonable initial handle offsets from centered-
    // difference neighbors. Without this, every point has zero tangents and
    // the resulting curve degenerates into straight chords.
    if (engineType == Spline::SplineType::CubicBezier)
    {
        const uint32 count = static_cast<uint32>(pointsCopy.size());
        for (uint32 i = 0; i < count; ++i)
        {
            Vector3 t = CenteredDiffTangent(pointsCopy, i) * kAutoBezierTangentScale;
            Vector3 tNeg = t * -1.0f;
            data->SetPointTangents(i, tNeg, t);
        }
    }

    splineService->RebuildCache(handle);

    // Create entity with Transform + Name + SplineComponent.
    // Use AddComponentImmediate so the entity is fully formed this frame.
    auto entity = world->CreateEntity();
    if (!entity.IsValid())
    {
        CancelStroke();
        return;
    }

    Components::Transform xf{};
    xf.SetIdentity();
    xf.matrix[12] = pointsCopy[0].x;
    xf.matrix[13] = pointsCopy[0].y;
    xf.matrix[14] = pointsCopy[0].z;
    world->AddComponentImmediate(entity, xf);

    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    std::strncpy(nm.value, "Spline", sizeof(nm.value) - 1);
    world->AddComponentImmediate(entity, nm);

    Components::SplineComponent splineComp{};
    splineComp.SplineDataIndex = handle.Index();
    splineComp.SplineDataGeneration = handle.Generation();
    splineComp.DefaultRadius = defaultRadius;
    world->AddComponentImmediate(entity, splineComp);

    // Notify the editor so the hierarchy panel refreshes.
    if (m_ChangeNotifications)
    {
        Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = world;
        e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
        m_ChangeNotifications->NotifyWorldStructureChanged(e);
    }

    if (undoNewSplineEntity && m_UndoRedo)
    {
        std::vector<uint8_t> worldAfter = world->SerializeWorld();
        if (worldAfter != worldUndoBefore)
        {
            m_UndoRedo->CommitAlreadyApplied(std::make_unique<GameEngine::Editor::WorldSnapshotCommand>(
                "Create Spline",
                world,
                m_ChangeNotifications,
                std::move(worldUndoBefore),
                std::move(worldAfter)));
        }
    }

    if (m_OnSplineCreated)
        m_OnSplineCreated(entity);

    CancelStroke();
}

void SplineTool::CancelStroke()
{
    m_SplineDragUndo = {};
    m_ClickPoints.clear();
    m_RawStrokePoints.clear();
    m_SimplifiedPoints.clear();
    m_IsDrawingBrush = false;
    m_HasStroke = false;
    m_StrokeCloseLoopLatched = false;
    m_StickyEntityId = 0;
    m_SelectedPointIndex = -1;
    m_Gizmo.SetPoints({});
    m_Gizmo.SetSelectedIndex(-1);
    m_Gizmo.SetCloseLoopHint(false);
}

void SplineTool::SimplifyPolyline(const std::vector<Vector3>& input,
                                   float32 tolerance,
                                   std::vector<Vector3>& output)
{
    Spline::SimplifyPolyline(input, tolerance, output);
}

float32 PickScreenScale(const GizmoRay& ray,
                        const Mathematics::Vector3& worldPosition,
                        bool constantScreenSizeEnabled,
                        bool smartDistanceScalingEnabled)
{
    if (!constantScreenSizeEnabled)
        return 1.0f;

    return ScreenScaleAtDistance((ray.origin - worldPosition).Length(), smartDistanceScalingEnabled);
}

namespace
{

// Closest-approach point of a ray to a world-space point. Returns the
// squared distance (clamped to positive-t half of the ray).
float32 RayPointDistSq(const GizmoRay& ray, const Vector3& p, float32& outT)
{
    Vector3 ro(ray.origin.x, ray.origin.y, ray.origin.z);
    Vector3 rd(ray.direction.x, ray.direction.y, ray.direction.z);
    Vector3 toP = p - ro;
    float32 t = Vector3::Dot(toP, rd);
    if (t < 0.0f) t = 0.0f;
    Vector3 closest = ro + rd * t;
    Vector3 diff = closest - p;
    outT = t;
    return diff.LengthSquared();
}

struct SplineControlPick
{
    ECS::EntityHandle Entity{};
    SplineECS::SplineHandle Handle{};
    int32 Kind = 0;
    int32 PointIndex = -1;
    float32 DistSq = std::numeric_limits<float32>::max();
    float32 RayT = std::numeric_limits<float32>::max();
};

bool SameControl(const SplineControlSelection& a,
                 ECS::EntityHandle entity,
                 int32 kind,
                 int32 pointIndex)
{
    return a.Entity == entity && a.Kind == kind && a.PointIndex == pointIndex;
}

bool ContainsControl(const std::vector<SplineControlSelection>& controls,
                     ECS::EntityHandle entity,
                     int32 kind,
                     int32 pointIndex)
{
    return std::any_of(controls.begin(), controls.end(), [&](const SplineControlSelection& c) {
        return SameControl(c, entity, kind, pointIndex);
    });
}

// False means "v holds no usable direction" and is the only signal callers
// have to step their fallback ladder, so non-finite input must report false
// rather than be normalized into a NaN the caller then trusts. NaN compares
// false against everything, so a bare `lenSq < eps` test would report success
// on it; an infinite lenSq needs the finite test for the same reason.
bool NormalizeVector(Vector3& v)
{
    const float32 lenSq = v.LengthSquared();
    if (!std::isfinite(lenSq) || lenSq < 1.0e-8f)
        return false;
    v = v * (1.0f / std::sqrt(lenSq));
    return true;
}

void SyncLegacySplineSelection(SplineSelection& sel)
{
    sel.KnotIndices.clear();
    if (sel.Controls.empty())
    {
        sel.Entity = {};
        sel.Kind = 0;
        sel.PointIndex = -1;
        return;
    }

    const auto& active = sel.Controls.back();
    sel.Entity = active.Entity;
    sel.Kind = active.Kind;
    sel.PointIndex = active.PointIndex;
    for (const auto& c : sel.Controls)
    {
        if (c.Entity == sel.Entity && c.Kind == 1)
            sel.KnotIndices.push_back(c.PointIndex);
    }
}

bool FindBestControlOnSpline(ECS::World* world,
                             SplineECS::SplineService* splineService,
                             ECS::EntityHandle entity,
                             const ScenePointerEvent& event,
                             float32 knotBase,
                             float32 handleBase,
                             bool constScreen,
                             bool smartDist,
                             SplineControlPick& best)
{
    if (!world || !splineService || !entity.IsValid() || !world->IsValid(entity))
        return false;

    const auto* comp = world->GetComponent<Components::SplineComponent>(entity);
    if (!comp || !ECS::Entity(world, entity).IsEnabled<Components::SplineComponent>())
        return false;

    SplineECS::SplineHandle handle(comp->SplineDataIndex, comp->SplineDataGeneration);
    const auto* data = splineService->GetSplineData(handle);
    if (!data || !data->IsValid())
        return false;

    const bool bezier = data->Type == Spline::SplineType::CubicBezier;
    const auto* xf = world->GetComponent<Components::WorldTransform>(entity);
    Mathematics::Matrix4x4 worldM;
    if (xf)
        worldM = Mathematics::Matrix4x4::FromColumnMajor(xf->matrix);

    const auto& pts = data->Points;
    const uint32 n = static_cast<uint32>(pts.size());
    bool found = false;
    for (uint32 i = 0; i < n; ++i)
    {
        float32 t = 0.0f;
        const Vector3 worldPt = xf ? worldM.TransformPoint(pts[i].Position) : pts[i].Position;
        const float32 pickR = knotBase * PickScreenScale(event.ray, worldPt, constScreen, smartDist);
        const float32 d = RayPointDistSq(event.ray, worldPt, t);
        if (d < pickR * pickR && t < best.RayT)
        {
            best = {entity, handle, 1, static_cast<int32>(i), d, t};
            found = true;
        }

        if (!bezier)
            continue;

        if (i > 0)
        {
            Vector3 hIn = pts[i].Position + pts[i].TangentIn;
            if (xf) hIn = worldM.TransformPoint(hIn);
            const float32 hPickR =
                handleBase * kBezierHandlePickRadiusScale *
                PickScreenScale(event.ray, hIn, constScreen, smartDist);
            const float32 dh = RayPointDistSq(event.ray, hIn, t);
            if (dh < hPickR * hPickR && t < best.RayT)
            {
                best = {entity, handle, 2, static_cast<int32>(i), dh, t};
                found = true;
            }
        }
        if (i + 1 < n)
        {
            Vector3 hOut = pts[i].Position + pts[i].TangentOut;
            if (xf) hOut = worldM.TransformPoint(hOut);
            const float32 hPickR =
                handleBase * kBezierHandlePickRadiusScale *
                PickScreenScale(event.ray, hOut, constScreen, smartDist);
            const float32 dh = RayPointDistSq(event.ray, hOut, t);
            if (dh < hPickR * hPickR && t < best.RayT)
            {
                best = {entity, handle, 3, static_cast<int32>(i), dh, t};
                found = true;
            }
        }
    }
    return found;
}

std::vector<ECS::EntityHandle> CollectEditableSplineEntities(
    ECS::World* world,
    const std::vector<ECS::EntityHandle>& selectedEntities,
    bool includeAllSplines)
{
    std::vector<ECS::EntityHandle> entities;
    entities.reserve(selectedEntities.size());
    // A spline another feature owns (SplineOwnerQuery) offers its knots only while selected and
    // not hidden, and editing it offers no other spline's.
    bool claimedSelected = false;
    for (const auto& entity : selectedEntities)
    {
        if (!entity.IsValid() || std::find(entities.begin(), entities.end(), entity) != entities.end())
            continue;
        const Editor::SplineOwnerClaim claim =
            world ? Editor::QuerySplineOwner(*world, entity) : Editor::SplineOwnerClaim{};
        if (claim.Claimed && claim.Hidden)
            continue;
        claimedSelected = claimedSelected || claim.Claimed;
        entities.push_back(entity);
    }

    if (!includeAllSplines || !world || claimedSelected)
        return entities;

    world->Query<ECS::Read<Components::SplineComponent>>()
        .Each([&](ECS::EntityHandle entity, const Components::SplineComponent&)
        {
            if (Editor::QuerySplineOwner(*world, entity).Claimed)
                return;
            if (std::find(entities.begin(), entities.end(), entity) == entities.end())
                entities.push_back(entity);
        });

    return entities;
}

UndoRedoService::SnapshotTarget MakeMultiSplineEditableSnapshotTarget(
    SplineECS::SplineService* splineService,
    const std::vector<std::pair<ECS::EntityHandle, SplineECS::SplineHandle>>& splines,
    ECS::World* world,
    Editor::EditorChangeNotifications* notifications,
    const std::string& label)
{
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [splineService, splines](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        out.clear();
        const uint32_t count = static_cast<uint32_t>(splines.size());
        out.resize(4u);
        std::memcpy(out.data(), &count, 4u);
        for (const auto& entry : splines)
        {
            UndoRedoService::SnapshotTarget::Snapshot one;
            const auto* data = splineService ? splineService->GetSplineData(entry.second) : nullptr;
            if (!Editor::SplineUndo::EncodeSplineConfig(data, one))
                return false;
            const uint32_t bytes = static_cast<uint32_t>(one.size());
            const size_t oldSize = out.size();
            out.resize(oldSize + 4u + one.size());
            std::memcpy(out.data() + oldSize, &bytes, 4u);
            std::memcpy(out.data() + oldSize + 4u, one.data(), one.size());
        }
        return true;
    };

    target.Apply = [splineService, splines](const UndoRedoService::SnapshotTarget::Snapshot& snap) -> bool
    {
        if (snap.size() < 4u)
            return false;
        uint32_t count = 0;
        std::memcpy(&count, snap.data(), 4u);
        if (count != splines.size())
            return false;

        size_t offset = 4u;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (offset + 4u > snap.size())
                return false;
            uint32_t bytes = 0;
            std::memcpy(&bytes, snap.data() + offset, 4u);
            offset += 4u;
            if (offset + bytes > snap.size())
                return false;

            UndoRedoService::SnapshotTarget::Snapshot one(bytes);
            if (bytes > 0u)
                std::memcpy(one.data(), snap.data() + offset, bytes);
            offset += bytes;

            Spline::SplineData restored{};
            if (!Editor::SplineUndo::DecodeSplineConfig(one, restored))
                return false;
            auto* data = splineService ? splineService->GetSplineData(splines[i].second) : nullptr;
            if (!data)
                return false;
            *data = std::move(restored);
            data->MarkDirty();
            splineService->RebuildCache(splines[i].second);
        }
        return offset == snap.size();
    };

    target.Notify = [world, splines, notifications](Editor::EditorChangeNotifications::ChangeKind kind)
    {
        if (!notifications)
            return;
        for (const auto& entry : splines)
            notifications->NotifyComponentChange<Components::SplineComponent>(world, entry.first, kind);
    };

    return target;
}

} // namespace

bool SplineTool::DeleteSelectedControlPoints()
{
    auto* world = &m_State->World();
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService)
        return false;

    SplineSelection& sel = m_State->Selection();

    // Only knots are erasable: a Bezier handle is a property of the point that
    // carries it, not a control that can be removed on its own.
    std::vector<SplineControlSelection> knots;
    for (const auto& control : sel.Controls)
    {
        if (control.Kind == 1 && control.PointIndex >= 0 && control.Entity.IsValid())
            knots.push_back(control);
    }
    if (knots.empty() && sel.Entity.IsValid() && sel.Kind == 1 && sel.PointIndex >= 0)
        knots.push_back({sel.Entity, sel.PointIndex, 1});

    if (knots.empty())
        return false;

    struct RemovalTarget
    {
        ECS::EntityHandle Entity{};
        SplineECS::SplineHandle Handle{};
        // Descending, so each erase leaves the not-yet-erased indices valid.
        std::vector<uint32> Indices;
        uint32 LowestIndex = 0;
    };

    std::vector<RemovalTarget> targets;
    for (const auto& knot : knots)
    {
        if (!world->IsValid(knot.Entity))
            continue;

        auto it = std::find_if(targets.begin(), targets.end(),
                               [&](const RemovalTarget& t) { return t.Entity == knot.Entity; });
        if (it == targets.end())
        {
            const auto* comp = world->GetComponent<Components::SplineComponent>(knot.Entity);
            if (!comp)
                continue;

            RemovalTarget target;
            target.Entity = knot.Entity;
            target.Handle = SplineECS::SplineHandle(comp->SplineDataIndex, comp->SplineDataGeneration);
            if (!splineService->GetSplineData(target.Handle))
                continue;

            targets.push_back(std::move(target));
            it = targets.end() - 1;
        }
        it->Indices.push_back(static_cast<uint32>(knot.PointIndex));
    }

    std::vector<RemovalTarget> removable;
    for (RemovalTarget& target : targets)
    {
        const auto* data = splineService->GetSplineData(target.Handle);
        if (!data)
            continue;
        const size_t pointCount = data->Points.size();

        std::sort(target.Indices.begin(), target.Indices.end(), std::greater<uint32>());
        target.Indices.erase(std::unique(target.Indices.begin(), target.Indices.end()),
                             target.Indices.end());
        target.Indices.erase(std::remove_if(target.Indices.begin(), target.Indices.end(),
                                            [pointCount](uint32 index) { return index >= pointCount; }),
                             target.Indices.end());
        if (target.Indices.empty())
            continue;

        // All-or-nothing per spline: erasing only the knots that fit under the
        // floor would leave the author guessing which ones survived.
        if (pointCount < Spline::SplineData::kMinPointCount + target.Indices.size())
        {
            Logger::Log::Info(
                "SplineTool: refused to delete {} of {} spline points - a spline keeps at least {}.",
                target.Indices.size(), pointCount, Spline::SplineData::kMinPointCount);
            continue;
        }

        target.LowestIndex = target.Indices.back();
        removable.push_back(std::move(target));
    }

    // A knot was selected, so Delete belongs to the spline either way: refusing
    // the removal must not fall through to deleting the whole entity.
    if (removable.empty())
        return true;

    const ECS::EntityHandle activeEntity =
        sel.Controls.empty() ? sel.Entity : sel.Controls.back().Entity;

    std::vector<std::pair<ECS::EntityHandle, SplineECS::SplineHandle>> undoSplines;
    undoSplines.reserve(removable.size());
    for (const auto& target : removable)
        undoSplines.emplace_back(target.Entity, target.Handle);

    const std::string editName =
        (removable.size() == 1u && removable.front().Indices.size() == 1u)
            ? std::string("Delete Spline Point")
            : std::string("Delete Spline Points");

    // The removal and what the recipes on these splines re-address because of
    // it undo as one step.
    if (m_UndoRedo)
        m_UndoRedo->BeginCompound(editName);

    UndoRedoService::InteractiveEdit edit;
    if (m_UndoRedo)
    {
        edit = m_UndoRedo->BeginInteractiveEdit(
            editName,
            undoSplines.size() <= 1u
                ? Editor::SplineUndo::MakeSplineEditableSnapshotTarget(
                      splineService, undoSplines.front().second, world, undoSplines.front().first,
                      m_ChangeNotifications, editName)
                : MakeMultiSplineEditableSnapshotTarget(
                      splineService, undoSplines, world, m_ChangeNotifications, editName));
    }

    ECS::EntityHandle survivorEntity{};
    uint32 survivorIndex = 0;
    std::vector<std::pair<ECS::EntityHandle, Editor::SplinePointRenumbering>> renumberings;
    renumberings.reserve(removable.size());

    for (const RemovalTarget& target : removable)
    {
        auto* data = splineService->GetSplineData(target.Handle);
        if (!data)
            continue;

        const uint32 countBefore = static_cast<uint32>(data->Points.size());
        for (uint32 index : target.Indices)
            (void)data->RemovePoint(index);
        splineService->RebuildCache(target.Handle);
        renumberings.emplace_back(target.Entity,
                                  Editor::SplinePointRenumbering::Removed(
                                      countBefore, target.Indices, data->IsEffectivelyClosed()));

        if (!survivorEntity.IsValid() || target.Entity == activeEntity)
        {
            survivorEntity = target.Entity;
            const uint32 remaining = static_cast<uint32>(data->Points.size());
            survivorIndex = (remaining == 0u) ? 0u : std::min(target.LowestIndex, remaining - 1u);
        }
    }

    if (edit)
    {
        edit.Commit();
    }
    else if (m_ChangeNotifications)
    {
        for (const RemovalTarget& target : removable)
        {
            m_ChangeNotifications->NotifyComponentChange<Components::SplineComponent>(
                world, target.Entity, Editor::EditorChangeNotifications::ChangeKind::Commit);
        }
    }
    for (const auto& [entity, renumbering] : renumberings)
        Editor::SplinePointEdited().Invoke({world, entity, m_UndoRedo, m_ChangeNotifications, &renumbering});
    if (m_UndoRedo)
        m_UndoRedo->EndCompound();

    // Land on the point that took the erased one's place — or the new last
    // point when the tail went — so repeated Delete walks the spline instead of
    // dropping the author out of point editing.
    sel.Controls.clear();
    if (survivorEntity.IsValid() && world->IsValid(survivorEntity))
        sel.Controls.push_back({survivorEntity, static_cast<int32>(survivorIndex), 1});
    SyncLegacySplineSelection(sel);

    // The point count changed, so the inspector needs new rows built, not the
    // existing ones refreshed.
    for (const RemovalTarget& target : removable)
        NotifySplineSelectionChanged(world, target.Entity);

    SyncTransformGizmoTarget();
    return true;
}

void SplineTool::UpdateHover(const ScenePointerEvent& event)
{
    auto& hover = m_State->Hover();
    hover.Kind = 0;
    hover.PointIndex = -1;
    hover.Entity = {};

    auto* world = &m_State->World();
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService)
        return;

    std::vector<ECS::EntityHandle> selectedEntities;
    if (m_SelectionListQuery)
        selectedEntities = m_SelectionListQuery();
    else if (m_SelectionQuery)
        selectedEntities.push_back(m_SelectionQuery());

    const auto& settings = Editor::SplineEditorSettings::Get();
    selectedEntities = CollectEditableSplineEntities(world, selectedEntities, settings.GetShowAllControls());
    if (selectedEntities.empty())
        return;

    const float32 knotBase   = std::max(settings.GetKnotSize(),   0.15f);
    const float32 handleBase = std::max(settings.GetHandleSize(), 0.15f);
    const bool constScreen = settings.GetConstantScreenSize();
    const bool smartDist = settings.GetSmartDistanceScaling();

    SplineControlPick best;
    for (const auto& entity : selectedEntities)
        FindBestControlOnSpline(world, splineService, entity, event, knotBase, handleBase,
                                constScreen, smartDist, best);

    if (best.PointIndex >= 0)
    {
        hover.Entity = best.Entity;
        hover.PointIndex = best.PointIndex;
        hover.Kind = best.Kind;
    }
}

bool SplineTool::TryBeginEditDrag(const ScenePointerEvent& event,
                                  bool hit,
                                  const Vector3& hitPos)
{
    (void)hit;
    (void)hitPos;
    if (event.phase != PointerPhase::Down || event.button != PointerButton::Left)
        return false;
    if (m_EditDragKind != 0)
        return false;

    auto* world = &m_State->World();
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService)
        return false;

    std::vector<ECS::EntityHandle> selectedEntities;
    if (m_SelectionListQuery)
        selectedEntities = m_SelectionListQuery();
    else if (m_SelectionQuery)
        selectedEntities.push_back(m_SelectionQuery());

    const auto& settings = Editor::SplineEditorSettings::Get();
    selectedEntities = CollectEditableSplineEntities(world, selectedEntities, settings.GetShowAllControls());
    if (selectedEntities.empty())
    {
        Logger::Log::Trace("SplineTool::EditDrag: no selected entity");
        return false;
    }

    const float32 knotBase   = std::max(settings.GetKnotSize(),   0.15f);
    const float32 handleBase = std::max(settings.GetHandleSize(), 0.15f);
    const bool constScreen = settings.GetConstantScreenSize();
    const bool smartDist = settings.GetSmartDistanceScaling();

    SplineControlPick best;
    for (const auto& entity : selectedEntities)
        FindBestControlOnSpline(world, splineService, entity, event, knotBase, handleBase,
                                constScreen, smartDist, best);

    if (best.PointIndex < 0)
    {
        Logger::Log::Trace("SplineTool::EditDrag: no knot hit (n={}, bestDist={})",
                           0, best.DistSq);
        SplineSelection& s = m_State->Selection();
        const ECS::EntityHandle previousEntity = s.Entity;
        s.Kind = 0;
        s.PointIndex = -1;
        s.KnotIndices.clear();
        s.Controls.clear();
        NotifySplineSelectionChanged(world, previousEntity);
        return false;
    }

    const bool additive = event.shift || event.ctrl;

    // Record selection so the scene gizmo can highlight the picked knot or
    // handle. Drag continues regardless — selection and drag coexist.
    SplineSelection& sel = m_State->Selection();
    const bool alreadySelected =
        ContainsControl(sel.Controls, best.Entity, best.Kind, best.PointIndex);
    {
        if (additive)
        {
            if (event.ctrl && !event.shift)
            {
                sel.Controls.erase(
                    std::remove_if(sel.Controls.begin(), sel.Controls.end(), [&](const SplineControlSelection& c) {
                        return SameControl(c, best.Entity, best.Kind, best.PointIndex);
                    }),
                    sel.Controls.end());
                SyncLegacySplineSelection(sel);
                NotifySplineSelectionChanged(world, best.Entity);
                return true;
            }

            if (event.shift && !event.ctrl && alreadySelected)
            {
                sel.Controls.erase(
                    std::remove_if(sel.Controls.begin(), sel.Controls.end(), [&](const SplineControlSelection& c) {
                        return SameControl(c, best.Entity, best.Kind, best.PointIndex);
                    }),
                    sel.Controls.end());
                SyncLegacySplineSelection(sel);
                NotifySplineSelectionChanged(world, best.Entity);
                return true;
            }

            if (!alreadySelected)
                sel.Controls.push_back({best.Entity, best.PointIndex, best.Kind});
        }
        else if (!alreadySelected || sel.Controls.size() <= 1u)
        {
            sel.Controls = {{best.Entity, best.PointIndex, best.Kind}};
        }

        if (sel.Controls.empty())
            sel.Controls.push_back({best.Entity, best.PointIndex, best.Kind});

        auto activeIt = std::find_if(sel.Controls.begin(), sel.Controls.end(), [&](const SplineControlSelection& c) {
            return SameControl(c, best.Entity, best.Kind, best.PointIndex);
        });
        if (activeIt != sel.Controls.end())
            std::rotate(activeIt, activeIt + 1, sel.Controls.end());
        SyncLegacySplineSelection(sel);
        NotifySplineSelectionChanged(world, best.Entity);
    }

    m_EditDragKind = best.Kind;
    m_EditDragPointIndex = best.PointIndex;
    m_EditDragEntity = best.Entity;
    m_SelectedPointIndex = best.PointIndex;
    m_EditDragControls.clear();
    m_EditDragAnchorWorld = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragAccumulatedWorldDelta = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragPlaneOrigin = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragPlaneNormal = Vector3(0.0f, 0.0f, 1.0f);
    m_EditDragPointerOffsetWorld = Vector3(0.0f, 0.0f, 0.0f);
    m_EditDragStartViewX = event.viewX;
    m_EditDragStartViewY = event.viewY;
    m_EditDragHasPlane = false;
    m_EditDragAwaitingMotion = true;

    std::vector<std::pair<ECS::EntityHandle, SplineECS::SplineHandle>> undoSplines;
    std::vector<ECS::EntityHandle> undoEntities;
    for (const auto& c : sel.Controls)
    {
        if (c.PointIndex < 0 || c.Kind == 0 || !c.Entity.IsValid() || !world->IsValid(c.Entity))
            continue;
        const auto* comp = world->GetComponent<Components::SplineComponent>(c.Entity);
        if (!comp || !ECS::Entity(world, c.Entity).IsEnabled<Components::SplineComponent>())
            continue;

        SplineECS::SplineHandle handle(comp->SplineDataIndex, comp->SplineDataGeneration);
        auto* data = splineService->GetSplineData(handle);
        if (!data || static_cast<uint32>(c.PointIndex) >= static_cast<uint32>(data->Points.size()))
            continue;

        EditDragControl dc{};
        dc.Entity = c.Entity;
        dc.Handle = handle;
        dc.PointIndex = c.PointIndex;
        dc.Kind = c.Kind;
        const auto& point = data->Points[static_cast<uint32>(c.PointIndex)];
        dc.StartPosition = point.Position;
        dc.StartTangentIn = point.TangentIn;
        dc.StartTangentOut = point.TangentOut;

        const auto* xf = world->GetComponent<Components::WorldTransform>(c.Entity);
        dc.HasTransform = xf != nullptr;
        if (xf)
        {
            dc.WorldMatrix = Mathematics::Matrix4x4::FromColumnMajor(xf->matrix);
            dc.InverseWorldMatrix = Mathematics::Inverse(dc.WorldMatrix);
        }
        dc.StartWorldPosition = dc.HasTransform ? dc.WorldMatrix.TransformPoint(point.Position) : point.Position;
        Vector3 localHandle = point.Position;
        if (c.Kind == 2)
            localHandle = point.Position + point.TangentIn;
        else if (c.Kind == 3)
            localHandle = point.Position + point.TangentOut;
        dc.StartWorldHandle = dc.HasTransform ? dc.WorldMatrix.TransformPoint(localHandle) : localHandle;
        if (SameControl(c, best.Entity, best.Kind, best.PointIndex))
            m_EditDragAnchorWorld = c.Kind == 1 ? dc.StartWorldPosition : dc.StartWorldHandle;

        if (std::find(undoEntities.begin(), undoEntities.end(), c.Entity) == undoEntities.end())
        {
            undoEntities.push_back(c.Entity);
            undoSplines.emplace_back(c.Entity, handle);
        }
        m_EditDragControls.push_back(dc);
    }

    if (m_EditDragControls.empty())
    {
        m_EditDragKind = 0;
        m_EditDragPointIndex = -1;
        m_EditDragEntity = {};
        m_EditDragAwaitingMotion = false;
        m_EditDragHasPlane = false;
        return false;
    }

    m_EditDragPlaneOrigin = m_EditDragAnchorWorld;
    m_EditDragPlaneNormal = event.cameraForward;
    if (!NormalizeVector(m_EditDragPlaneNormal))
    {
        m_EditDragPlaneNormal = Vector3(event.ray.direction.x, event.ray.direction.y, event.ray.direction.z);
        if (!NormalizeVector(m_EditDragPlaneNormal))
            m_EditDragPlaneNormal = Vector3(0.0f, 0.0f, 1.0f);
    }
    {
        float32 rayT = 0.0f;
        Vector3 planeHit;
        if (Mathematics::IntersectRayPlane(event.ray, m_EditDragPlaneOrigin, m_EditDragPlaneNormal, rayT, planeHit) &&
            rayT >= 0.0f)
        {
            m_EditDragPointerOffsetWorld = m_EditDragAnchorWorld - planeHit;
            m_EditDragHasPlane = true;
        }
    }

    m_SplineDragUndo = {};
    if (m_UndoRedo)
    {
        m_SplineDragUndo = m_UndoRedo->BeginInteractiveEdit(
            "Edit Spline",
            undoSplines.size() <= 1u
                ? Editor::SplineUndo::MakeSplineEditableSnapshotTarget(
                      splineService, undoSplines.front().second, world, undoSplines.front().first,
                      m_ChangeNotifications, "Edit Spline")
                : MakeMultiSplineEditableSnapshotTarget(
                      splineService, undoSplines, world, m_ChangeNotifications, "Edit Spline"));
    }
    return true;
}

bool SplineTool::UpdateEditDrag(const ScenePointerEvent& event,
                                bool hit,
                                const Vector3& hitPos)
{
    if (m_EditDragKind == 0)
        return false;

    auto* world = &m_State->World();
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService || m_EditDragControls.empty())
    {
        m_SplineDragUndo = {};
        m_EditDragKind = 0;
        m_EditDragPointIndex = -1;
        m_EditDragEntity = {};
        m_EditDragControls.clear();
        m_EditDragAccumulatedWorldDelta = Vector3(0.0f, 0.0f, 0.0f);
        m_EditDragHasPlane = false;
        m_EditDragAwaitingMotion = false;
        return false;
    }

    if (event.phase == PointerPhase::Move)
    {
        Vector3 currentAnchor = m_EditDragAnchorWorld;

        if (m_EditDragHasPlane)
        {
            float32 rayT = 0.0f;
            Vector3 planeHit;
            if (!Mathematics::IntersectRayPlane(event.ray, m_EditDragPlaneOrigin, m_EditDragPlaneNormal, rayT, planeHit) ||
                rayT < 0.0f)
            {
                return true;
            }
            currentAnchor = planeHit + m_EditDragPointerOffsetWorld;
        }
        else if (m_EditDragKind == 1) // knot
        {
            if (!hit)
                return true;
            currentAnchor = hitPos;
        }
        else
        {
            // Intersect ray with the horizontal plane through the active
            // handle's knot, then translate every selected handle endpoint by
            // the same world delta.
            auto activeIt = std::find_if(m_EditDragControls.begin(), m_EditDragControls.end(),
                [&](const EditDragControl& c) {
                    return c.Entity == m_EditDragEntity && c.Kind == m_EditDragKind &&
                           c.PointIndex == m_EditDragPointIndex;
                });
            if (activeIt == m_EditDragControls.end())
                return true;

            if (std::abs(event.ray.direction.y) > 1e-6f)
            {
                const float32 t = (activeIt->StartWorldPosition.y - event.ray.origin.y) /
                                  event.ray.direction.y;
                if (t >= 0.0f)
                {
                    currentAnchor = Vector3(event.ray.origin.x + event.ray.direction.x * t,
                                       activeIt->StartWorldPosition.y,
                                       event.ray.origin.z + event.ray.direction.z * t);
                }
                else
                    return true;
            }
            else
                return true;
        }

        if (m_EditDragAwaitingMotion)
        {
            constexpr float32 kStartDragThresholdPx = 3.0f;
            const float32 dx = event.viewX - m_EditDragStartViewX;
            const float32 dy = event.viewY - m_EditDragStartViewY;
            if (dx * dx + dy * dy < kStartDragThresholdPx * kStartDragThresholdPx)
                return true;

            m_EditDragAwaitingMotion = false;
        }

        const Vector3 deltaWorld = currentAnchor - m_EditDragAnchorWorld;
        std::vector<SplineECS::SplineHandle> rebuilt;
        for (const auto& dc : m_EditDragControls)
        {
            auto* data = splineService->GetSplineData(dc.Handle);
            if (!data || dc.PointIndex < 0 ||
                static_cast<uint32>(dc.PointIndex) >= static_cast<uint32>(data->Points.size()))
            {
                continue;
            }

            const uint32 i = static_cast<uint32>(dc.PointIndex);
            if (dc.Kind == 1)
            {
                const Vector3 worldPos = dc.StartWorldPosition + deltaWorld;
                const Vector3 localPos = dc.HasTransform ? dc.InverseWorldMatrix.TransformPoint(worldPos) : worldPos;
                data->SetPointPosition(i, localPos);
            }
            else
            {
                const Vector3 worldHandle = dc.StartWorldHandle + deltaWorld;
                const Vector3 localHandle =
                    dc.HasTransform ? dc.InverseWorldMatrix.TransformPoint(worldHandle) : worldHandle;
                const Vector3 offset = localHandle - data->Points[i].Position;
                Vector3 tangentIn = data->Points[i].TangentIn;
                Vector3 tangentOut = data->Points[i].TangentOut;
                if (dc.Kind == 2)
                {
                    tangentIn = offset;
                    tangentOut = Vector3(0.0f, 0.0f, 0.0f) - offset;
                }
                else
                {
                    tangentOut = offset;
                    tangentIn = Vector3(0.0f, 0.0f, 0.0f) - offset;
                }
                data->SetPointTangents(i, tangentIn, tangentOut);
            }

            if (std::find(rebuilt.begin(), rebuilt.end(), dc.Handle) == rebuilt.end())
            {
                splineService->RebuildCache(dc.Handle);
                rebuilt.push_back(dc.Handle);
            }
        }
        if (m_SplineDragUndo)
            m_SplineDragUndo.Preview([splineService, rebuilt]() {
                for (const auto& h : rebuilt)
                    splineService->RebuildCache(h);
            });
        else
        {
            NotifyEditedSplineControls(world, Editor::EditorChangeNotifications::ChangeKind::Preview);
        }
        return true;
    }

    if (event.phase == PointerPhase::Up)
    {
        // Close-loop-on-drag: releasing an endpoint knot near the opposite
        // endpoint of an open spline converts it into a closed loop. Mirrors
        // the stroke-time detection in FinalizeSpline so dragging matches the
        // authoring behavior the user already expects.
        // Closing the loop removes the dragged endpoint, which renumbers the
        // points after it for every recipe addressed by point.
        std::optional<Editor::SplinePointRenumbering> closedLoop;
        ECS::EntityHandle closedLoopEntity{};
        if (m_EditDragKind == 1 && m_EditDragControls.size() == 1u)
        {
            const auto& dc = m_EditDragControls.front();
            auto* data = splineService->GetSplineData(dc.Handle);
            if (data && !data->Closed && data->Points.size() >= 3)
            {
                const uint32 dragged = static_cast<uint32>(dc.PointIndex);
                const uint32 last = static_cast<uint32>(data->Points.size() - 1);
                const bool isEndpoint = (dragged == 0 || dragged == last);
                if (isEndpoint)
                {
                    const uint32 other = (dragged == 0) ? last : 0;
                    const Vector3 diff = data->Points[dragged].Position - data->Points[other].Position;
                    const float32 tol = Editor::SplineEditorSettings::Get().GetCloseLoopTolerance();
                    if (tol > 0.0f && diff.LengthSquared() <= tol * tol)
                    {
                        const uint32 countBefore = static_cast<uint32>(data->Points.size());
                        data->RemovePoint(dragged);
                        data->Closed = true;
                        data->MarkDirty();
                        splineService->RebuildCache(dc.Handle);
                        const uint32 removed[1] = {dragged};
                        closedLoop = Editor::SplinePointRenumbering::Removed(
                            countBefore, removed, data->IsEffectivelyClosed());
                        closedLoopEntity = dc.Entity;
                    }
                }
            }
        }

        if (closedLoop && m_UndoRedo)
            m_UndoRedo->BeginCompound("Close Spline Loop");
        if (m_SplineDragUndo)
            m_SplineDragUndo.Commit();
        m_SplineDragUndo = {};
        if (closedLoop)
        {
            Editor::SplinePointEdited().Invoke(
                {world, closedLoopEntity, m_UndoRedo, m_ChangeNotifications, &*closedLoop});
            if (m_UndoRedo)
                m_UndoRedo->EndCompound();
        }

        m_EditDragKind = 0;
        m_EditDragPointIndex = -1;
        m_EditDragEntity = {};
        m_EditDragControls.clear();
        m_EditDragAccumulatedWorldDelta = Vector3(0.0f, 0.0f, 0.0f);
        m_EditDragHasPlane = false;
        m_EditDragAwaitingMotion = false;

        if (m_ChangeNotifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = world;
            e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            m_ChangeNotifications->NotifyWorldStructureChanged(e);
        }
        return true;
    }

    return true;
}

bool SplineTool::TryExtendExistingSpline(const std::vector<Vector3>& stroke, float32 toleranceSq)
{
    if (stroke.size() < 2)
        return false;

    auto* world = &m_State->World();
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!world || !splineService)
        return false;

    const Vector3 strokeStart = stroke.front();
    const Vector3 strokeEnd   = stroke.back();

    struct Match
    {
        ECS::EntityHandle entity{};
        SplineECS::SplineHandle handle;
        Mathematics::Matrix4x4 worldMatrix{};
        Mathematics::Matrix4x4 inverseWorldMatrix{};
        bool hasTransform = false;
        // Which endpoint of the target spline matched:
        // 0 = target start (stroke prepends), 1 = target end (stroke appends).
        int targetSide = 0;
        // Which endpoint of the stroke matched the target:
        // 0 = stroke start, 1 = stroke end.
        int strokeSide = 0;
        float32 distSq = std::numeric_limits<float32>::max();
    };
    Match best;

    world->Query<ECS::Read<Components::SplineComponent>>()
        .Each([&](ECS::EntityHandle e, const Components::SplineComponent& comp)
        {
            SplineECS::SplineHandle h(comp.SplineDataIndex, comp.SplineDataGeneration);
            const auto* data = splineService->GetSplineData(h);
            if (!data || !data->IsValid() || data->Closed)
                return;
            Mathematics::Matrix4x4 worldM;
            Mathematics::Matrix4x4 inverseWorldM;
            bool hasTransform = false;
            if (const auto* xf = world->GetComponent<Components::WorldTransform>(e))
            {
                worldM = Mathematics::Matrix4x4::FromColumnMajor(xf->matrix);
                inverseWorldM = Mathematics::Inverse(worldM);
                hasTransform = true;
            }

            const Vector3 targetStart = hasTransform
                ? worldM.TransformPoint(data->Points.front().Position)
                : data->Points.front().Position;
            const Vector3 targetEnd = hasTransform
                ? worldM.TransformPoint(data->Points.back().Position)
                : data->Points.back().Position;

            auto check = [&](int targetSide, const Vector3& targetPos)
            {
                auto considered = [&](int strokeSide, const Vector3& sp)
                {
                    Vector3 d = sp - targetPos;
                    float32 dsq = d.LengthSquared();
                    if (dsq <= toleranceSq && dsq < best.distSq)
                    {
                        best.entity = e;
                        best.handle = h;
                        best.worldMatrix = worldM;
                        best.inverseWorldMatrix = inverseWorldM;
                        best.hasTransform = hasTransform;
                        best.targetSide = targetSide;
                        best.strokeSide = strokeSide;
                        best.distSq = dsq;
                    }
                };
                considered(0, strokeStart);
                considered(1, strokeEnd);
            };
            check(0, targetStart);
            check(1, targetEnd);
        });

    if (!best.entity.IsValid())
        return false;

    if (!splineService->GetSplineData(best.handle))
        return false;

    // Assemble the ordered list of points to append. If the matching stroke
    // side is the END, we reverse the stroke so the appended sequence starts
    // at the target endpoint. Skip the first element of the sequence since
    // it duplicates the target endpoint itself.
    std::vector<Vector3> toAppend;
    toAppend.reserve(stroke.size());
    if (best.strokeSide == 0)
        toAppend.assign(stroke.begin(), stroke.end());
    else
        toAppend.assign(stroke.rbegin(), stroke.rend());

    // Extending an EXISTING spline uses that spline's own DefaultRadius — the value
    // the spline inspector's "Default Radius" field edits — so a per-spline width
    // survives a later stroke. The editor-wide setting seeds only a NEW spline.
    const auto* targetSpline = world->GetComponent<Components::SplineComponent>(best.entity);
    const float32 defaultRadius = targetSpline
                                      ? targetSpline->DefaultRadius
                                      : Editor::SplineEditorSettings::Get().GetDefaultRadius();

    // Extending at the start inserts the new points before every existing one,
    // which renumbers them for every recipe addressed by point; appending at
    // the end renumbers nothing.
    const auto* extended = splineService->GetSplineData(best.handle);
    const uint32 countBefore = extended ? static_cast<uint32>(extended->Points.size()) : 0u;
    const uint32 prepended = best.targetSide == 1 || toAppend.size() < 2u
                                 ? 0u
                                 : static_cast<uint32>(toAppend.size() - 1u);
    if (prepended > 0u && m_UndoRedo)
        m_UndoRedo->BeginCompound("Extend Spline");

    Editor::SplineUndo::CommitSplineDataOneShot(
        splineService,
        best.handle,
        world,
        best.entity,
        m_ChangeNotifications,
        m_UndoRedo,
        "Extend Spline",
        [best, defaultRadius, toAppend = std::move(toAppend)](GameEngine::Spline::SplineData* data) {
            if (best.targetSide == 1) // append after last point
            {
                for (size_t i = 1; i < toAppend.size(); ++i)
                {
                    const Vector3 localPoint = best.hasTransform
                        ? best.inverseWorldMatrix.TransformPoint(toAppend[i])
                        : toAppend[i];
                    data->AddPoint(localPoint, defaultRadius);
                }
            }
            else
            {
                for (size_t i = 1; i < toAppend.size(); ++i)
                {
                    const Vector3 p = best.hasTransform
                        ? best.inverseWorldMatrix.TransformPoint(toAppend[i])
                        : toAppend[i];
                    data->InsertPoint(0, p, defaultRadius);
                }
            }

            if (data->Type == Spline::SplineType::CubicBezier)
            {
                const uint32 n = static_cast<uint32>(data->Points.size());
                std::vector<Vector3> positions;
                positions.reserve(n);
                for (uint32 i = 0; i < n; ++i)
                    positions.push_back(data->Points[i].Position);
                for (uint32 i = 0; i < n; ++i)
                {
                    Vector3 t = CenteredDiffTangent(positions, i) * kAutoBezierTangentScale;
                    Vector3 tNeg = t * -1.0f;
                    data->SetPointTangents(i, tNeg, t);
                }
            }
        });

    if (prepended > 0u)
    {
        const auto* after = splineService->GetSplineData(best.handle);
        const Editor::SplinePointRenumbering renumbering = Editor::SplinePointRenumbering::Inserted(
            countBefore, 0u, prepended, after && after->IsEffectivelyClosed());
        Editor::SplinePointEdited().Invoke(
            {world, best.entity, m_UndoRedo, m_ChangeNotifications, &renumbering});
        if (m_UndoRedo)
            m_UndoRedo->EndCompound();
    }

    if (m_ChangeNotifications)
    {
        Editor::EditorChangeNotifications::WorldStructureChangedEvent ev{};
        ev.world = world;
        ev.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
        m_ChangeNotifications->NotifyWorldStructureChanged(ev);
    }

    return true;
}

void SplineTool::SmoothPolyline(std::vector<Vector3>& points, int32 iterations)
{
    if (iterations <= 0 || points.size() < 3)
        return;

    // Chaikin corner-cutting: replaces each interior segment with two points
    // at 1/4 and 3/4 along it. Endpoints are preserved so the spline still
    // terminates at the original start / end positions.
    for (int32 it = 0; it < iterations; ++it)
    {
        std::vector<Vector3> next;
        next.reserve(points.size() * 2);
        next.push_back(points.front());
        for (size_t i = 0; i + 1 < points.size(); ++i)
        {
            const Vector3& a = points[i];
            const Vector3& b = points[i + 1];
            next.push_back(a * 0.75f + b * 0.25f);
            next.push_back(a * 0.25f + b * 0.75f);
        }
        next.push_back(points.back());
        points = std::move(next);
    }
}

void SplineTool::SnapPointsToSurface(std::vector<Vector3>& points)
{
    if (points.empty())
        return;

    // Drop from the stroke's own Y bounds + a safety margin so the ray starts
    // above any geometry the original samples came from.
    float32 yMax = points.front().y;
    for (const auto& p : points)
        yMax = std::max(yMax, p.y);
    const float32 dropHeight = yMax + 100.0f;

    for (auto& point : points)
    {
        GizmoRay downRay;
        downRay.origin    = {point.x, dropHeight, point.z};
        downRay.direction = {0.0f, -1.0f, 0.0f};
        Vector3 snapped;
        if (RaycastScene(downRay, snapped))
            point = snapped;
    }
}

bool SplineTool::BeginMarquee(const ScenePointerEvent& event,
                              bool hit,
                              const Vector3& hitPos)
{
    (void)hit;
    // Build a click plane through the cursor hit position (or ray origin +
    // some distance along the ray if no hit) that faces the camera. Knot
    // inclusion is tested in this plane's 2D basis.
    Vector3 planePoint;
    if (hit)
    {
        planePoint = hitPos;
    }
    else
    {
        const Vector3 rd(event.ray.direction.x, event.ray.direction.y, event.ray.direction.z);
        const Vector3 ro(event.ray.origin.x,    event.ray.origin.y,    event.ray.origin.z);
        planePoint = ro + rd * 50.0f;
    }

    const Vector3 rd(event.ray.direction.x, event.ray.direction.y, event.ray.direction.z);
    // Plane normal faces the camera (opposite of ray direction).
    Vector3 n = rd * -1.0f;
    const float nLenSq = n.LengthSquared();
    if (nLenSq < 1.0e-8f)
        return false;
    n = n * (1.0f / std::sqrt(nLenSq));

    // Orthonormal basis (u, v) on the plane.
    Vector3 u;
    Vector3 v;
    BuildPlaneBasis(n, u, v);

    m_MarqueeActive = true;
    m_MarqueeStart = planePoint;
    m_MarqueeCurrent = planePoint;
    m_MarqueePlaneU = u;
    m_MarqueePlaneV = v;
    m_MarqueePlaneNormal = n;
    m_MarqueeLassoPoints.clear();
    m_MarqueeLassoPoints.push_back(planePoint);

    const Editor::SplineSelectionShape shape = Editor::SplineEditorSettings::Get().GetSelectionShape();
    m_Gizmo.SetMarquee(true,
                       shape == Editor::SplineSelectionShape::Lasso ? 1 : 0,
                       m_MarqueeStart,
                       m_MarqueeCurrent,
                       m_MarqueeLassoPoints,
                       m_MarqueePlaneU,
                       m_MarqueePlaneV);
    return true;
}

void SplineTool::UpdateMarquee(const ScenePointerEvent& event,
                               bool hit,
                               const Vector3& hitPos)
{
    (void)hit;
    (void)hitPos;
    if (!m_MarqueeActive)
        return;

    float rayT = 0.0f;
    Vector3 cur;
    if (!Mathematics::IntersectRayPlane(event.ray, m_MarqueeStart, m_MarqueePlaneNormal, rayT, cur) ||
        rayT < 0.0f)
        return;
    m_MarqueeCurrent = cur;

    const Editor::SplineSelectionShape shape = Editor::SplineEditorSettings::Get().GetSelectionShape();
    if (shape == Editor::SplineSelectionShape::Lasso)
    {
        if (m_MarqueeLassoPoints.empty() ||
            Vector3::Dot(cur - m_MarqueeLassoPoints.back(), cur - m_MarqueeLassoPoints.back()) > 0.0625f)
        {
            m_MarqueeLassoPoints.push_back(cur);
        }
    }

    m_Gizmo.SetMarquee(true,
                       shape == Editor::SplineSelectionShape::Lasso ? 1 : 0,
                       m_MarqueeStart,
                       m_MarqueeCurrent,
                       m_MarqueeLassoPoints,
                       m_MarqueePlaneU,
                       m_MarqueePlaneV);
}

void SplineTool::FinishMarquee()
{
    if (!m_MarqueeActive)
        return;

    auto* world = &m_State->World();

    // Gather knots of the currently selected spline into the plane basis and
    // run a point-in-shape test.
    std::vector<int32> picked;
    ECS::EntityHandle targetEntity;

    if (m_SelectionQuery)
    {
        auto selected = m_SelectionQuery();
        auto* splineService = SplineECS::SplineService::TryGet();
        if (selected.IsValid() && world && splineService && world->IsValid(selected))
        {
            const auto* comp = world->GetComponent<Components::SplineComponent>(selected);
            if (comp && ECS::Entity(world, selected).IsEnabled<Components::SplineComponent>())
            {
                SplineECS::SplineHandle handle(comp->SplineDataIndex, comp->SplineDataGeneration);
                const auto* data = splineService->GetSplineData(handle);
                if (data && data->IsValid())
                {
                    targetEntity = selected;

                    const Editor::SplineSelectionShape shape =
                        Editor::SplineEditorSettings::Get().GetSelectionShape();

                    // Compute rectangle bounds in plane basis.
                    const Vector3 d = m_MarqueeCurrent - m_MarqueeStart;
                    const float u0 = 0.0f;
                    const float v0 = 0.0f;
                    const float u1 = Vector3::Dot(d, m_MarqueePlaneU);
                    const float v1 = Vector3::Dot(d, m_MarqueePlaneV);
                    const float uMin = std::min(u0, u1);
                    const float uMax = std::max(u0, u1);
                    const float vMin = std::min(v0, v1);
                    const float vMax = std::max(v0, v1);

                    // Precompute lasso polygon in plane basis.
                    std::vector<Mathematics::Vector2> lassoPoly;
                    if (shape == Editor::SplineSelectionShape::Lasso)
                    {
                        lassoPoly.reserve(m_MarqueeLassoPoints.size());
                        for (const auto& p : m_MarqueeLassoPoints)
                        {
                            const Vector3 rel = p - m_MarqueeStart;
                            lassoPoly.emplace_back(Vector3::Dot(rel, m_MarqueePlaneU),
                                                   Vector3::Dot(rel, m_MarqueePlaneV));
                        }
                    }

                    const auto& pts = data->Points;
                    for (size_t i = 0; i < pts.size(); ++i)
                    {
                        // Project knot onto the click plane along the plane normal.
                        const Vector3 rel = pts[i].Position - m_MarqueeStart;
                        const float pu = Vector3::Dot(rel, m_MarqueePlaneU);
                        const float pv = Vector3::Dot(rel, m_MarqueePlaneV);

                        bool include = false;
                        if (shape == Editor::SplineSelectionShape::Rectangle)
                            include = (pu >= uMin && pu <= uMax && pv >= vMin && pv <= vMax);
                        else
                            include = Mathematics::PointInPolygon(Mathematics::Vector2(pu, pv), lassoPoly);

                        if (include)
                            picked.push_back(static_cast<int32>(i));
                    }
                }
            }
        }
    }

    SplineSelection& sel = m_State->Selection();
    const ECS::EntityHandle previousEntity = sel.Entity;
    if (!picked.empty() && targetEntity.IsValid())
    {
        sel.Entity = targetEntity;
        sel.KnotIndices = std::move(picked);
        sel.Controls.clear();
        sel.Controls.reserve(sel.KnotIndices.size());
        for (int32 idx : sel.KnotIndices)
            sel.Controls.push_back({targetEntity, idx, 1});
        sel.Kind = 1;
        sel.PointIndex = sel.KnotIndices.front();
        NotifySplineSelectionChanged(world, targetEntity);
    }
    else
    {
        sel.Kind = 0;
        sel.PointIndex = -1;
        sel.KnotIndices.clear();
        sel.Controls.clear();
        NotifySplineSelectionChanged(world, previousEntity);
    }

    CancelMarquee();
}

void SplineTool::CancelMarquee()
{
    m_MarqueeActive = false;
    m_MarqueeLassoPoints.clear();
    m_Gizmo.SetMarquee(false, 0, m_MarqueeStart, m_MarqueeCurrent,
                       m_MarqueeLassoPoints, m_MarqueePlaneU, m_MarqueePlaneV);
}

} // namespace GameEngine::Editor::SceneTools
