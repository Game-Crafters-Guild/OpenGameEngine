#include "Markups/MarkupTool.h"

#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "Markups/MarkupDrawList.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupRequests.h"
#include "Mathematics/Ray.h"
#include "Picking/MeshPickingService.h"
#include "SceneView/SceneViewDropPlacement.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "SceneView/SceneViewProjection.h"
#include "SceneView/SplineStrokeCapture.h"
#include "SceneViewController.h"
#include "Spline/SplineUtility.h"
#include "Types/Color.h"
#include "UI/ContextMenuLabels.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace GameEngine::Editor
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{
// A new mark-up's half size in meters: a 20 m square, 8 m tall, standing on the ground; a sphere
// 20 m across.
constexpr float kNewHalfWidth = 10.0f;
constexpr float kNewHalfHeight = 4.0f;
// The lasso keeps a sample once the pointer is this far from the last one (the spline brush's).
constexpr float kLassoSpacing = 1.0f;
// The lasso's simplification: this fraction of its bounding diagonal, within these bounds.
constexpr float kLassoToleranceFraction = 0.01f;
constexpr float kLassoMinTolerance = 0.5f;
constexpr float kLassoMaxTolerance = 5.0f;
// A click this many pixels from the first knot closes the clicked outline.
constexpr float kCloseClickPixels = 12.0f;
constexpr const char* kCrossingNotice = "The outline crosses itself; draw it again without crossing";
const Color kPreviewColor(kMarkupAccentRgb[0], kMarkupAccentRgb[1], kMarkupAccentRgb[2], 1.0f);
// The preview rides this far above the ground it was drawn on, so the terrain between its samples
// does not hide it.
constexpr float kPreviewLift = 0.3f;

MarkupToolShape& ChosenShape()
{
    static MarkupToolShape shape = MarkupToolShape::Box;
    return shape;
}

MarkupRequestContext UserContext(MarkupEditorBridge& bridge, ECS::World& world, UndoRedoService* undo,
                                 EditorChangeNotifications* notifications)
{
    MarkupRequestContext context;
    context.World = &world;
    context.Bridge = &bridge;
    context.Undo = undo;
    context.Notifications = notifications;
    context.PlayMode = bridge.IsInPlayMode();
    return context;
}

Vector2 GroundOf(const Vector3& point) { return Vector2(point.x, point.z); }

} // namespace

std::vector<ContextMenuManipulator::Item> BuildMarkupToolMenuItems(const OpenColorPickerWindowFn&)
{
    const struct
    {
        const char* Label;
        MarkupToolShape Shape;
    } kShapes[] = {
        {"Box: click to place", MarkupToolShape::Box},
        {"Sphere: click to place", MarkupToolShape::Sphere},
        {"Region: drag a lasso or click its points", MarkupToolShape::Region},
    };
    std::vector<ContextMenuManipulator::Item> items;
    for (const auto& shape : kShapes)
        items.push_back({.Path = shape.Label,
                         .Flags = ContextMenuLabels::CheckedFlag(GetMarkupToolShape() == shape.Shape),
                         .OnActivate = [value = shape.Shape] { SetMarkupToolShape(value); }});
    return items;
}

MarkupToolShape GetMarkupToolShape()
{
    return ChosenShape();
}

void SetMarkupToolShape(MarkupToolShape shape)
{
    ChosenShape() = shape;
}

nlohmann::json PlaceMarkup(MarkupEditorBridge& bridge, ECS::World& world, UndoRedoService* undo,
                           EditorChangeNotifications* notifications, const Vector3& ground, MarkupToolShape shape)
{
    const bool sphere = shape == MarkupToolShape::Sphere;
    const float halfHeight = sphere ? kNewHalfWidth : kNewHalfHeight;
    const nlohmann::json params{{"title", "Mark-up"},
                                {"shape", sphere ? "sphere" : "box"},
                                {"center", {ground.x, ground.y + halfHeight, ground.z}},
                                {"halfExtents", {kNewHalfWidth, halfHeight, kNewHalfWidth}},
                                {"status", "Requested"},
                                {"author", "User"}};
    return CreateMarkup(UserContext(bridge, world, undo, notifications), params);
}

nlohmann::json PlaceRegion(MarkupEditorBridge& bridge, ECS::World& world, UndoRedoService* undo,
                           EditorChangeNotifications* notifications, std::span<const Vector2> knots)
{
    nlohmann::json outline = nlohmann::json::array();
    for (const Vector2& knot : knots)
        outline.push_back({knot.x, knot.y});
    const nlohmann::json params{{"title", "Region"},
                                {"kind", "region"},
                                {"outline", std::move(outline)},
                                {"status", "Requested"},
                                {"author", "User"}};
    return CreateMarkup(UserContext(bridge, world, undo, notifications), params);
}

std::vector<Vector2> SimplifyLassoStroke(std::span<const Vector3> stroke)
{
    if (stroke.size() < 2)
        return {};
    // Simplified on the ground plane, the loop closed back to its first sample.
    std::vector<Vector3> loop;
    loop.reserve(stroke.size() + 1);
    Vector2 min = GroundOf(stroke.front());
    Vector2 max = min;
    for (const Vector3& sample : stroke)
    {
        loop.emplace_back(sample.x, 0.0f, sample.z);
        min = Vector2(std::min(min.x, sample.x), std::min(min.y, sample.z));
        max = Vector2(std::max(max.x, sample.x), std::max(max.y, sample.z));
    }
    loop.push_back(loop.front());
    float tolerance = std::clamp(kLassoToleranceFraction * (max - min).Length(), kLassoMinTolerance, kLassoMaxTolerance);
    std::vector<Vector3> kept;
    for (;;)
    {
        Spline::SimplifyPolyline(loop, tolerance, kept);
        // The closing sample repeats the first.
        if (kept.size() > 1)
            kept.pop_back();
        if (kept.size() <= MarkupECS::kMaxRegionKnots)
            break;
        tolerance *= 2.0f;
    }
    std::vector<Vector2> knots;
    knots.reserve(kept.size());
    for (const Vector3& point : kept)
        knots.push_back(GroundOf(point));
    return knots;
}

std::optional<Vector3> ResolveMarkupRegionGround(const Mathematics::Ray3D& ray, ECS::World& world)
{
    Picking::PickOptions options;
    options.IncludeMeshes = false;
    options.IncludePrimitives = false;
    options.IncludeBoundsFallback = false;
    const Picking::PickResult terrain = Picking::RaycastScene(ray, world, options);
    if (terrain.Hit)
        return terrain.Best.WorldPosition;
    float t = 0.0f;
    Vector3 onPlane;
    if (Mathematics::IntersectRayPlane(ray, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), t, onPlane) && t >= 0.0f)
        return onPlane;
    return std::nullopt;
}

MarkupTool::MarkupTool(SceneViewController& owner, MarkupEditorBridge& bridge)
    : m_Owner(owner), m_Bridge(bridge)
{
}

void MarkupTool::OnPointerEvent(const SceneTools::ScenePointerEvent& event)
{
    if (event.alt)
        return;
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return;
    const Mathematics::Ray3D ray{event.ray.origin, event.ray.direction};
    if (GetMarkupToolShape() == MarkupToolShape::Region)
    {
        if (const std::optional<Vector3> ground = ResolveMarkupRegionGround(ray, *world))
            DrawRegion(event, *ground);
        return;
    }
    const SceneViewDropPoint point = ResolveSceneViewDropPoint(ray, *world);
    if (event.phase != SceneTools::PointerPhase::Down || event.button != SceneTools::PointerButton::Left ||
        point.OnPendingMesh)
        return;
    PlaceAt(point.Position);
}

void MarkupTool::PlaceAt(const Vector3& ground)
{
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    const nlohmann::json result = PlaceMarkup(m_Bridge, *world, m_Owner.GetUndoRedoService(),
                                              m_Owner.GetChangeNotifications(), ground, GetMarkupToolShape());
    if (IsRefusal(result))
    {
        if (m_Bridge.IsInPlayMode())
            ShowSceneViewNotice(std::string(MarkupEditorBridge::kPlayModeNotice));
        Logger::Log::Warning("Mark-up tool: {}", result.dump());
        return;
    }
    const ECS::EntityHandle entity(result.value("entityId", 0u));
    m_Owner.OnEntityPicked(entity);
    m_Owner.SetActiveTool(SceneViewController::ToolKind::Transform);
}

void MarkupTool::DrawRegion(const SceneTools::ScenePointerEvent& event, const Vector3& ground)
{
    m_Pointer = ground;
    const bool left = event.button == SceneTools::PointerButton::Left;
    if (event.phase == SceneTools::PointerPhase::Down && left)
    {
        m_Pressed = true;
        m_Dragged = false;
        m_Stroke.clear();
        SceneTools::CaptureSplineStroke(m_Stroke, ground, kLassoSpacing);
        return;
    }
    if (event.phase == SceneTools::PointerPhase::Move && m_Pressed)
    {
        // A drag paints the lasso; the clicked knots give way to it.
        if (SceneTools::CaptureSplineStroke(m_Stroke, ground, kLassoSpacing))
        {
            m_Dragged = true;
            m_Knots.clear();
        }
        return;
    }
    if (event.phase != SceneTools::PointerPhase::Up || !m_Pressed || !left)
        return;
    m_Pressed = false;
    if (m_Dragged)
    {
        SceneTools::EndSplineStroke(m_Stroke, ground);
        const std::vector<Vector2> knots = SimplifyLassoStroke(m_Stroke);
        m_Stroke.clear();
        FinishRegion(knots);
        return;
    }
    m_Stroke.clear();
    // A click places a knot, or closes the outline on its first knot.
    if (m_Knots.size() >= MarkupECS::kMinRegionKnots && NearFirstKnot(event, m_Knots.front()))
    {
        std::vector<Vector2> knots;
        for (const Vector3& knot : m_Knots)
            knots.push_back(GroundOf(knot));
        m_Knots.clear();
        FinishRegion(knots);
        return;
    }
    m_Knots.push_back(ground);
}

bool MarkupTool::NearFirstKnot(const SceneTools::ScenePointerEvent& event, const Vector3& point) const
{
    Vector2 pixel;
    if (!SceneTools::ProjectWorldToView(event, point, pixel))
        return false;
    const Vector2 offset(pixel.x - event.viewX, pixel.y - event.viewY);
    return offset.Length() <= kCloseClickPixels;
}

void MarkupTool::FinishRegion(const std::vector<Vector2>& knots)
{
    if (knots.size() < MarkupECS::kMinRegionKnots)
        return;
    if (MarkupECS::CheckRegionOutline(knots))
    {
        ShowSceneViewNotice(kCrossingNotice);
        return;
    }
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return;
    const nlohmann::json result =
        PlaceRegion(m_Bridge, *world, m_Owner.GetUndoRedoService(), m_Owner.GetChangeNotifications(), knots);
    if (IsRefusal(result))
    {
        ShowSceneViewNotice(m_Bridge.IsInPlayMode() ? std::string(MarkupEditorBridge::kPlayModeNotice)
                                                    : result.value("reason", std::string(kCrossingNotice)));
        Logger::Log::Warning("Mark-up tool: {}", result.dump());
        return;
    }
    // The new region is selected and its knots show for editing.
    m_Owner.OnEntityPicked(ECS::EntityHandle(result.value("entityId", 0u)));
    m_Owner.SetActiveRegisteredTool("spline");
}

void MarkupTool::CancelRegion()
{
    m_Stroke.clear();
    m_Knots.clear();
    m_Pressed = false;
    m_Dragged = false;
}

void MarkupTool::OnKeyEvent(const SceneTools::SceneKeyEvent& event)
{
    if (!event.pressed || GetMarkupToolShape() != MarkupToolShape::Region)
        return;
    const int key = static_cast<int>(event.keyCode);
    if (key == Input::kKeyCode_Escape)
    {
        CancelRegion();
        return;
    }
    if ((key == Input::kKeyCode_Enter || key == Input::kKeyCode_NumPadEnter) &&
        m_Knots.size() >= MarkupECS::kMinRegionKnots)
    {
        std::vector<Vector2> knots;
        for (const Vector3& knot : m_Knots)
            knots.push_back(GroundOf(knot));
        m_Knots.clear();
        FinishRegion(knots);
    }
}

void MarkupTool::GatherGizmos(SceneTools::GizmoCollector& collector)
{
    if (!m_Stroke.empty() || !m_Knots.empty())
        collector.AddGizmo(this);
}

void MarkupTool::Render(SceneTools::GizmoRenderContext& context)
{
    // The lasso while it is painted, or the clicked knots with the pointer as the next one; both
    // close back to their first point, as the region will.
    std::vector<Vector3> outline = m_Stroke.empty() ? m_Knots : m_Stroke;
    if (m_Stroke.empty() && !m_Knots.empty())
        outline.push_back(m_Pointer);
    if (outline.size() < 2)
        return;
    std::vector<Vector3> lines;
    lines.reserve(outline.size() * 2);
    const Vector3 lift(0.0f, kPreviewLift, 0.0f);
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        lines.push_back(outline[i] + lift);
        lines.push_back(outline[(i + 1) % outline.size()] + lift);
    }
    context.DrawColoredLines(lines.data(), lines.size() / 2, kPreviewColor, 1.0f);
    for (const Vector3& knot : m_Knots)
        context.DrawWireCircle(knot + lift, Vector3(0.0f, 1.0f, 0.0f), 0.75f, kPreviewColor, 1.0f);
}

} // namespace GameEngine::Editor
