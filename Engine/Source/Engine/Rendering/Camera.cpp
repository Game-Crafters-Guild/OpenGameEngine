#include "Engine/Rendering/Camera.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Math.h"

#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer {
using namespace ::GameEngine::Rendering;

Mathematics::Matrix4x4 Camera::ComputeViewMatrix() const
{
    return Mathematics::Inverse(worldTransform);
}

Mathematics::Matrix4x4 Camera::ComputeProjectionMatrix(float aspectRatio,
                                                       std::optional<float> orthoHeightOverride) const
{
    const float safeAspect = std::max(aspectRatio, 0.0001f);
    if (params.Perspective)
    {
        const float fovRad = Rendering::Math::ToRadians(params.FovY);
        const float squeeze = std::clamp(params.AnamorphicSqueeze, 1.0f, 4.0f);
        return Mathematics::MakePerspectiveLH_ZO_ReverseZ(fovRad, safeAspect * squeeze, params.NearZ, params.FarZ);
    }

    const float requestedHeight = orthoHeightOverride.value_or(params.OrthographicSize);
    const float orthoHeight = std::max(requestedHeight, 0.0001f);
    const float halfH = orthoHeight * 0.5f;
    const float halfW = halfH * safeAspect;
    return Mathematics::MakeOrthographicLH_ZO_ReverseZ(-halfW, halfW, -halfH, halfH, params.NearZ, params.FarZ);
}

Mathematics::Matrix4x4 Camera::ComputeViewProjectionMatrix(float aspectRatio,
                                                           std::optional<float> orthoHeightOverride) const
{
    return ComputeProjectionMatrix(aspectRatio, orthoHeightOverride) * ComputeViewMatrix();
}

Rendering::CameraData Camera::ToCameraData(float aspectRatio,
                                           std::optional<float> orthoHeightOverride) const
{
    const Mathematics::Matrix4x4 viewM = ComputeViewMatrix();
    const Mathematics::Matrix4x4 projM = ComputeProjectionMatrix(aspectRatio, orthoHeightOverride);
    const Mathematics::Matrix4x4 viewProjM = projM * viewM;

    Rendering::CameraData data{};
    const float* viewSrc = viewM.Data();
    const float* projSrc = projM.Data();
    const float* viewProjSrc = viewProjM.Data();
    for (int i = 0; i < 16; ++i)
    {
        data.view[i] = viewSrc[i];
        data.proj[i] = projSrc[i];
        data.viewProj[i] = viewProjSrc[i];
    }

    // Extract camera world position from the worldTransform translation column.
    const float* wt = worldTransform.Data();
    data.cameraPos[0] = wt[12];
    data.cameraPos[1] = wt[13];
    data.cameraPos[2] = wt[14];
    data.cameraPos[3] = 0.0f;

    return data;
}

std::optional<Camera> FindActiveCamera(ECS::World& world)
{
    Camera result{};
    bool found = false;

    auto q = world.Query<ECS::Read<Components::Camera>>();
    q.Each([&](ECS::EntityHandle e, const Components::Camera& cam)
    {
        if (found)
            return;

        // Extract world transform (prefer WorldTransform, fallback to Transform).
        float matrix[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };

        if (world.HasComponent<Components::WorldTransform>(e))
        {
            if (auto* xf = world.GetComponent<Components::WorldTransform>(e))
                for (int i = 0; i < 16; ++i)
                    matrix[i] = xf->matrix[i];
        }
        else if (world.HasComponent<Components::Transform>(e))
        {
            if (auto* xf = world.GetComponent<Components::Transform>(e))
                for (int i = 0; i < 16; ++i)
                    matrix[i] = xf->matrix[i];
        }

        result.worldTransform = Mathematics::Matrix4x4(glm::make_mat4(matrix));
        result.params = cam;
        result.entity = e;
        found = true;
    });

    if (!found)
        return std::nullopt;
    return result;
}

} // namespace GameEngine::Engine::Renderer
