#pragma once

#include "Engine/Rendering/AmbientOcclusion/GtaoHistory.h"
#include "Engine/Rendering/AmbientOcclusion/GtaoQuality.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Before World: prepare representative depth + filtered depth mips, evaluate
// visibility/bent normals, denoise, optionally reproject, and optionally upsample.
// Publishes rgba16f View.GTAO (world bent normal in rgb, visibility in a).
// JSON: {"type":"AmbientOcclusion", "quality":"medium", "temporal":true,
//        "output":"View.GTAO"}. Quality: medium (half), high/ultra (full).
class AONode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "AmbientOcclusion"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void LoadShaders(Rendering::IDevice* device);

    enum Stage : size_t
    {
        Prepare,
        DepthMip,
        Sweep,
        Blur,
        Upsample,
        Temporal,
        StageCount
    };
    struct Shader
    {
        Rendering::ComputePipelineId Pipeline{};
        std::unique_ptr<Rendering::ShaderMeta> Meta;
        Rendering::DescriptorSetLayoutDesc Layout{};
    };
    struct HistoryRecord
    {
        uint64_t WrittenFrame = ~uint64_t{0};
        uint32_t Parity = 1;
        GtaoHistoryKey Key{};
        Rendering::CameraData Camera{};
    };
    /// Per-view state that outlives a frame. Pool names are built once so the
    /// declare path does not concatenate strings every frame; Committed is the
    /// history the last temporal dispatch actually wrote, Pending is what this
    /// frame's dispatch will commit if it runs to completion.
    struct ViewState
    {
        std::string DepthMipsPool;
        std::string OutputPool;
        std::array<std::string, 2> HistoryPool;
        std::array<std::string, 2> SurfacePool;
        HistoryRecord Committed;
        HistoryRecord Pending;
        // Any failed setup prevents dependent dispatches from treating unwritten
        // intermediates as valid and, especially, from accepting them as history.
        bool ChainValid = false;
    };

    ViewState& ViewStateFor(uint32_t viewId);

    std::string m_Id;
    std::string m_OutputKey = Names::View::GTAO;
    GtaoQuality m_Quality = GtaoQuality::Medium;
    bool m_Temporal = true;
    bool m_LoadAttempted = false;
    std::array<Shader, StageCount> m_Shaders{};
    std::unordered_map<uint32_t, ViewState> m_Views;
    Rendering::SamplerHandle m_Sampler{};
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
