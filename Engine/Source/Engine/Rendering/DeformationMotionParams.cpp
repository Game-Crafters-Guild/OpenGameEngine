#include "Engine/Rendering/DeformationMotionParams.h"

#include "Engine/Rendering/ViewTemporalHistory.h"
#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::Engine::Renderer
{

const char* DeformationMotionArmName(DeformationMotionArm arm)
{
    switch (arm)
    {
    case DeformationMotionArm::ColorPassTarget: return "colour-pass target";
    case DeformationMotionArm::PrepassTarget:   return "prepass target";
    case DeformationMotionArm::SeparatePass:    return "separate pass";
    case DeformationMotionArm::None:            break;
    }
    return "none";
}

DeformationMotionArm ResolveDeformationMotionArm(bool colorPassTarget, bool prepassTarget,
                                                 bool separatePass)
{
    const int enabled = (colorPassTarget ? 1 : 0) + (prepassTarget ? 1 : 0) + (separatePass ? 1 : 0);
    if (enabled > 1)
    {
        static bool warnedOnce = false;
        if (!warnedOnce)
        {
            warnedOnce = true;
            Logger::Log::Error(
                "Deformation motion: {} producer arms are enabled at once (colour-pass target {}, "
                "prepass target {}, separate pass {}). They write one shared target, so two of "
                "them would write every deforming pixel twice and the later write would win "
                "non-deterministically. No deforming motion is produced this run — enable exactly "
                "one arm.",
                enabled, colorPassTarget, prepassTarget, separatePass);
        }
        return DeformationMotionArm::None;
    }
    if (colorPassTarget)
        return DeformationMotionArm::ColorPassTarget;
    if (prepassTarget)
        return DeformationMotionArm::PrepassTarget;
    if (separatePass)
        return DeformationMotionArm::SeparatePass;
    return DeformationMotionArm::None;
}

bool BuildDeformationMotionParams(const DeformationMotionEndpoint& endpoint,
                                  const DeformationMotionRaster& raster,
                                  DeformationMotionParamsGPU& out)
{
    if (endpoint.Previous == nullptr || !endpoint.PreviousValid)
        return false;
    // A re-anchored origin (the history's first frame, the precision ceiling)
    // leaves the two endpoints measured from different zeros. Their difference
    // is then not this frame's delta, so the pair is treated as absent history
    // for exactly the one frame the re-anchor costs.
    if (endpoint.Previous->DeformationOrigin != endpoint.CurrentOrigin)
        return false;
    if (raster.Viewport.Width <= 0.0f || raster.Viewport.Height <= 0.0f)
        return false;

    std::memcpy(out.PrevViewProj, endpoint.Previous->Camera.viewProj, sizeof(out.PrevViewProj));
    out.PrevTimeParams[0] = endpoint.Previous->DeformationTimeSeconds;
    out.PrevTimeParams[1] = endpoint.Previous->DeformationScrollSeconds;
    out.PrevTimeParams[2] = 0.0f;
    out.PrevTimeParams[3] = 0.0f;
    out.ViewportRect[0] = raster.Viewport.X;
    out.ViewportRect[1] = raster.Viewport.Y;
    out.ViewportRect[2] = raster.Viewport.Width;
    out.ViewportRect[3] = raster.Viewport.Height;
    // NDC-to-viewport-UV is a halving, and the y axis flips: ApplyNdcJitter
    // adds the offset in Y-up NDC while GE_YUpNdcToViewportUV flips y. The
    // fragment subtracts this from the UV it reads off gl_FragCoord, which
    // recovers the unjittered projection at the pixel centre.
    out.JitterUv[0] = 0.5f * raster.NdcJitterX;
    out.JitterUv[1] = -0.5f * raster.NdcJitterY;
    out.JitterUv[2] = 0.0f;
    out.JitterUv[3] = 0.0f;
    return true;
}

} // namespace GameEngine::Engine::Renderer
