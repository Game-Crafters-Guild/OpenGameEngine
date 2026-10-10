#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <memory>
#include <string>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::ShaderMeta;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Histogram auto-exposure (eye adaptation). Two compute passes per view:
//   auto_exposure_histogram.comp — HDR scene colour + the view's resolved depth -> two 256-bin
//                                  log-luminance histograms, geometry and sky (the far plane)
//   auto_exposure_resolve.comp   — meters the geometry, with the sky as a ceiling when it is a large
//                                  share of the frame -> adapted LINEAR exposure
// The resolved exposure lives in the per-view "ExposureHistory" buffer — a PERSISTENT blueprint
// resource (rendergraph "resources" entry: 16-byte perView Storage buffer, zeroOnCreate), never a
// transient (those alias across frames). Consumers (BloomThreshold, Tonemap) bind it by name and
// the blackboard materializes it on first resolve, so declaration order does not matter. For
// rendergraphs that predate the resource entry, DeclareForView imports + publishes the buffer
// itself. Only the metering passes are gated on ExposureMode == Auto (IsAutoExposureActive); when
// off the buffer is simply not updated and consumers fall back to their static exposure (the
// zeroed contents fail their exposureScale > 0 guard).
//
// Must appear after the node that produces its metering input and before the
// Tonemap node. Shipped graphs set "input" to the last pre-bloom chain value
// (ForwardPlus: HDRHeatDistortion) so bloom energy and HDR grading don't bias
// the measured luminance; the node default stays HDRFiltered so legacy graphs
// that omit "input" keep resolving.
//
// JSON shape: { "id": "AutoExposure", "type": "AutoExposure", "input": "HDRHeatDistortion", "enabled": true }
class AutoExposureNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "AutoExposure"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void LoadShaders(IDevice* device);

    // Metering source, overridden via the node's "input". The default stays
    // HDRFiltered — the name every tonemap chain produces — so legacy graphs
    // that omit "input" keep their pre-existing (post-bloom) metering rather
    // than silently failing to resolve a chain-specific name. Shipped graphs
    // set the pre-bloom tap explicitly.
    std::string m_InputKey = "HDRFiltered";
    std::string m_HistogramPrefix = "Pipeline.AutoExposure.Histogram.View";
    std::string m_Id;

    float m_DeltaTimeSeconds = 0.0f; // latched in Declare (DeclareForView carries no dt)
    bool m_ShadersLoaded = false;    // latches on SUCCESS; failed loads retry next frame
    bool m_WarnedUnresolvedInput = false;
    bool m_WarnedUnresolvedDepth = false;
    // 1b under-size guard: warn-once (persistent across frames — never per-frame
    // state) if the resolved ExposureHistory is smaller than the state struct.
    // Keyed on the single buffer this node consumes (kExposurePublishName).
    bool m_WarnedExposureHistoryUnderSize = false;

    // auto_exposure_histogram.comp — HDR colour + view depth -> geometry and sky histograms
    ComputePipelineId m_HistogramPipelineId{};
    std::unique_ptr<ShaderMeta> m_HistogramMeta;
    DescriptorSetLayoutDesc m_HistogramSet0Layout{};

    // auto_exposure_resolve.comp — histograms -> metered, adapted exposure (single thread)
    ComputePipelineId m_ResolvePipelineId{};
    std::unique_ptr<ShaderMeta> m_ResolveMeta;
    DescriptorSetLayoutDesc m_ResolveSet0Layout{};

    SamplerHandle m_Sampler{}; // linear-clamp for the HDR luminance taps
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
