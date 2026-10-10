// Selection gizmo and selection tool implementations for Scene View.

#include "SceneView/SelectionTool.h"
#include "SceneViewController.h"

#include <cmath>
#include <vector>

#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Types/Color.h"
#include "Types/ColorUtils.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"

namespace GameEngine {
namespace Editor {
namespace SceneTools {

using GameEngine::Components::LocalBounds;
using GameEngine::Components::WorldTransform;
using GameEngine::Mathematics::Vector3;

namespace
{
// Wire box of the entity's LocalBounds (the default box when it has none),
// placed by its WorldTransform. Returns false when it has no WorldTransform.
bool DrawEntityBoundsBox(GizmoRenderContext& context,
                         GameEngine::ECS::World& world,
                         GameEngine::ECS::EntityHandle entity,
                         const Color& color,
                         float thickness)
{
    const auto* worldXf = world.GetComponent<WorldTransform>(entity);
    if (!worldXf)
        return false;
    const auto* bounds = world.GetComponent<LocalBounds>(entity);
    const GameEngine::Mathematics::BoundingBox kDefault{};
    const auto& bb = bounds ? bounds->Box : kDefault;
    const auto aabb = bb.ToAABB();
    context.DrawTransformedWireBox(aabb.min, aabb.max, Mathematics::Matrix4x4::FromColumnMajor(worldXf->matrix),
                                   color, thickness);
    return true;
}
} // namespace

// --- SelectionGizmo -------------------------------------------------------

void SelectionGizmo::SetSelection(GameEngine::ECS::EntityHandle entity)
{
    m_SelectedEntities.clear();
    if (entity.IsValid())
    {
        m_SelectedEntities.push_back(entity);
    }
}

void SelectionGizmo::SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_SelectedEntities = entities;
}

void SelectionGizmo::SetDescendants(std::vector<GameEngine::ECS::EntityHandle> descendants)
{
    m_SelectionDescendants = std::move(descendants);
}

void SelectionGizmo::Clear()
{
    m_SelectedEntities.clear();
    m_SelectionDescendants.clear();
}

void SelectionGizmo::Render(GizmoRenderContext& context)
{
    if (m_SelectedEntities.empty())
        return;

    const auto& settings = SceneViewSettings::Get();
    if (settings.GetSelectionHighlightStyle() == SelectionHighlightStyle::Outline)
        return;

    GameEngine::ECS::World* world = context.GetWorld();
    if (!world)
        return;

    const Color kColor = ColorUtils::UnpackArgb(settings.GetSelectionBoxColor());
    const float kThickness = settings.GetSelectionBoxThickness();

    for (auto it = m_SelectedEntities.begin(); it != m_SelectedEntities.end(); )
    {
        if (!it->IsValid() || !world->IsValid(*it) || !DrawEntityBoundsBox(context, *world, *it, kColor, kThickness))
            it = m_SelectedEntities.erase(it);
        else
            ++it;
    }

    // Draw descendants of selected entities (same color, thinner box).
    const Color kDescColor(kColor.r, kColor.g, kColor.b, kColor.a * 0.55f);
    const float kDescThickness = kThickness * 0.66f;

    for (const auto& descendant : m_SelectionDescendants)
    {
        if (descendant.IsValid() && world->IsValid(descendant))
            DrawEntityBoundsBox(context, *world, descendant, kDescColor, kDescThickness);
    }
}

// --- MarqueeGizmo ---------------------------------------------------------

void MarqueeGizmo::SetRect(const Mathematics::Vector3& start, const Mathematics::Vector3& current,
                           const Mathematics::Vector3& planeU, const Mathematics::Vector3& planeV)
{
    m_Start   = start;
    m_Current = current;
    m_PlaneU  = planeU;
    m_PlaneV  = planeV;
}

void MarqueeGizmo::SetLasso(const std::vector<Mathematics::Vector3>& points)
{
    m_Lasso = points;
}

void MarqueeGizmo::Render(GizmoRenderContext& context)
{
    if (!m_Active)
        return;

    const Color& kColor = m_Color;
    const float  kThickness = m_Thickness;

    // The marquee is a screen-space rectangle parked on a plane a fixed
    // distance in front of the camera; that distance is a construction detail,
    // not a place in the scene. Depth-testing it dims the box wherever geometry
    // sits nearer than the plane, which reads as a patchy, half-drawn selection.
    // Covers both DrawThickLine paths — above the thickness threshold it emits
    // triangles, below it lines.
    GizmoDepthModeScope depthScope(context, GizmoDepthMode::AlwaysOnTop);

    if (m_Shape == 0)
    {
        const Vector3 d = m_Current - m_Start;
        const float uExtent = Vector3::Dot(d, m_PlaneU);
        const float vExtent = Vector3::Dot(d, m_PlaneV);

        const Vector3 c0 = m_Start;
        const Vector3 c1 = m_Start + m_PlaneU * uExtent;
        const Vector3 c2 = c1 + m_PlaneV * vExtent;
        const Vector3 c3 = m_Start + m_PlaneV * vExtent;

        DrawThickLine(context, c0, c1, kColor, kThickness);
        DrawThickLine(context, c1, c2, kColor, kThickness);
        DrawThickLine(context, c2, c3, kColor, kThickness);
        DrawThickLine(context, c3, c0, kColor, kThickness);
    }
    else
    {
        for (size_t i = 0; i + 1 < m_Lasso.size(); ++i)
            DrawThickLine(context, m_Lasso[i], m_Lasso[i + 1], kColor, kThickness);
        if (m_Lasso.size() >= 3)
            DrawThickLine(context, m_Lasso.back(), m_Lasso.front(), kColor, kThickness);
    }
}

// --- HoverGizmo -----------------------------------------------------------

void HoverGizmo::SetHover(GameEngine::ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void HoverGizmo::SetDescendants(std::vector<GameEngine::ECS::EntityHandle> descendants)
{
    m_HoveredDescendants = std::move(descendants);
}

void HoverGizmo::Clear()
{
    m_HoveredEntity = {};
    m_HoveredDescendants.clear();
}

void HoverGizmo::Render(GizmoRenderContext& context)
{
    if (!m_HoveredEntity.IsValid())
        return;

    const auto& settings = SceneViewSettings::Get();
    if (settings.GetSelectionHighlightStyle() == SelectionHighlightStyle::Outline)
        return;

    GameEngine::ECS::World* world = context.GetWorld();
    if (!world || !world->IsValid(m_HoveredEntity))
    {
        m_HoveredEntity = {};
        m_HoveredDescendants.clear();
        return;
    }

    // Hover preview reads as a lighter version of the selection box: same color
    // but half alpha, slightly thinner.
    const Color selectionColor = ColorUtils::UnpackArgb(settings.GetSelectionBoxColor());
    const Color kColor(selectionColor.r, selectionColor.g, selectionColor.b, selectionColor.a * 0.55f);
    const float kThickness = settings.GetSelectionBoxThickness() * 0.66f;

    DrawEntityBoundsBox(context, *world, m_HoveredEntity, kColor, kThickness);
    for (const auto& descendant : m_HoveredDescendants)
        DrawEntityBoundsBox(context, *world, descendant, kColor, kThickness);
}

// --- SelectionTool --------------------------------------------------------

SelectionTool::SelectionTool(GameEngine::SceneViewController& owner)
    : m_Owner(owner)
{
}

void SelectionTool::OnPointerEvent(const ScenePointerEvent& event)
{
    if (event.button != PointerButton::Left || event.phase != PointerPhase::Down)
        return;

    // All picking (mesh raycast, light/probe icons, click-through sequencing,
    // modifier handling) lives in the controller so SelectionTool and
    // TransformTool behave identically.
    m_Owner.PickViaClickThrough(event);
}

void SelectionTool::OnKeyEvent(const SceneKeyEvent& /*event*/)
{
    // No keyboard handling for selection yet. This can later support multi-
    // select modifiers (Ctrl, Shift), focus shortcuts, etc.
}






} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
