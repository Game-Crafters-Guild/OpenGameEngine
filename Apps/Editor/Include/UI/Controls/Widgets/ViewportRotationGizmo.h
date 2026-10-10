#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

class SceneViewController;
class SceneViewPanel;

// Small interactive 3D orientation gizmo drawn in the top-right corner of the
// Scene View viewport.
//
// - Shows three axis "planets" (X/Y/Z and their negatives) around a center
// - Hovering an axis highlights it
// - Click an axis → tween the camera to the orthogonal view
// - Click-drag anywhere inside the circle → orbit the camera freely
class ViewportRotationGizmo final : public UIElement
{
public:
    ViewportRotationGizmo();
    ~ViewportRotationGizmo() override = default;

    void SetSceneController(SceneViewController* controller)
    {
        if (m_Controller != controller) m_HasPerspectiveAngle = false;
        m_Controller = controller;
    }
    void SetPanel(SceneViewPanel* panel) { m_Panel = panel; }
    void SetViewportIndex(size_t index) { m_ViewportIndex = index; }

    // Called every frame by SceneViewPanel so the gizmo re-draws when the
    // camera angles change (e.g. while panning in the viewport).
    void Tick();

    // Custom drawing via SDF primitive pipeline.
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    struct Axis2D
    {
        int Index = 0;      // 0..5: +X,+Y,+Z,-X,-Y,-Z
        float ScreenX = 0;  // local to gizmo rect
        float ScreenY = 0;
        float Z = 0;        // view-space depth for painter sort
    };

    void BuildAxes(float centerX, float centerY, float radius, Axis2D (&out)[6]) const;
    int  HitTestAxis(float localX, float localY, float centerX, float centerY, float radius) const;

    void OnMouseDown(UIEvent& ev);
    void OnMouseUp(UIEvent& ev);
    void OnMouseMove(UIEvent& ev);
    void OnMouseLeave(UIEvent& ev);

    void SnapToAxis(int axis);
    void ToggleGameProjection();

    SceneViewController* m_Controller = nullptr; // not owned
    SceneViewPanel*      m_Panel      = nullptr; // not owned
    size_t               m_ViewportIndex = 0;

    // Restore the perspective viewing angle after the angled ortho preset.
    bool m_HasPerspectiveAngle = false;
    float m_PerspectiveYaw = 0.0f;
    float m_PerspectivePitch = 0.0f;

    // Last-seen camera angles for dirty tracking.
    float m_LastYaw   = 0.0f;
    float m_LastPitch = 0.0f;

    // Interaction state.
    bool  m_Orbiting       = false;
    bool  m_PressedInside  = false;
    float m_PressX         = 0.0f;
    float m_PressY         = 0.0f;
    float m_LastX          = 0.0f;
    float m_LastY          = 0.0f;
    int   m_HoveredAxis    = -1;
};

} // namespace GameEngine
