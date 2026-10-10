#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
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
// Produces View.DepthResolved — always a single-sample R32F texture — so
// every depth-sampling consumer can use plain sampler2D with a sampled read.
//
// When sampleCount > 1 runs depth_resolve.comp (sampler2DMS → image2D R32F).
// When sampleCount == 1 runs depth_copy.comp (sampler2D D32 → image2D R32F).
//
// Must appear after DepthPrepass and before any depth-sampling node.
//
// The instance that publishes View.DepthResolved also publishes View.OccluderDepthResolved, the
// depth GTAO and the world pass's contact shadows read. When the view has non-occluding prepass
// heads (grass), it copies the depth twice around the non-occluding prepass
// (RenderServices::AddWorldNonOccludingDepthPrepassForView):
//   - View.OccluderDepthResolved: taken before the heads draw, so the blades neither occlude nor
//     darken GTAO and the contact shadows;
//   - View.DepthResolved: taken after them, so every other reader (the ocean surface and its
//     underwater composite, the fogs, SSR, the transparents, the HZB) sees the depth the colour
//     shows. A reader that decides visibility from this depth would otherwise draw over the blades.
// Without heads it copies once and publishes that copy under both names.
//
// JSON shape (schemaVersion 2):
// {
//   "id": "DepthResolve",
//   "type": "DepthResolve",
//   "enabled": true,
//   "output": "View.DepthResolved",   // optional blackboard key to publish
//   "poolName": "Pipeline.DepthResolved.View" // optional persistent-pool prefix
// }
//
// A second instance placed AFTER the ocean surface (with a distinct output/pool)
// gives a depth that includes transparent water, which the volumetric fog needs
// so it fogs the ocean by the water's own distance rather than the sky behind it.
class DepthResolveNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "DepthResolve"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void LoadShaders(IDevice* device);
    // The persistent single-sample R32F target one copy writes (pool name `poolName`), sized w x h.
    Rendering::RenderGraph::RGTexture ImportResolveTarget(ViewDeclare& d, const std::string& poolName,
                                                          uint32_t width, uint32_t height) const;
    // Declares one copy of `depth` into `target` (resolve when `depth` is multisampled).
    Rendering::RenderGraph::RGPass DeclareResolvePass(ViewDeclare& d, const char* passName,
                                                      Rendering::RenderGraph::RGTexture depth,
                                                      Rendering::RenderGraph::RGTexture target, uint32_t width,
                                                      uint32_t height);
    void RecordResolve(Rendering::RenderGraph::RGContext& ctx, Rendering::RenderGraph::RGTexture depth,
                       Rendering::RenderGraph::RGTexture target, uint32_t width, uint32_t height) const;

    std::string m_Id;
    std::string m_OutputKey = Names::View::DepthResolved;
    std::string m_PoolPrefix = "Pipeline.DepthResolved.View";

    bool m_LoadAttempted = false;

    // depth_resolve.comp — sampler2DMS input (MSAA path)
    ComputePipelineId m_ResolvePipelineId{};
    std::unique_ptr<ShaderMeta> m_ResolveMeta;
    DescriptorSetLayoutDesc m_ResolveSet0Layout{};

    // depth_copy.comp — sampler2D input (1-sample path)
    ComputePipelineId m_CopyPipelineId{};
    std::unique_ptr<ShaderMeta> m_CopyMeta;
    DescriptorSetLayoutDesc m_CopySet0Layout{};

    SamplerHandle m_Sampler{};
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
