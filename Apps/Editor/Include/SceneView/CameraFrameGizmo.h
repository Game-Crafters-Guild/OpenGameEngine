#pragma once

#include "Components/Rendering/Camera.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

// Scene View gizmo that overlays the active game camera's visible frame as a
// blue line box. In orthographic / 2D mode it draws the framed rectangle; in
// perspective mode it draws the camera frustum outline. The host feeds the
// active camera each frame via SetCamera(); the gizmo draws from the stored
// copy so it never needs ECS world access of its own.
class CameraFrameGizmo : public IGizmo
{
public:
    CameraFrameGizmo() = default;

    // worldTransform is a column-major 4x4 (translation in columns 12/13/14,
    // right/up/forward in columns 0/1/2). When hasCamera is false the gizmo
    // draws nothing.
    void SetCamera(bool hasCamera,
                   const GameEngine::Components::Camera& params,
                   const float worldTransform[16]);

    void Render(GizmoRenderContext& context) override;

private:
    bool                       m_HasCamera{false};
    GameEngine::Components::Camera m_Params{};
    float                      m_WorldTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace GameEngine::Editor::SceneTools
