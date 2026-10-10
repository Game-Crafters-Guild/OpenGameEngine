#pragma once

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"
#include "Rendering/CameraTypes.h"
#include "Types/Types.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Ocean
{

struct OceanRasterDepthCaptureSettings
{
    bool Enabled = false;
    uint32 Resolution = 512u;
    uint32 RenderLayerMask = 0xFFFFFFFFu;
    float32 SizeX = 512.0f;
    float32 SizeZ = 512.0f;
    float32 TopPadding = 64.0f;
    float32 DeepWaterDepth = 60000.0f;
};

// Hidden top-down dynamic raster producer for the seabed-depth cascade. It renders
// regular MeshRenderer geometry through a secondary orthographic view, then
// converts that reverse-Z depth buffer into an R32F water-depth texture. The
// texture is consumed by OceanSeabedDepth with one-frame latency.
class OceanRasterDepthCapture
{
public:
    ~OceanRasterDepthCapture();

    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                        float32 centerX, float32 centerZ, float32 seaLevel,
                        const OceanRasterDepthCaptureSettings& settings);

    bool HasSampleableCapture(uint32 /*frameIndex*/) const
    {
        return m_OutputTexture.IsValid() && m_Sampler.IsValid() &&
               m_HasSampleableTexture;
    }

    ::GameEngine::Rendering::TextureHandle GetTexture() const { return m_OutputTexture; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    // Import the water-depth output at its resting-state contract (rests in
    // ShaderResource; the readback frames MarkOutput it back). Centralizes the
    // claim — the seabed bake's declared read and the readback pass must agree
    // (import dedups by physical, first claim wins). Invalid when not ready.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    float32 GetOriginX() const { return m_OriginX; }
    float32 GetOriginZ() const { return m_OriginZ; }
    float32 GetSizeX() const { return m_SizeX; }
    float32 GetSizeZ() const { return m_SizeZ; }
    float32 GetDeepWaterDepth() const { return m_DeepWaterDepth; }
    ::GameEngine::Rendering::ViewId GetCaptureViewId() const { return m_ViewId; }
    bool SampleDepth(float32 worldX, float32 worldZ, float32& outDepth) const;
    void InvalidateReadback();

    // Post-submit stamp: attaches the submission's fence token to this frame's
    // pending readback (RGReadbackRing completion contract).
    void OnFrameSubmitted(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                          const ::GameEngine::Rendering::IDevice::GpuSyncToken& token)
    {
        m_Readback.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetired(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame)
    {
        m_Readback.OnFrameStreamRetired(frame);
    }

private:
    bool EnsurePipeline(::GameEngine::Rendering::IDevice& device);
    bool EnsureOutput(::GameEngine::Rendering::IDevice& device, uint32 resolution);
    bool EnsureReadbackRing(::GameEngine::Rendering::IDevice& device, uint32 resolution);
    void EnsureView(Engine::Renderer::RenderServices& rs,
                    const ::GameEngine::Rendering::ViewDesc& mainView);
    ::GameEngine::Rendering::CameraData MakeTopDownCamera(float32 centerX, float32 centerZ,
                                                          float32 seaLevel,
                                                          const OceanRasterDepthCaptureSettings& settings) const;
    void ScheduleReadback(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                          ::GameEngine::Rendering::RenderGraph::RGTexture output,
                          uint32 resolution);
    void ResolveReadyReadback();

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    bool m_PipelineLoadAttempted = false;
    bool m_WarnedLoadFailed = false;

    ::GameEngine::Rendering::ViewId m_ViewId = 0;
    ::GameEngine::Rendering::CameraId m_CameraId = 0;
    bool m_ViewAllocated = false;
    // OnDemand capture cycle: the view is fed only on requested frames, and a
    // request made during declaration covers the NEXT frame (extraction for the
    // current frame already ran). Arm on the dirty frame, capture on the next.
    bool m_CaptureArmed = false;
    uint64 m_ArmedAtFrame = 0;

    ::GameEngine::Rendering::TextureHandle m_OutputTexture;
    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    uint32 m_Resolution = 0u;
    bool m_HasSampleableTexture = false;
    uint32 m_FramesUntilSampleable = 0u;
    bool m_HaveLastSampleableTickFrame = false;
    uint64 m_LastSampleableTickFrame = 0u;

    bool m_HasCaptureFootprint = false;
    bool m_HaveLastCaptureFrame = false;
    uint64 m_LastCaptureFrame = 0u;
    uint32 m_CaptureMask = 0u;
    uint64 m_CaptureWorldId = 0u;
    float32 m_CaptureSeaLevel = 0.0f;
    float32 m_CaptureTopPadding = 0.0f;

    float32 m_OriginX = 0.0f;
    float32 m_OriginZ = 0.0f;
    float32 m_SizeX = 1.0f;
    float32 m_SizeZ = 1.0f;
    float32 m_DeepWaterDepth = 60000.0f;

    ::GameEngine::Rendering::ComputePipelineId m_PipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};

    // Footprint metadata stamped when the copy is scheduled; the CPU sample
    // window must match the tile the data was CAPTURED for, not the current one.
    struct ReadbackMeta
    {
        float32 OriginX = 0.0f;
        float32 OriginZ = 0.0f;
        float32 SizeX = 1.0f;
        float32 SizeZ = 1.0f;
        float32 Deep = 60000.0f;
    };
    ::GameEngine::Rendering::RenderGraph::RGReadbackRing<ReadbackMeta> m_Readback;
    uint32 m_ReadbackResolution = 0u;

    mutable std::mutex m_ReadbackMutex;
    std::vector<float32> m_CPUWaterDepth;
    bool m_HasReadbackData = false;
    float32 m_ReadOriginX = 0.0f;
    float32 m_ReadOriginZ = 0.0f;
    float32 m_ReadSizeX = 1.0f;
    float32 m_ReadSizeZ = 1.0f;
    float32 m_ReadDeepWaterDepth = 60000.0f;
};

} // namespace GameEngine::Ocean
