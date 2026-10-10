#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Sky/SkyRenderer.h"

#include <string>
#include <unordered_map>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::BufferHandle;
using ::GameEngine::Rendering::IDevice;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
class SkyRenderNode final : public IRenderPipelineNode
{
  public:
    ~SkyRenderNode() override;
    const char* GetTypeName() const override { return "SkyRender"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void RebuildPassNames(const std::string& pipelineName);

    // Build the five transient descriptor set descs once from the renderer's
    // reflected binding slots. Cached as members so the per-frame execute
    // callbacks never heap-allocate a bindings vector (the property the old
    // static Get*DSD() helpers preserved).
    void EnsureDescriptorSetDescs(const Rendering::SkyRenderer::SkyBindingSlots& slots);

    Rendering::BufferHandle GetOrCreatePerViewSkyUBO(Rendering::IDevice* dev, uint32_t viewKey, uint32_t slot);

    std::string m_Id;
    std::string m_Json;

    std::string m_OutputRef = Names::View::Color;

    // Cached pass names for frame-scope passes
    std::string m_CachedPipelineName;
    std::string m_TransmittancePassName;
    std::string m_MultiscatterPassName;
    std::string m_SkyViewLutPassName;

    // Per-view sky UBOs to avoid multi-view overwrites on shared buffers.
    static constexpr uint32_t kMaxFrameSlots = Rendering::IDevice::kMaxSupportedFramesInFlight;
    struct PerViewUBOs
    {
        Rendering::BufferHandle skyUBO[kMaxFrameSlots] = {};
    };
    std::unordered_map<uint32_t, PerViewUBOs> m_PerViewUBOs;

    Rendering::IDevice* m_Device = nullptr;
    bool m_WarnedNoComponent = false;

    // Transient descriptor set descs, built once from reflected binding slots.
    bool m_DescriptorSetDescsBuilt = false;
    Rendering::DescriptorSetDesc m_TransmittanceDSD;
    Rendering::DescriptorSetDesc m_MultiscatterDSD;
    Rendering::DescriptorSetDesc m_SkyViewLutDSD;
    Rendering::DescriptorSetDesc m_SkyRenderDSD;
    Rendering::DescriptorSetDesc m_StarBillboardDSD;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
