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

// Persistent foam simulation. Each frame it advects the previous frame's foam,
// decays it exponentially, and deposits new foam where the FFT displacement
// folds (the Jacobian goes negative — breaking/pinching crests). The result is
// a camera-snapped R16F cascade the surface samples for whitecaps, so foam
// accumulates on crests and trails behind waves over time rather than being a
// per-pixel function of the instantaneous Jacobian.
//
// Ping-pong over two persistent OceanCascadeArrays: read previous, write next.
// One instance lives on OceanRenderFeature. RecordDispatch() records the compute
// pass (with manual barriers) onto a command list, mirroring OceanFFT. A failed
// init leaves the surface on its per-pixel Jacobian foam (graceful degrade).
class OceanFoamSim
{
public:
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution). Base
    // scale applies to the next snap (no recreation); a non-zero resolution resizes
    // the ping-pong cascade textures. 0 = leave unchanged.
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        for (auto& a : m_Foam)
        {
            if (baseScale > 0.0f) a.SetBaseScale(baseScale);
            if (resolution > 0u) a.Resize(device, resolution);
        }
        m_Resolution = m_Foam[0].GetResolution();
    }

    // The foam cascade the surface samples this frame (the just-written target).
    ::GameEngine::Rendering::TextureHandle GetFoamTexture() const;
    ::GameEngine::Rendering::SamplerHandle GetFoamSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetFoamLayout() const;

    // RG imports of the ping-pong pair at their resting state (call after
    // BeginFrame flipped the index). Write = this frame's target (the texture
    // GetFoamTexture returns), Prev = last frame's history the dispatch samples.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportWriteRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportPrevRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportWriteRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 writeIndex) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportPrevRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 writeIndex) const;

    // Begin a new sim frame (call once per frame at schedule time, before the
    // forward contributor binds the foam texture): advance the ping-pong index so
    // the frame's write target is fixed, then snap only that target to the camera.
    // The read target keeps the layout it was written with so history can be sampled
    // by world position. RecordDispatch (which runs at graph-execution time) must
    // not mutate the index — only this does — so the texture the surface binds is
    // the one the dispatch writes.
    void BeginFrame(float cameraX, float cameraZ);
    uint32 BeginSubstep(float cameraX, float cameraZ);
    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_Foam[0].RebaseOrigin(shiftX, shiftZ);
        m_Foam[1].RebaseOrigin(shiftX, shiftZ);
    }

    // Per-frame inputs: the FFT displacement cascade + its sampler (the Jacobian
    // source), the active FFT cascade count, the foam controls, and dt.
    void SetFrameInputs(::GameEngine::Rendering::TextureHandle fftDisplacement,
                        ::GameEngine::Rendering::SamplerHandle fftSampler, uint32 fftCascadeCount,
                        float fadeRate, float coverage, float strength, float deltaTime,
                        float shapeWeight, float shapeMaxHorizontalDisplacement,
                        float shapeMaxVerticalDisplacement, float shapeRespectShallowAttenuation,
                        float shapeSubSurfaceDepthMax, float windVelocityX, float windVelocityZ,
                        float waveOriginOffsetX = 0.0f, float waveOriginOffsetZ = 0.0f);
    void ConfigureSimulation(float32 frequency, uint32 maximumSubsteps);
    void ClearSimulationConfiguration();
    uint32 GetPendingSubstepCount() const { return m_PendingSubsteps; }
    void FinishSubsteps()
    {
        m_PendingSubsteps = 0u;
        m_SkippedTime = 0.0f;
        m_ResetHistory = false;
    }
    void InvalidateHistory()
    {
        m_ResetHistory = true;
        m_TimeAccumulator = 0.0f;
        m_SkippedTime = 0.0f;
    }

    // Optional combined displacement source. Kept for a future world-stable combine
    // path; the render node currently leaves it unavailable so foam sums the raw FFT
    // cascades directly.
    void SetCombinedInputs(::GameEngine::Rendering::TextureHandle combinedDisplacement,
                           ::GameEngine::Rendering::SamplerHandle combinedSampler,
                           const OceanCascadeLayoutGPU& layout,
                           bool available);
    void SetAuthoredSources(const OceanFoamInputGPU* sources, uint32 count);

    // Shoreline-foam inputs (Phase 5): the seabed depth cascade (shares this
    // foam cascade's layout) + its sampler, the shoreline controls, and whether
    // any depth content is active. Pass a valid texture even when no depth content
    // is present — binding 4 must always have a real
    // sampler2DArray; SeabedDepthAvailable=false keeps the term off.
    void SetShorelineInputs(::GameEngine::Rendering::TextureHandle seabedDepth,
                            ::GameEngine::Rendering::SamplerHandle seabedSampler, float maxDepth,
                            float strength, bool available);

    // Flow-advection inputs (Phase 8): the flow cascade (shares this foam
    // cascade's layout) + its sampler, and whether a flow field was baked this
    // frame (gates the advection). Pass a valid texture even when no flow is
    // present — binding 5 must always have a real sampler2DArray; available=false
    // keeps the advection at zero (foam read in place). flowScale converts the
    // cascade's m/s velocity to the advection step magnitude.
    void SetFlowInputs(::GameEngine::Rendering::TextureHandle flow,
                       ::GameEngine::Rendering::SamplerHandle flowSampler, float flowScale,
                       bool available);

    // Finalize this frame's GPU params (cascade layouts follow the ping-pong
    // index BeginFrame fixed). Called at DECLARE time; the caller writes the
    // result into a render-graph upload-ring allocation and hands the
    // {buffer, offset} to RecordDispatch.
    void FillParams(OceanFoamParamsGPU& out);
    void FillParams(OceanFoamParamsGPU& out, uint32 writeIndex,
                    uint32 substepIndex) const;

    // Records the foam advect/decay/accumulate dispatch with barriers, leaving
    // the written foam cascade sampleable by the world pass. BeginFrame selects
    // the ping-pong target. Params come from the frame's upload ring (FillParams
    // above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset, uint32 writeIndex);

private:
    // RGBA16F foam cascade; LOD 0 covers this many meters (each higher LOD doubles).
    static constexpr uint32 kFoamResolution = 256;
    static constexpr uint32 kFoamLodCount = 4;
    static constexpr float kFoamBaseScale = 64.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kFoamResolution;
    uint32 m_LodCount = kFoamLodCount;

    // Ping-pong foam cascades. m_WriteIndex is written this frame; the other is
    // read with its previous snapped layout.
    OceanCascadeArray m_Foam[2];
    uint32 m_WriteIndex = 0;
    bool m_UseFixedStep = false;
    float32 m_SimulationFrequency = 30.0f;
    uint32 m_MaximumSubsteps = 2u;
    float32 m_TimeAccumulator = 0.0f;
    float32 m_SubstepDelta = 0.0f;
    uint32 m_PendingSubsteps = 0u;
    // Seconds of substeps that were pending when the next frame's inputs arrived:
    // frames that dispatched no foam (no water in view). The next dispatched
    // step covers them in one closed-form step (FillParams).
    float32 m_SkippedTime = 0.0f;
    bool m_ResetHistory = false;

    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for advection + surface read

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanFoamParamsGPU m_Params;

    // Per-frame inputs captured by SetFrameInputs.
    ::GameEngine::Rendering::TextureHandle m_FFTDisplacement;
    ::GameEngine::Rendering::SamplerHandle m_FFTSampler;
    ::GameEngine::Rendering::TextureHandle m_CombinedDisplacement;
    ::GameEngine::Rendering::SamplerHandle m_CombinedSampler;

    // Shoreline inputs (the seabed depth cascade for the shoreline term).
    ::GameEngine::Rendering::TextureHandle m_SeabedDepth;
    ::GameEngine::Rendering::SamplerHandle m_SeabedSampler;

    // Flow inputs (the flow cascade advecting the foam).
    ::GameEngine::Rendering::TextureHandle m_Flow;
    ::GameEngine::Rendering::SamplerHandle m_FlowSampler;
};

} // namespace GameEngine::Ocean
