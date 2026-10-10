#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
class CommandList;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

// Displacement-combine cascade (optional, OceanRenderer.CombineDisplacementCascade).
// Each frame it sums the tileable FFT displacement cascades into a single
// camera-snapped, viewer-centered Texture2DArray (one slice per LOD), per LOD only
// summing the cascades resolvable at that layer's texel (an anti-aliased LOD
// pyramid). The surface then samples ~2 adjacent slices instead of all 16 FFT
// cascades per vertex/fragment. A pure per-frame bake (no history -> single array,
// no ping-pong). One instance lives on OceanRenderFeature; a failed init leaves
// the surface on the direct 16-cascade sum (graceful degrade).
class OceanCombineSim
{
public:
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution), mirror
    // of OceanFoamSim::ConfigureCascades. Base scale applies to the next snap (no
    // recreation); a non-zero resolution resizes the cascade texture. 0 = unchanged.
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        if (baseScale > 0.0f) m_Combine.SetBaseScale(baseScale);
        if (resolution > 0u) m_Combine.Resize(device, resolution);
        m_Resolution = m_Combine.GetResolution();
    }

    // The combined displacement cascade + its sampler + snapped layout the surface
    // samples this frame.
    ::GameEngine::Rendering::TextureHandle GetTexture() const { return m_Combine.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const { return m_Combine.GetLayout(); }
    // RG import of the combine cascade at its resting state (see OceanCascadeArray::ImportRG).
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

    // Snap the cascade to the camera (call once per frame at schedule time, before
    // the forward contributor binds the texture + layout, so they agree).
    void BeginFrame(float cameraX, float cameraZ);
    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_Combine.RebaseOrigin(shiftX, shiftZ);
    }

    // Per-frame inputs: the FFT displacement cascade + its sampler, and the active
    // FFT cascade count (so the bake skips inactive layers).
    void SetFrameInputs(::GameEngine::Rendering::TextureHandle fftDisplacement,
                        ::GameEngine::Rendering::SamplerHandle fftSampler, uint32 fftCascadeCount,
                        float waveOriginOffsetX = 0.0f, float waveOriginOffsetZ = 0.0f);

    // Finalize this frame's GPU params (the cascade layout BeginFrame snapped).
    // Called at DECLARE time; the caller writes the result into a render-graph
    // upload-ring allocation and hands the {buffer, offset} to RecordDispatch.
    void FillParams(OceanCombineParamsGPU& out);

    // Records the combine dispatch with barriers, leaving the cascade sampleable by
    // the world pass. Params come from the frame's upload ring (FillParams above) —
    // no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

private:
    // RGBA16F displacement cascade; LOD 0 covers this many meters (each higher LOD
    // doubles). The surface clamps to the coarsest layer past baseScale*2^(N-1).
    static constexpr uint32 kCombineResolution = 256;
    static constexpr uint32 kCombineLodCount = kMaxOceanLodCascades;
    static constexpr float kCombineBaseScale = 64.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kCombineResolution;
    uint32 m_LodCount = kCombineLodCount;

    OceanCascadeArray m_Combine; // single array (per-frame bake, no ping-pong)

    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for surface read

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanCombineParamsGPU m_Params;

    // Per-frame inputs captured by SetFrameInputs.
    ::GameEngine::Rendering::TextureHandle m_FFTDisplacement;
    ::GameEngine::Rendering::SamplerHandle m_FFTSampler;
};

} // namespace GameEngine::Ocean
