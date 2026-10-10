#pragma once

#include <memory>
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Rendering
{
class IDevice;
struct CameraData;
struct ShaderMeta;
}
namespace GameEngine::Engine::Renderer
{
// The same per-view cascade constants consumed by directional lighting.
// No shadow-map texture is needed to project its fixed PCF footprint.
struct ScreenSpaceShadowFilter
{
    Rendering::BufferHandle Constants{};
    uint64_t Offset = 0;
    uint64_t Bytes = 0;
    uint32_t Resolution = 0;
    uint32_t Quality = 0;
};
// Device-local compute pass bundle, not a registered/global service. Owns the
// cached pipelines and sampler; RGFrame owns per-view textures and scratch.
// The world-pass contributor decides when these depth-only passes are needed.
class ScreenSpaceShadowPasses
{
  public:
    explicit ScreenSpaceShadowPasses(Rendering::IDevice* device);
    ~ScreenSpaceShadowPasses();
    // After an in-place device rebuild: the sampler died with the old device.
    // Forgets it without destroying it; the next pass recreates it.
    void OnDeviceRebuilt() { m_Sampler = {}; }
    Rendering::RenderGraph::RGTexture DeclareMaskPass(
        Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
        Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData& camera,
        const float towardLight[3], float thickness,
        const ScreenSpaceShadowFilter* filter = nullptr);

  private:
    Rendering::ComputePipelineId m_MatchPipeline{};
    std::unique_ptr<Rendering::ShaderMeta> m_MatchMeta;
    Rendering::DescriptorSetLayoutDesc m_MatchLayout{};
    bool LoadShader();
    Rendering::IDevice* m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::ComputePipelineId m_PreparePipeline{};
    Rendering::ComputePipelineId m_ResolvePipeline{};
    std::unique_ptr<Rendering::ShaderMeta> m_ResolveMeta;
    Rendering::DescriptorSetLayoutDesc m_ResolveLayout{};
    std::unique_ptr<Rendering::ShaderMeta> m_PrepareMeta;
    Rendering::DescriptorSetLayoutDesc m_PrepareLayout{};
    std::unique_ptr<Rendering::ShaderMeta> m_Meta;
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::SamplerHandle m_Sampler{};
    bool m_WarnedLoadFailure = false;
};
} // namespace GameEngine::Engine::Renderer
