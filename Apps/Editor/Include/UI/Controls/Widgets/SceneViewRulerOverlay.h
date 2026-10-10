#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

class SceneViewController;
class SceneViewPanel;

// Overlay drawn on top of the Scene View viewport. Active only in 2D mode:
//   - Horizontal ruler band along the top edge with adaptive tick spacing
//   - Vertical ruler band along the left edge
//   - Live cursor position indicator on each ruler
//   - Magnification readout in the top-left intersection square (zoom in
//     pixels-per-world-unit, or integer pixel scale when pixel-perfect mode
//     is active)
//
// The element fills the viewport (position:absolute; inset:0) but has
// pointer-events:none so it does not steal pan/orbit input. The owning
// SceneViewPanel pushes the cursor position via SetCursor() each frame.
class SceneViewRulerOverlay final : public UIElement
{
public:
    SceneViewRulerOverlay();
    ~SceneViewRulerOverlay() override = default;

    void SetSceneController(SceneViewController* controller) { m_Controller = controller; }
    void SetPanel(SceneViewPanel* panel) { m_Panel = panel; }

    // Called every frame from SceneViewPanel so the overlay reacts to
    // camera/zoom/mode changes and re-emits primitives when needed.
    void Tick();

    // Update the live cursor position used to draw the position indicators.
    // localX/localY are logical CSS px relative to this element's top-left
    // corner (pointer events are logical). Pass inside=false when the cursor
    // leaves the viewport.
    void SetCursor(float localX, float localY, bool inside);

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    SceneViewController* m_Controller = nullptr; // not owned
    SceneViewPanel*      m_Panel      = nullptr; // not owned

    // Last camera state seen during Tick(); used for dirty tracking so we
    // only re-emit primitives when something visible actually changed.
    float m_LastCamX     = 0.0f;
    float m_LastCamY     = 0.0f;
    float m_LastDistance = 0.0f;
    float m_LastViewW    = 0.0f;
    float m_LastViewH    = 0.0f;
    bool  m_LastShown    = false;
    int   m_LastPixelScale = 0;
    bool  m_LastPixelPerfect = false;
    float m_LastOpacity  = -1.0f;
    uint32_t m_LastIndicatorColor = 0u;
    float    m_LastIndicatorThickness = -1.0f;

    // Cursor state, in this element's local logical CSS px.
    float m_CursorX      = 0.0f;
    float m_CursorY      = 0.0f;
    bool  m_CursorInside = false;
};

} // namespace GameEngine
