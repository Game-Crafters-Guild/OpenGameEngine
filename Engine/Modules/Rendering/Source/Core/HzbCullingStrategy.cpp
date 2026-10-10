#include "Rendering/Core/HzbCullingStrategy.h"

#include "Rendering/Core/GPUCulling.h"

#include <cstdlib>

namespace GameEngine
{
namespace Rendering
{

void HzbCullingStrategy::ScheduleCulling(const ViewCullingContext& ctx)
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
    input.nearPlane = ctx.NearPlane;
    input.farPlane = ctx.FarPlane;
    input.firstInstance = ctx.FirstInstance;
    input.instanceCount = ctx.InstanceCount;
    input.renderLayerMask = ctx.RenderLayerMask;
    input.frameIndex = ctx.FrameIndex;
    input.deltaTime = ctx.DeltaTime;
    // The strategy's whole difference from FrustumCullingStrategy today:
    // reserve the phase-B slice for the view's own (non-cascade) generation.
    input.reserveOcclusionSlice = (ctx.CascadeIndex == kCullingCascadeIndexNone);

    ctx.CullingPipeline->SubmitView(input);
}

std::shared_ptr<ICullingStrategy> MakeDefaultOcclusionStrategyOrNull()
{
    static const std::shared_ptr<ICullingStrategy> s_Strategy =
        []() -> std::shared_ptr<ICullingStrategy>
    {
        if (const char* env = std::getenv("GE_HZB_OCCLUSION"); env && env[0] == '0')
            return nullptr;
        return std::make_shared<HzbCullingStrategy>();
    }();
    return s_Strategy;
}

} // namespace Rendering
} // namespace GameEngine
