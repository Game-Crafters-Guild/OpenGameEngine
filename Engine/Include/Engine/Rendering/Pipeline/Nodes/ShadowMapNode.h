#pragma once

#include "Engine/Rendering/Pipeline/Nodes/ShadowReceiverReduce.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::GraphicsPipelineId;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::ShaderMeta;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Per-view pipeline node that owns every shadow family (cascades for the
// directional light plus area/spot/point). Creates a shadow data upload pass
// and per-cascade depth render passes; optionally a debug overlay pass showing
// shadow map thumbnails.
// Configurable via .rendergraph JSON:
//   { "type": "ShadowMap", "cascades": 4, "resolution": 2048,
//     "splitLambda": 0.5, "maxShadowDistance": 100.0, "buffer": "ShadowData" }
// "fitShadowDistanceToScene": true replaces maxShadowDistance with a range fitted
// per view to the bounds of every GPUScene mesh instance, in every world and
// render layer (ShadowMapRenderFeature::FitShadowDistanceToScene). Terrain and
// water are not mesh instances and do not extend it; the range has no upper
// bound but the camera's far plane, which clamps the cascade splits. Meant for
// pipelines whose scenes have no single scale, such as a model viewer.
// The legacy type key "CascadedShadowMap" is still accepted (parsed identically,
// with a deprecation warning); see the node registration in FrameOrchestrator.
class ShadowMapNode final : public IRenderPipelineNode
{
  public:
    ~ShadowMapNode() override;

    const char* GetTypeName() const override { return "ShadowMap"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    // Resolved punctual shadow resolution: the explicit "punctualResolution"
    // when set, else the cascade resolution (pre-decouple default → byte-identical).
    uint32_t PunctualResolution() const
    {
        return m_PunctualResolution != 0 ? m_PunctualResolution : m_Resolution;
    }

    std::string m_Id;
    std::string m_Json;

    // RenderGraph path: Poisson temporal-rotation frame index (the old upload pass
    // kept it on a shared_ptr because its exec lambda persisted; Declare
    // runs fresh per frame so a plain member suffices).
    uint32_t m_RGFrameCounter = 0;

    // Parsed from JSON.
    uint32_t m_NumCascades = 4;
    uint32_t m_Resolution = 2048;
    // Punctual (area/spot/point) shadow-map resolution, DECOUPLED from the
    // directional cascade resolution above (arc slice S0). Sentinel 0 means
    // "unset" — Initialize resolves it to m_Resolution so a .rendergraph that
    // only specifies "resolution" keeps the pre-decouple behaviour (punctual ==
    // cascade res) byte-for-byte. Set the "punctualResolution" JSON key to
    // override, or a per-light shadowResolutionTier to shrink an individual light.
    uint32_t m_PunctualResolution = 0;
    // Max simultaneously-shadowed point lights per view (arc slice M1). The atlas
    // is budget * 6 array layers at the High tier; lower-importance point lights
    // beyond the budget stay lit but unshadowed. The default is 4; choosing the
    // shipped value is tracked in #2934. Overridable via the "pointShadowBudget"
    // JSON key.
    uint32_t m_PointShadowBudget = 4;
    // MSM4 moments-array resolution per cascade (independent of depth-array
    // Resolution above). 1024 is the default sweet spot — high enough to
    // capture sharp depth discontinuities cleanly while keeping the texture
    // at 32 MiB per view. Allowed values: {512, 1024, 2048}.
    uint32_t m_MomentsResolution = 1024;
    float m_SplitLambda = 0.5f;
    float m_MaxShadowDistance = 100.0f;
    bool m_FitShadowDistanceToScene = false;
    // NDC-space anti-acne bias added to the receiver's reference depth before
    // the shadow comparison. Multiplied at sample time by per-cascade
    // (cascade0DepthSpan / thisDepthSpan), so the world-space contact gap is
    // approximately constant across cascades. 0.0001 NDC × cascade0 50m span
    // ≈ 5 mm world peter-panning at the contact point — small enough not to
    // visibly detach the shadow, large enough to mask reverse-Z + D32_FLOAT
    // rasterization noise.
    float m_DepthBias = 0.0001f;
    float m_NormalBias = 0.02f;
    std::string m_BufferRef;

    // Debug overlay infrastructure (shadow map thumbnails).
    void EnsureOverlayResources(Rendering::IDevice* device);
    Rendering::IDevice* m_OverlayDevice = nullptr;
    Rendering::SamplerHandle m_OverlaySampler;
    Rendering::GraphicsPipelineId m_OverlayPipelineId{};
    Rendering::DescriptorSetLayoutDesc m_OverlaySet0Layout{};
    std::unique_ptr<Rendering::ShaderMeta> m_OverlayMeta;
    std::vector<uint8_t> m_OverlayVS;
    std::vector<uint8_t> m_OverlayFS;
    bool m_OverlayLoadAttempted = false;

    // SDSM measurement of the cascades' receivers, declared after the node loop.
    ShadowReceiverReduce m_ReceiverReduce;

    // MSM4 fullscreen-quad pipelines: one for the moments-write
    // (samples cascade depth, computes Hamburger-quantized moments) and
    // two for the separable Gaussian blur (H then V). All three intern
    // eagerly in EnsureMsmResources; set-0 layouts are stashed for
    // execute-time descriptor allocation.
    void EnsureMsmResources(Rendering::IDevice* device);
    Rendering::GraphicsPipelineId m_MsmWritePipelineId{};
    Rendering::GraphicsPipelineId m_MsmBlurHPipelineId{};
    Rendering::GraphicsPipelineId m_MsmBlurVPipelineId{};
    Rendering::DescriptorSetLayoutDesc m_MsmWriteSet0Layout{};
    Rendering::DescriptorSetLayoutDesc m_MsmBlurHSet0Layout{};
    Rendering::DescriptorSetLayoutDesc m_MsmBlurVSet0Layout{};
    std::unique_ptr<Rendering::ShaderMeta> m_MsmWriteMeta;
    std::unique_ptr<Rendering::ShaderMeta> m_MsmBlurHMeta;
    std::unique_ptr<Rendering::ShaderMeta> m_MsmBlurVMeta;
    std::vector<uint8_t> m_MsmWriteVS;
    std::vector<uint8_t> m_MsmWriteFS;
    std::vector<uint8_t> m_MsmBlurVS;
    std::vector<uint8_t> m_MsmBlurHFS;
    std::vector<uint8_t> m_MsmBlurVFS;
    Rendering::SamplerHandle m_MsmDepthSampler;
    bool m_MsmLoadAttempted = false;

    // MSM4 fused compute pipeline (msm_cascade.comp): one dispatch per
    // cascade does encode + horizontal blur + vertical blur in shared
    // memory, replacing the 3-pass graphics chain above. The graphics
    // pipelines stay loaded as a runtime fallback for when the compute
    // shaderpkg is missing (older builds, hot-reload partial states).
    Rendering::ComputePipelineId             m_MsmCascadeComputePipelineId{};
    Rendering::DescriptorSetLayoutDesc       m_MsmCascadeComputeSet0Layout{};
    std::unique_ptr<Rendering::ShaderMeta>   m_MsmCascadeComputeMeta;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
