#include "Rendering/Core/FrustumCullingStrategy.h"

#include "Rendering/Core/GPUCulling.h"

namespace GameEngine
{
namespace Rendering
{

void FrustumCullingStrategy::ScheduleCulling(const ViewCullingContext& ctx)
{

    if (ctx.CullingPipeline == nullptr)
    {
        return;
    }

    ViewCullingInput input{};
    input.viewId = ctx.Id;
    input.cascadeIndex = ctx.CascadeIndex;
    input.viewMatrix = ctx.ViewMatrix;
    input.projMatrix = ctx.ProjMatrix;
    input.viewProjMatrix = ctx.ViewProjMatrix;
    for (int i = 0; i < 6; ++i)
    {
        input.frustumPlanes[i] = ctx.FrustumPlanes[i];
    }
    input.cameraPosition = ctx.CameraPosition;
    input.cameraForward = ctx.CameraForward;
    input.cameraRelativeOrigin = ctx.CameraRelativeOrigin;
    input.nearPlane = ctx.NearPlane;
    input.farPlane = ctx.FarPlane;
    input.firstInstance = ctx.FirstInstance;
    input.instanceCount = ctx.InstanceCount;
    input.renderLayerMask = ctx.RenderLayerMask;
    input.frameIndex = ctx.FrameIndex;
    input.deltaTime = ctx.DeltaTime;

    ctx.CullingPipeline->SubmitView(input);
}

} // namespace Rendering
} // namespace GameEngine
