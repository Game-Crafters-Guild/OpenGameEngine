#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"

#include <array>
#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
class CommandList;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

// Local wave-override bake. Writes an RGBA16F cascade where R is wave
// weight/amplitude, G is horizontal chop scale, B is local directional-wave
// height, and A is aggregate local FFT blend. Companion RGBA16F cascades store
// one local FFT blend weight per local spectrum stream (four streams per page).
// Defaults are (1,1,0,0) and zero local FFT masks, so scenes with no source keep
// the global spectrum unchanged.
class OceanWaveMaskSim
{
public:
    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_Mask.RebaseOrigin(shiftX, shiftZ);
        for (auto& mask : m_LocalFFTMasks)
            mask.RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        if (baseScale > 0.0f)
        {
            m_Mask.SetBaseScale(baseScale);
            for (auto& localMask : m_LocalFFTMasks)
                localMask.SetBaseScale(baseScale);
        }
        if (resolution > 0u)
        {
            m_Mask.Resize(device, resolution);
            for (auto& localMask : m_LocalFFTMasks)
                localMask.Resize(device, resolution);
        }
        m_Resolution = m_Mask.GetResolution();
    }

    ::GameEngine::Rendering::TextureHandle GetTexture() const { return m_Mask.GetTexture(); }
    ::GameEngine::Rendering::TextureHandle GetLocalFFTMaskTexture(uint32 page = 0u) const
    {
        return (page < kOceanLocalFFTMaskPages) ? m_LocalFFTMasks[page].GetTexture()
                                                : ::GameEngine::Rendering::TextureHandle{};
    }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const { return m_Mask.GetLayout(); }
    // RG imports of the mask cascade / local-FFT mask pages at their resting
    // state (see OceanCascadeArray::ImportRG).
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportMaskRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportLocalFFTMaskRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 page) const;

    bool HasSources() const
    {
        return m_Params.SourceCount > 0 || m_Params.PolygonCount > 0 ||
               m_Params.TextureSourceCount > 0;
    }

    void BeginFrame(float cameraX, float cameraZ);
    void SetTime(float time, float originOffsetX = 0.0f, float originOffsetZ = 0.0f)
    {
        m_Params.Time = time;
        m_Params.WaveOriginOffsetX = originOffsetX;
        m_Params.WaveOriginOffsetZ = originOffsetZ;
    }
    void SetSources(const OceanWaveMaskSourceGPU* sources, uint32 count);
    void SetPolygonSources(const OceanWaveMaskPolygonGPU* polygons, uint32 count);
    void SetTextureSources(const OceanWaveMaskTextureSourceGPU* sources,
                           const ::GameEngine::Rendering::TextureHandle* textures,
                           uint32 count);

    // Finalize this frame's GPU params (cascade layout snapped by BeginFrame).
    // Called at DECLARE time; the caller writes the result into a render-graph
    // upload-ring allocation and hands the {buffer, offset} to RecordDispatch.
    void FillParams(OceanWaveMaskParamsGPU& out);

    // Records the mask-bake dispatch with barriers, leaving the cascades
    // sampleable by the world pass. Params come from the frame's upload ring
    // (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

private:
    static constexpr uint32 kResolution = 256;
    static constexpr uint32 kLodCount = 4;
    static constexpr float kBaseScale = 64.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kResolution;
    uint32 m_LodCount = kLodCount;

    OceanCascadeArray m_Mask;
    std::array<OceanCascadeArray, kOceanLocalFFTMaskPages> m_LocalFFTMasks;
    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    ::GameEngine::Rendering::TextureHandle m_DummyWaveMaskTexture;
    std::array<::GameEngine::Rendering::TextureHandle, kMaxOceanWaveMaskTextureSources>
        m_WaveMaskTextures{};

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;
    OceanWaveMaskParamsGPU m_Params;
};

} // namespace GameEngine::Ocean
