#pragma once

#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc

#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
class CommandList;
} // namespace GameEngine::Rendering

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
struct RGTexture;
} // namespace GameEngine::Rendering::RenderGraph

namespace GameEngine::Ocean
{

// Turns a wind spectrum into a tileable displacement cascade array via FFT, a
// faithful port of the reference's compute chain (SpectrumInit -> SpectrumUpdate ->
// butterfly IFFT rows -> cols). Owns its GPU textures (stable handles, so the
// displacement can be bound on the ocean surface draw) and compute pipelines.
//
// One instance lives on OceanRenderFeature. RecordDispatch() records all passes
// onto a command list (called inside one render-graph pass); the surface samples
// GetDisplacementTexture() (Repeat/Linear), summing cascades in the vertex modifier.
class OceanFFT
{
public:
    ~OceanFFT();

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    // Per-frame parameters (filled by OceanExtractionSystem). spectrumDirty forces
    // the otherwise-cached H0 spectrum-init to re-run (wind/spectrum changed).
    void SetParams(const OceanFFTParamsGPU& params, bool spectrumDirty);

    // Finalize this frame's GPU params. Called at DECLARE time; the caller writes
    // the result into a render-graph upload-ring allocation and hands the
    // {buffer, offset} to RecordDispatch.
    void FillParams(OceanFFTParamsGPU& out);

    // Records SpectrumInit (when dirty) -> SpectrumUpdate -> FFT rows -> FFT cols,
    // with barriers, leaving the displacement array sampleable by the world pass.
    // All four dispatches bind the same params. Params come from the frame's
    // upload ring (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

    ::GameEngine::Rendering::TextureHandle GetDisplacementTexture() const { return m_Displacement; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    uint32 GetCascadeCount() const { return kOceanFFTCascades; }

    // Import the displacement into the frame's render graph at its resting
    // state (created at ShaderResource; every dispatching frame restores it).
    // Dedups by physical handle — producer and consumer arms may both import.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportDisplacementRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

private:
    bool CreateTextures();
    bool CreateButterflyTable();
    bool CompilePipelines();

    // Records one compute pass: bind UBO (binding 0) + the given storage images,
    // then dispatch. images[i] binds at slot startSlot+i.
    void RecordPass(::GameEngine::Rendering::IDevice* device,
                    ::GameEngine::Rendering::CommandList* cl,
                    ::GameEngine::Rendering::ComputePipelineId pipeline,
                    const ::GameEngine::Rendering::DescriptorSetLayoutDesc& layout,
                    ::GameEngine::Rendering::BufferHandle paramsBuffer,
                    uint64 paramsOffset,
                    const ::GameEngine::Rendering::TextureHandle* images,
                    const uint32* slots, uint32 imageCount,
                    uint32 groupsX, uint32 groupsY, uint32 groupsZ);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    bool m_SpectrumDirty = true;
    bool m_DisplacementWritten = false;

    // Cascade textures (all kOceanFFTResolution^2 x kOceanFFTCascades arrays).
    ::GameEngine::Rendering::TextureHandle m_H0;          // rgba32f (ampPos, ampNeg)
    ::GameEngine::Rendering::TextureHandle m_SpecH;       // rg32f spectral height
    ::GameEngine::Rendering::TextureHandle m_SpecX;       // rg32f spectral displaceX
    ::GameEngine::Rendering::TextureHandle m_SpecZ;       // rg32f spectral displaceZ
    ::GameEngine::Rendering::TextureHandle m_TmpH;        // rg32f after row pass
    ::GameEngine::Rendering::TextureHandle m_TmpX;
    ::GameEngine::Rendering::TextureHandle m_TmpZ;
    ::GameEngine::Rendering::TextureHandle m_Displacement; // rgba16f (dispX, height, dispZ)
    ::GameEngine::Rendering::TextureHandle m_Butterfly;    // rgba32f res x passes twiddles
    ::GameEngine::Rendering::SamplerHandle m_Sampler;      // repeat/linear for surface sampling

    ::GameEngine::Rendering::ComputePipelineId m_InitPipe{};
    ::GameEngine::Rendering::ComputePipelineId m_UpdatePipe{};
    ::GameEngine::Rendering::ComputePipelineId m_FFTHPipe{};
    ::GameEngine::Rendering::ComputePipelineId m_FFTVPipe{};

    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_InitLayout;
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_UpdateLayout;
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_FFTHLayout;
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_FFTVLayout;

    OceanFFTParamsGPU m_Params;
};

} // namespace GameEngine::Ocean
