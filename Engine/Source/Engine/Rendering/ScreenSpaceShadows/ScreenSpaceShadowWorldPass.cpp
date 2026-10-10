#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowWorldPass.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include <cmath>

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr float kMinLightDirectionLength = 1e-6f;
constexpr const char* kShadowConstantsBinding = "ShadowData";
}

Rendering::MaterialKeyword ContributeScreenSpaceShadows(
    RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
    const ScreenSpaceShadowView& view, std::unique_ptr<ScreenSpaceShadowPasses>& passes,
    Rendering::RenderGraph::RGTexture& mask, Rendering::MaterialKeyword keywords)
{
    using Rendering::MaterialKeyword;
    // A keyword is permission to sample a mask produced in this view/frame,
    // not a persisted pipeline option or merely an enabled volume setting.
    keywords = keywords & ~MaterialKeyword::ScreenSpaceShadows;
    if (!view.Settings.ScreenSpaceShadows || view.TransmissiveOnly || !Rendering::HasKeyword(keywords, MaterialKeyword::Shadows))
        return keywords;

    if (view.CanDeclare && !mask.IsValid() && view.Depth.IsValid() && view.Camera)
    {
        const auto* sun = SelectPrimaryDirectional(services.GetWorldLights(view.WorldId));
        if (sun && sun->castsShadows != 0)
        {
            float towardLight[3] = {-sun->directionWS[0], -sun->directionWS[1], -sun->directionWS[2]};
            const float length = std::sqrt(towardLight[0] * towardLight[0] +
                towardLight[1] * towardLight[1] + towardLight[2] * towardLight[2]);
            if (std::isfinite(length) && length > kMinLightDirectionLength)
            {
                for (float& value : towardLight) value /= length;
                if (!passes)
                    passes = std::make_unique<ScreenSpaceShadowPasses>(services.GetDevice());
                ScreenSpaceShadowFilter filter{};
                const auto shadowMap = services.GetShadowMapArrayRG(frame, view.ViewId);
                // Match the current per-view PCF footprint, not a second fit.
                // Only the fixed grid kernels have one: a ray-traced mask and the
                // blocker-dependent filters (PCSS, DPCF, MSM) have no footprint to
                // project, and asking for one would schedule a pass that copies.
                const bool gridFilter =
                    view.Settings.Filter == Components::DirectionalShadowFilter::Grid5x5 ||
                    view.Settings.Filter == Components::DirectionalShadowFilter::Grid3x3;
                if (gridFilter && !view.HasRayTracedMask && shadowMap.IsValid())
                {
                    if (auto* instance = services.Spine().PipelineInstanceForFrame(frame))
                        if (const auto* resources = instance->FrameResourcesFor(&frame))
                            resources->ForEachBufferBinding(view.ViewId,
                                [&](const std::string& name, const Pipeline::PipelineBufferBindingRG& binding)
                                {
                                    if (name == kShadowConstantsBinding)
                                    {
                                        filter.Constants = binding.Buffer;
                                        filter.Offset = binding.Offset;
                                        filter.Bytes = binding.Size;
                                        filter.Resolution = frame.Graph().ResourceDesc(shadowMap.Id).Width;
                                        filter.Quality = static_cast<uint32_t>(view.Settings.Filter);
                                    }
                                });
                }
                mask = passes->DeclareMaskPass(frame, view.ViewId, view.Depth, *view.Camera,
                    towardLight, view.Settings.ScreenSpaceShadowThickness, &filter);
            }
        }
    }
    return mask.IsValid() ? keywords | MaterialKeyword::ScreenSpaceShadows : keywords;
}
} // namespace GameEngine::Engine::Renderer
