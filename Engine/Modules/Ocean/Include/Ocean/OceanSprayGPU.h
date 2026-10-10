#pragma once
#include "Components/Rendering/Ocean.h"
#include "Ocean/OceanFrameStamp.h"
#include "Rendering/Core/Device.h"
#include "Rendering/CameraTypes.h"
#include <string>
#include <unordered_map>
namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
}
namespace GameEngine::Ocean
{
class OceanRenderFeature;
class OceanSprayGPU
{
  public:
    ~OceanSprayGPU();
    void SetSettings(const Components::OceanSurface &surface, float windSpeed, float windDegrees);
    bool Enabled() const
    {
        return m_Surface.Spray;
    }
    void RebaseOrigin(float x, float y, float z);
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, OceanRenderFeature &ocean);

  private:
    bool Initialize(Rendering::IDevice *device);
    struct History
    {
        float Time = 0;
        // Ocean time of the last frame that could emit; once every droplet
        // born by then has landed, a calm view schedules no spray passes.
        float LastEmissionTime = 0;
        float Origin[3]{};
        // Persistent render-graph buffer names, built on the view's first frame.
        std::string StateName, VisibleName, ArgsName;
        bool Active = false;
        OceanFrameStamp Declared;
        std::chrono::steady_clock::time_point LastUsed{};
    };
    Rendering::IDevice *m_Device = nullptr;
    Components::OceanSurface m_Surface{};
    float m_WindSpeed = 0, m_WindDegrees = 0, m_Origin[3]{};
    Rendering::SamplerHandle m_Sampler{};
    bool m_LoadFailed = false;
    Rendering::ComputePipelineId m_Simulate{}, m_Cull{};
    Rendering::GraphicsPipelineId m_Draw{};
    Rendering::DescriptorSetLayoutDesc m_SimLayout{}, m_CullLayout{}, m_DrawLayout{}, m_OceanLayout{};
    std::unordered_map<Rendering::ViewId, History> m_History;
};
} // namespace GameEngine::Ocean
