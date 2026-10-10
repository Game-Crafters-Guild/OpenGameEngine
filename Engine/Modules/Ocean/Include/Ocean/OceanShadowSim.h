#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanFrameStamp.h"
#include "Ocean/OceanInputDrawSource.h"
#include "Ocean/OceanSettingsAsset.h"
#include "Rendering/CameraTypes.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
}
namespace GameEngine::Ocean
{
// Per-view histories: each engine view has its own directional shadow camera.
struct alignas(16) OceanShadowSamplingGPU
{
    OceanCascadeLayoutGPU Layout{};
    float Channels[4]{}; // hard scale, soft scale, available, pad
};

class OceanShadowSim
{
  public:
    OceanShadowSim();
    ~OceanShadowSim();
    void SetSettings(const OceanShadowSettings &settings)
    {
        m_Settings = settings;
    }
    void SetInputs(std::vector<OceanInputDrawPacket> inputs)
    {
        m_Inputs = std::move(inputs);
    }
    void RebaseOrigin(float x, float z);
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, const OceanParamsGPU &params);
    // `frame` is the render graph's RGFrame::FrameIndex(), the one DeclareForView stamped.
    bool FillSampling(Rendering::ViewId view, uint64 frame, OceanShadowSamplingGPU &out) const;
    Rendering::TextureHandle GetTexture(Rendering::ViewId view) const;
    Rendering::SamplerHandle GetSampler() const
    {
        return m_Sampler;
    }
    Rendering::RenderGraph::RGTexture ImportRG(Rendering::RenderGraph::RGFrame &frame,
                                               Rendering::ViewId view) const;

  private:
    struct History;
    bool Initialize(Rendering::IDevice *device);
    Rendering::IDevice *m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::SamplerHandle m_Sampler{};
    bool m_LoadFailed = false;
    OceanShadowSettings m_Settings{};
    std::vector<OceanInputDrawPacket> m_Inputs;
    std::unordered_map<Rendering::ViewId, std::unique_ptr<History>> m_Histories;
};
} // namespace GameEngine::Ocean
