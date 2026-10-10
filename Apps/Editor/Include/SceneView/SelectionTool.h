// Selection tool and selection gizmo declarations for Scene View.
#pragma once

#include <array>
#include <vector>

// Only need ECS entity handle type here; keep World/query details in the .cpp
#include "ECS/Entity.h"
#include "SceneViewTools.h"

namespace GameEngine {

class SceneViewController; // forward declaration to avoid header cycle

namespace Editor {
namespace SceneTools {

// World-space selection overlay gizmo. Renders bounding boxes around one or
// more selected entities. SelectionTool owns the selection logic and updates
// the entity list; this gizmo stays always-on regardless of the active tool.
class SelectionGizmo : public IGizmo
{
public:
    SelectionGizmo() = default;

    // Convenience: single-entity selection. Clears previous selection.
    void SetSelection(GameEngine::ECS::EntityHandle entity);

    // Multi-entity selection. Replaces the current list. This keeps the gizmo
    // ready for a future multi-select aware SelectionContext.
    void SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities);

    // Descendants of selected entities to also highlight (not part of the
    // logical selection — just visual feedback).
    void SetDescendants(std::vector<GameEngine::ECS::EntityHandle> descendants);

    void Clear();

    void Render(GizmoRenderContext& context) override;

private:
    std::vector<GameEngine::ECS::EntityHandle> m_SelectedEntities;
    std::vector<GameEngine::ECS::EntityHandle> m_SelectionDescendants;
};

// Marquee selection overlay. Drawn while the user is dragging out a rectangle
// or lasso during scene-wide multi-select (TransformTool in Select mode).
// Points are stored in world space on the click plane picked at drag start.
class MarqueeGizmo : public IGizmo
{
public:
    MarqueeGizmo() = default;

    void SetActive(bool active) { m_Active = active; }
    bool IsActive() const { return m_Active; }

    void SetShape(int shape) { m_Shape = shape; } // 0 = rect, 1 = lasso
    void SetRect(const Mathematics::Vector3& start, const Mathematics::Vector3& current,
                 const Mathematics::Vector3& planeU, const Mathematics::Vector3& planeV);
    void SetLasso(const std::vector<Mathematics::Vector3>& points);

    // Appearance: thickness in pixels.
    void SetColor(const Color& color) { m_Color = color; }
    void SetThickness(float thickness) { m_Thickness = thickness; }

    void Render(GizmoRenderContext& context) override;

private:
    bool m_Active = false;
    int  m_Shape = 0;
    Mathematics::Vector3 m_Start{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_Current{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_PlaneU{1.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_PlaneV{0.0f, 1.0f, 0.0f};
    std::vector<Mathematics::Vector3> m_Lasso;
    Color m_Color {1.0f, 1.0f, 0.2f, 1.0f};
    float m_Thickness = 2.0f;
};

// World-space hover overlay gizmo. Renders a subtle bounding box around a hovered entity
// (e.g., while hovering in the Scene View or Hierarchy).
class HoverGizmo : public IGizmo
{
public:
    HoverGizmo() = default;

    void SetHover(GameEngine::ECS::EntityHandle entity);
    void SetDescendants(std::vector<GameEngine::ECS::EntityHandle> descendants);
    void Clear();

    void Render(GizmoRenderContext& context) override;

private:
    GameEngine::ECS::EntityHandle              m_HoveredEntity{};
    std::vector<GameEngine::ECS::EntityHandle> m_HoveredDescendants;
};

// Selection tool that performs ray/AABB picking against renderable entities
// and forwards the picked entity to SceneViewController. The visual overlay is
// handled by SelectionGizmo, which stays active even when another tool (e.g.
// TransformTool) is the primary Scene tool.
class SelectionTool : public ISceneTool
{
public:
    explicit SelectionTool(GameEngine::SceneViewController& owner);

    const char* GetName() const override { return "SelectionTool"; }

    void OnActivated() override {}
    void OnDeactivated() override {}

    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& event) override;

    // SelectionTool itself does not contribute gizmos directly; the selection
    // overlay is provided by SelectionGizmo, which is registered as an
    // always-on gizmo in SceneViewController.
    void GatherGizmos(GizmoCollector& /*collector*/) override {}

private:
    GameEngine::SceneViewController& m_Owner;
};

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine

