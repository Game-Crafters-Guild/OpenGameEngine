#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/VolumetricFogSettings.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <unordered_map>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
}

namespace GameEngine::Engine::Renderer
{

class RenderServices;

class VolumetricFogRenderer final : public IRenderFeature
{
  public:
    ~VolumetricFogRenderer() override;

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }
    // A failed Initialize is terminal for the session (the missing shaders or
    // capabilities do not appear later); callers must not retry per frame.
    bool InitializeFailed() const { return m_InitAttempted && !m_Initialized; }

    void SetSettings(::GameEngine::Rendering::ViewId viewId, const VolumetricFogSettings& settings);
    const VolumetricFogSettings& GetSettings(::GameEngine::Rendering::ViewId viewId) const;
    void SetEnabled(::GameEngine::Rendering::ViewId viewId, bool enabled);
    bool IsEnabled(::GameEngine::Rendering::ViewId viewId) const;
    void ResetHistory(::GameEngine::Rendering::ViewId viewId);

    // RenderGraph declaration path: declare this frame's fog chain into the view's
    // scene color. Settings/sequencing resolve at declaration (the FillParams
    // history gating, the two upload-ring allocs, the froxel transients, the
    // history imports); exec lambdas capture by value and only record. The
    // caller has already gated on depth/feature-init/enabled.
    void DeclareForView(Pipeline::ViewDeclare& d,
                        ::GameEngine::Rendering::RenderGraph::RGTexture sceneColor,
                        ::GameEngine::Rendering::RenderGraph::RGTexture depth);

  private:
    struct PerViewState
    {
        VolumetricFogSettings settings{};
        ::GameEngine::Rendering::BufferHandle ubo[::GameEngine::Rendering::IDevice::kMaxSupportedFramesInFlight]{};
        ::GameEngine::Rendering::BufferHandle localVolumeBuffer[::GameEngine::Rendering::IDevice::kMaxSupportedFramesInFlight]{};
        VolumetricFogGrid grid{};
        VolumetricFogGrid historyGrid{};
        float effectiveMaxDistance = 0.0f;
        float historyMaxDistance = 0.0f;
        float currentSunDirection[3]{0.35f, -0.65f, 0.68f};
        float currentSunColor[3]{1.0f, 0.86f, 0.62f};
        float currentSunIntensity = 1.0f;
        bool hasLocalFogLights = false;
        float historySunDirection[3]{0.35f, -0.65f, 0.68f};
        float historySunColor[3]{1.0f, 0.86f, 0.62f};
        float historySunIntensity = 1.0f;
        uint64_t frameCounter = 0;
        uint64_t historyFrame = 0;
        bool historyValid = false;
        float previousViewProj[16]{};
        float previousCameraPos[4]{};
    };

    struct FogGpuParams;

    bool CreatePipelines();
    bool CreateJitterAtlasTexture();
    void DestroyResources();
    ::GameEngine::Rendering::BufferHandle GetOrCreateUBO(PerViewState& state, uint32_t slot);
    ::GameEngine::Rendering::BufferHandle GetOrCreateLocalVolumeBuffer(PerViewState& state, uint32_t slot);
    // Lazily-created zero-fallback buffers for the RenderGraph lighting bind (a missing
    // cluster/light ref binds these so the fog dispatch still runs).
    ::GameEngine::Rendering::BufferHandle GetZeroShadowBuffer(::GameEngine::Rendering::IDevice* dev);
    ::GameEngine::Rendering::BufferHandle GetZeroStorageBuffer(::GameEngine::Rendering::IDevice* dev);
    bool CreateDensityNoiseAtlasTexture();
    void FillParams(RenderServices& services,
                    ::GameEngine::Rendering::ViewId viewId,
                    const ::GameEngine::Rendering::CameraData& camera,
                    const VolumetricFogGrid& grid,
                    float effectiveMaxDistance,
                    uint32_t renderWidth,
                    uint32_t renderHeight,
                    FogGpuParams& out) const;

    ::GameEngine::Rendering::GraphicsPipelineId EnsureCompositePipeline(::GameEngine::Rendering::IDevice& device);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;
    bool m_InitAttempted = false;
    bool m_WarnedShaderLoad = false;
    bool m_WarnedCullShaderLoad = false;

    ::GameEngine::Rendering::PipelineDesc m_CullPipeline{};
    ::GameEngine::Rendering::PipelineDesc m_MediaPipeline{};
    ::GameEngine::Rendering::PipelineDesc m_LightingPipeline{};
    ::GameEngine::Rendering::PipelineDesc m_FilterPipeline{};
    ::GameEngine::Rendering::PipelineDesc m_IntegratePipeline{};
    ::GameEngine::Rendering::PipelineDesc m_TemporalPipeline{};
    ::GameEngine::Rendering::PipelineDesc m_CompositePipeline{};
    ::GameEngine::Rendering::ComputePipelineId m_CullPipelineId{};
    ::GameEngine::Rendering::ComputePipelineId m_MediaPipelineId{};
    ::GameEngine::Rendering::ComputePipelineId m_LightingPipelineId{};
    ::GameEngine::Rendering::ComputePipelineId m_FilterPipelineId{};
    ::GameEngine::Rendering::ComputePipelineId m_IntegratePipelineId{};
    ::GameEngine::Rendering::ComputePipelineId m_TemporalPipelineId{};
    ::GameEngine::Rendering::GraphicsPipelineId m_CompositePipelineId{};

    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_ComputeLayout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CullLayout{};
    // Per-pass copies of the superset above, each carrying the image shapes ITS
    // shader declares. WebGPU validates a bind group against the layout, and
    // the passes disagree — binding 3 is a sampler3D in the temporal pass and a
    // samplerCube in the media pass, binding 14 a cube only in lighting — so
    // one shared layout cannot describe all five.
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_MediaLayout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_LightingLayout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_FilterLayout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_IntegrateLayout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_TemporalLayout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CompositeLayout{};
    ::GameEngine::Rendering::SamplerHandle m_LinearClampSampler{};
    ::GameEngine::Rendering::SamplerHandle m_JitterAtlasSampler{};
    ::GameEngine::Rendering::TextureHandle m_JitterAtlasTexture{};
    ::GameEngine::Rendering::SamplerHandle m_DensityNoiseSampler{};
    ::GameEngine::Rendering::TextureHandle m_DensityNoiseTexture{};
    ::GameEngine::Rendering::BufferHandle m_ZeroShadowBuffer{};
    ::GameEngine::Rendering::BufferHandle m_ZeroStorageBuffer{};

    mutable VolumetricFogSettings m_DefaultSettings{};
    std::unordered_map<::GameEngine::Rendering::ViewId, PerViewState> m_PerView;
};

} // namespace GameEngine::Engine::Renderer
