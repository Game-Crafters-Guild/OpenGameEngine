#pragma once

#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Types/Types.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class Material;
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{
class Material;
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

class OceanRenderFeature;

// Builds the ocean surface draw as a unified forward DrawCommand.
// Compiles the ocean material (ocean_vertex_modifier.glsl + ocean_surface.glsl)
// through the standard adapter pipeline, then builds one instanced grid draw per
// view with the per-frame OceanParamsBuffer forwarded on set 2 via DrawBindings.
// Owned by OceanRenderFeature. NOT a registered forward producer: the sole
// caller is OceanRenderNode, via OceanRenderFeature::BuildSurfaceCommandForView
// per view — nothing here reaches RenderServices::RegisterForwardEmit, which is
// why idle-recompute elision pins on OceanRenderFeature::WritesDynamicDepth
// rather than the producer registry, and why reflection probes exclude the
// ocean (see the constructor note).
class OceanForwardContributor final
{
public:
    OceanForwardContributor(OceanRenderFeature& feature,
                            Engine::Renderer::RenderServices& renderServices);

    bool EnsureMaterial();
    // Builds the surface draw. Every DYNAMIC sim cascade the command binds is
    // also RG-imported (when a frame stream is active) so the consuming pass
    // can declare the sampled reads (the bind alone forms no producer->consumer
    // edge): the Ocean node passes outSampledCascades through to
    // AddForwardCommandPassForView. The span points into
    // per-view scratch — valid until the next BuildForwardCommand for the same
    // view. Static textures (caustics, user assets, IBL) are excluded: they
    // rest in ShaderResource and are never graph-written.
    bool BuildForwardCommand(Engine::Renderer::ForwardEmitContext& ctx,
                             Engine::Renderer::DrawCommand& outCommand,
                             std::span<const ::GameEngine::Rendering::RenderGraph::RGTexture>*
                                 outSampledCascades = nullptr);

private:
    OceanRenderFeature& m_Feature;
    Engine::Renderer::RenderServices& m_RenderServices;
    Engine::Renderer::Material* m_Material = nullptr;
    bool m_MaterialInitAttempted = false;

    // Lazy-interned pipeline variants for the ocean's vec2 grid-pos vertex layout.
    // Index 0 = one-sided above water, index 1 = two-sided underwater. Keeping both
    // avoids quad-view cameras repeatedly replacing one shared pipeline variant.
    ::GameEngine::Rendering::GraphicsPipelineId m_OceanPipelineIds[2]{};
    uint32_t m_PipelineMaterialVersion = 0;

    // 1x1 zeroed cascade-array stand-in for uOceanDisplacement / uOceanFoam when
    // their sims are unavailable. The compat shader profile declares exactly
    // these two cascade textures, and WebGPU refuses a bind group with any
    // declared entry unwritten — the availability flags in OceanParamsBuffer
    // already gate all sampling, so the content is never read meaningfully.
    ::GameEngine::Rendering::TextureHandle m_FallbackCascade{};
    ::GameEngine::Rendering::SamplerHandle m_FallbackCascadeSampler{};
    bool EnsureFallbackCascade(::GameEngine::Rendering::IDevice& device);

    // Per-view scratch keeps DrawCommand span-typed fields valid until the
    // per-view forward pass consumes them (the forward-contributor scratch pattern).
    struct ViewScratch
    {
        std::vector<Engine::Renderer::DrawBindings::BufferEntry> Buffers;
        std::vector<Engine::Renderer::DrawBindings::TextureEntry> Textures;
        std::vector<::GameEngine::Rendering::RenderGraph::RGTexture> SampledRG;
        uint32_t LastFrameUsed = 0;
    };
    std::unordered_map<::GameEngine::Rendering::ViewId, ViewScratch> m_Scratch;
};

} // namespace GameEngine::Ocean
