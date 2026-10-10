#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

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
// Both FXAA modes, run at the END of the LDR post chain (after tonemap + all
// post FX, matching the paper).
//
// AntiAliasingMode::FXAA is classic single-frame FXAA: the spatial pass alone,
// unjittered, written straight to the output — no depth, no history.
//
// AntiAliasingMode::TemporalFXAA is the two-frame technique (Decima 2017). It
// shares the jittered single-sample raster path with TAA — the view registers
// ViewAntiAliasing with the FXAA edge-jitter pattern — but resolves here
// instead of in the HDR chain. Two compute passes per view:
//   fxaa (compute) — luma FXAA on the tonemapped LDR input.
//   fxaa_resolve (compute) — contrast-driven sharpen of the FXAA'd current
//     frame, analytic camera reprojection of the previous frame from depth,
//     3x3 neighborhood + depth reject, fixed 50/50 blend on accept. Writes the
//     SHARPENED CURRENT frame to a ping-pong history (never the blend, so
//     nothing accumulates -> no ghosting trail) and the pass output.
// History layout: rgb = sharpened LDR, a = this frame's raster depth.
//
// The node is the FXAA gate for the LDR tail: when the view has no
// ViewAntiAliasing state, or its mode is neither FXAA, it stitches its input through
// under the output name so downstream consumers (FinalCopy) bind one name
// unconditionally.
//
// JSON shape: { "id": "TemporalFxaa", "type": "TemporalFxaa",
//               "input": "LDRFinal", "output": "LDRFxaa" }
class TemporalFxaaNode final : public IRenderPipelineNode
{
  public:
    ~TemporalFxaaNode() override;

    const char* GetTypeName() const override { return "TemporalFxaa"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

    // Whether the ping-pong history about to be read holds a frame this view
    // rendered at the current extent. historyFresh comes from
    // ImportPersistentTexture: true means the pool handed back a
    // recycled/undefined physical, so nothing this view wrote survives in it.
    static bool ComputeHistoryValid(uint64_t historyFrame, uint32_t prevWidth, uint32_t prevHeight,
                                    uint32_t width, uint32_t height, bool prevCameraValid,
                                    bool historyFresh);

  private:
    void LoadShaders(IDevice* device);

    std::string m_InputKey = "LDRFinal";
    std::string m_OutputKey = "LDRFxaa";
    std::string m_Id;

    bool m_ShadersLoaded = false; // latches on SUCCESS; failed loads retry next frame
    bool m_WarnedUnresolvedInput = false;
    bool m_WarnedBindingMismatch = false;

    // fxaa.comp — pass 1, Fast variant
    ComputePipelineId m_FxaaPipelineId{};
    std::unique_ptr<ShaderMeta> m_FxaaMeta;
    DescriptorSetLayoutDesc m_FxaaSet0Layout{};

    // fxaa_quality.comp — pass 1, Quality variant (FxaaQuality selects)
    ComputePipelineId m_FxaaQualityPipelineId{};
    std::unique_ptr<ShaderMeta> m_FxaaQualityMeta;
    DescriptorSetLayoutDesc m_FxaaQualitySet0Layout{};

    // fxaa_resolve.comp — pass 2
    ComputePipelineId m_ResolvePipelineId{};
    std::unique_ptr<ShaderMeta> m_ResolveMeta;
    DescriptorSetLayoutDesc m_ResolveSet0Layout{};

    SamplerHandle m_LinearClampSampler{};
    // The device the sampler above was created on, so the destructor can
    // return it; null until LoadShaders succeeds.
    IDevice* m_Device = nullptr;

    // Per-view history bookkeeping (the fog idiom): parity selects the
    // ping-pong pool texture; validity requires having written history for the
    // immediately preceding frame at the same extent.
    struct ViewHistory
    {
        uint64_t HistoryFrame = 0;      // parity counter (advances once per resolved frame)
        uint32_t Width = 0;
        uint32_t Height = 0;
    };
    std::unordered_map<uint32_t, ViewHistory> m_ViewHistory;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
