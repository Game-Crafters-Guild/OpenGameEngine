#pragma once

#include "Engine/Rendering/ResolvedShadowSettings.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;

// Frame-local inputs to the shadow feature's world-pass contribution. GPU
// resources stay in the pass bundle; view outputs stay in the frame registry.
struct ScreenSpaceShadowView
{
    uint32_t ViewId;
    uint64_t WorldId;
    Rendering::RenderGraph::RGTexture Depth;
    const Rendering::CameraData* Camera;
    const ResolvedShadowSettings& Settings;
    bool CanDeclare;
    bool TransmissiveOnly;
    bool HasRayTracedMask;
};

Rendering::MaterialKeyword ContributeScreenSpaceShadows(
    RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
    const ScreenSpaceShadowView& view, std::unique_ptr<ScreenSpaceShadowPasses>& passes,
    Rendering::RenderGraph::RGTexture& mask, Rendering::MaterialKeyword keywords);
} // namespace GameEngine::Engine::Renderer
