#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <memory>
#include <string>

namespace GameEngine::Engine::Renderer::Pipeline
{

// Writes a depth attachment from another depth buffer, nearest-neighbor by construction
// (texelFetch + gl_FragDepth, compare Always): interpolating reverse-Z depth across a silhouette
// invents surfaces that occlude mid-air. Two uses:
// - A display-extent depth reconstituted from the internal raster depth of a view running an
//   internal-resolution split. The editor's gizmo and overlay passes depth-test at the display
//   extent and have no other source for it. The TAA resolve and the RenderScaleUpscale node share
//   this pass, so the two cannot drift apart.
// - A single-sample copy, at the same extent, of a multisampled depth: sample 0 of each pixel.
//   Under MSAA the late transparents composite into the resolved 1-sample colour and depth-test
//   against it (TransmissivePassNode).
class DepthUpsamplePass
{
public:
    // Interns the pipeline for a single-sample source on first call, and with
    // `multisampledSource` the one for a multisampled source; cheap and idempotent afterwards.
    // False means the shader package is not staged yet — retry next frame.
    bool EnsureLoaded(::GameEngine::Rendering::IDevice* device, bool multisampledSource = false);
    bool IsLoaded() const { return m_SingleSample.PipelineId.IsValid(); }

    // Declares one pass writing every pixel of `outputDepth` from `internalDepth`. A source whose
    // graph description has more than one sample is read at sample 0 (same extent as the output);
    // its pipeline must have been loaded with `multisampledSource`. Load op is DontCare: the
    // fullscreen triangle covers the whole surface, so previous contents never matter.
    void Declare(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                 ::GameEngine::Rendering::RenderGraph::RGTexture internalDepth,
                 ::GameEngine::Rendering::RenderGraph::RGTexture outputDepth, uint32_t outWidth,
                 uint32_t outHeight, const std::string& passName);

private:
    // One fragment program and its pipeline: taau_depth_upsample reads a sampler2D,
    // depth_copy_sample0 a sampler2DMS.
    struct Variant
    {
        ::GameEngine::Rendering::GraphicsPipelineId PipelineId{};
        std::unique_ptr<::GameEngine::Rendering::ShaderMeta> Meta;
        ::GameEngine::Rendering::DescriptorSetLayoutDesc Set0Layout{};
    };
    static bool LoadVariant(::GameEngine::Rendering::IDevice& device, const char* package, const char* debugName,
                            Variant& out);

    Variant m_SingleSample;
    Variant m_Multisampled;
    // texelFetch in the fragment stage bypasses filtering; the sampler exists
    // only to satisfy the combined-image-sampler binding.
    ::GameEngine::Rendering::SamplerHandle m_Sampler{};
    bool m_WarnedBindingMismatch = false;
};

} // namespace GameEngine::Engine::Renderer::Pipeline
