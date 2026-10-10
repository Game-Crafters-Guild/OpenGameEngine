#include "SceneView/SceneViewProjection.h"

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewEvents.h"

namespace GameEngine::Editor::SceneTools
{

namespace
{

// Nearest view depth, in meters, a perspective projection accepts; a nearer
// point is treated as behind the camera.
constexpr float kMinPerspectiveViewDepth = 0.01f;

} // namespace

bool ProjectWorldToView(const ScenePointerEvent& view,
                        const Mathematics::Vector3& world,
                        Mathematics::Vector2& outPixel)
{
    using Mathematics::Vector3;

    if (view.viewW <= 0.0f || view.viewH <= 0.0f)
        return false;

    const Vector3 rel = world - view.cameraPos;
    const float vx = Vector3::Dot(rel, view.cameraRight);
    const float vy = Vector3::Dot(rel, view.cameraUp);
    const float aspect = view.viewW / view.viewH;

    float ndcX = 0.0f;
    float ndcY = 0.0f;
    if (view.tanHalfFovY <= 0.0f)
    {
        const float halfH = view.orthoHeight * 0.5f;
        if (halfH <= 0.0f)
            return false;
        ndcX = vx / (halfH * aspect);
        ndcY = vy / halfH;
    }
    else
    {
        const float vz = Vector3::Dot(rel, view.cameraForward);
        if (vz <= kMinPerspectiveViewDepth)
            return false;
        ndcX = (vx / vz) / (view.tanHalfFovY * aspect);
        ndcY = (vy / vz) / view.tanHalfFovY;
    }

    outPixel = Mathematics::Vector2((ndcX * 0.5f + 0.5f) * view.viewW,
                                    (1.0f - (ndcY * 0.5f + 0.5f)) * view.viewH);
    return true;
}

} // namespace GameEngine::Editor::SceneTools
