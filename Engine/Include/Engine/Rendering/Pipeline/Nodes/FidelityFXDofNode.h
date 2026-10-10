#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <array>
#include <memory>
#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// AMD FidelityFX DoF 1.1-style compute pipeline supplied by the
// Packages/fidelityfx-dof package. The node owns render-graph resources;
// the package owns the shaders and license notice.
class FidelityFXDofNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "FidelityFXDepthOfField"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    struct ComputeStage
    {
        GameEngine::Rendering::ComputePipelineId Pipeline{};
        std::unique_ptr<GameEngine::Rendering::ShaderMeta> Meta;
        GameEngine::Rendering::DescriptorSetLayoutDesc Set0{};
    };

    void LoadShaders(GameEngine::Rendering::IDevice* device);

    std::string m_Id;
    std::string m_InputKey = "HDRDefocused";
    std::string m_DepthKey = "View.DepthResolved";
    std::string m_OutputKey = "HDRPhysicalDoF";
    std::string m_ExposureBufferKey = "ExposureHistory";
    std::string m_RequiredPackage = "fidelityfx-dof";
    bool m_LoadAttempted = false;
    std::array<ComputeStage, 5> m_Stages{};
    GameEngine::Rendering::TextureFormat m_CocFormat = GameEngine::Rendering::TextureFormat::Unknown;
    GameEngine::Rendering::SamplerHandle m_LinearSampler{};
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
