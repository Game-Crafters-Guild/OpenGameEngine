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

// Dynamic (interactive) wave simulation. Solves the 2D wave equation on a
// camera-snapped RG16F (height, velocity) cascade and injects per-frame impulses
// (a buoyant body striking the water, a splash), producing spreading, decaying
// ripples layered on top of the FFT spectrum. The dynamic height is
// sampled by the surface vertex modifier (displacement) and fragment shader
// (normal fold).
//
// Ping-pong over two persistent OceanCascadeArrays: read previous, write next —
// the wave equation needs the prior state. One instance lives on
// OceanRenderFeature. RecordDispatch() records the compute pass (with manual
// barriers) onto a command list, mirroring OceanFoamSim. A failed init leaves the
// surface on the spectrum waves only (DynamicWavesAvailable stays 0).
//
// The cascade shares the foam cascade's resolution / LOD count / base scale; it
// snaps on the frames it steps, so the surface samples it with its own layout
// (OceanSampledCascade::DynWaves).
class OceanDynWaves
{
public:
    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_State[0].RebaseOrigin(shiftX, shiftZ);
        m_State[1].RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution).
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        for (auto& a : m_State)
        {
            if (baseScale > 0.0f) a.SetBaseScale(baseScale);
            if (resolution > 0u) a.Resize(device, resolution);
        }
        m_Resolution = m_State[0].GetResolution();
    }

    // The dynamic-wave cascade the surface samples this frame (the just-written
    // target) + its sampler and snapped layout.
    ::GameEngine::Rendering::TextureHandle GetTexture() const;
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const;

    // RG imports of the ping-pong pair at their resting state (call after
    // BeginFrame flipped the index). Write = this frame's target (the texture
    // GetTexture returns), Prev = last frame's history the dispatch samples.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportWriteRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportPrevRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportWriteRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 writeIndex) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportPrevRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 writeIndex) const;

    // Begin a new sim frame (call once per frame at schedule time, before the
    // forward contributor binds the texture): advance the ping-pong index so the
    // frame's write target is fixed, then snap only that target to the camera. The
    // read target keeps the layout it was written with so history can be sampled by
    // world position. RecordDispatch (which runs at graph-execution time) must not
    // mutate the index — only this does.
    void BeginFrame(float cameraX, float cameraZ);
    uint32 BeginSubstep(float cameraX, float cameraZ);

    // Per-frame impulse list + the sim scalars + dt. Replaces the prior frame's
    // impulse set (impulses are additive each frame they are present).
    void SetFrameInputs(const OceanWaveImpulseGPU* impulses, uint32 count, float waveSpeed,
                        float damping, float deltaTime);
    void ConfigureSimulation(float32 frequency, uint32 maximumSubsteps,
                             float32 damping, float32 courantNumber, float32 gravity,
                             float32 shallowAttenuation, float32 horizontalDisplacement,
                             float32 displacementClamp, uint32 minimumCascade,
                             uint32 maximumCascade);
    void ClearSimulationConfiguration();
    void SetShallowWaterInputs(::GameEngine::Rendering::TextureHandle seabedDepth,
                               ::GameEngine::Rendering::SamplerHandle seabedSampler,
                               bool available);
    uint32 GetPendingSubstepCount() const { return m_PendingSubsteps; }
    void FinishSubsteps();
    void InvalidateHistory()
    {
        m_ResetHistory = true;
        m_TimeAccumulator = 0.0f;
    }

    // True once the sim has advanced impulse-free for enough SIMULATED time that
    // the damped field is imperceptibly flat. Wall/render frames do not count:
    // TimeScale=0 and fixed-time capture must preserve a frozen disturbance.
    bool IsQuiescent() const { return m_SimulatedSecondsSinceImpulse >= kQuiescentSeconds; }

    // Finalize this frame's GPU params (cascade layouts follow the ping-pong
    // index BeginFrame fixed). Called at DECLARE time; the caller writes the
    // result into a render-graph upload-ring allocation and hands the
    // {buffer, offset} to RecordDispatch.
    void FillParams(OceanDynWavesParamsGPU& out);
    void FillParams(OceanDynWavesParamsGPU& out, uint32 writeIndex,
                    uint32 substepIndex) const;

    // Records the wave-equation + injection dispatch with barriers, leaving the
    // written cascade sampleable by the world pass. Advances nothing (BeginFrame
    // already flipped the ping-pong index). Params come from the frame's upload
    // ring (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset, uint32 writeIndex);

private:
    // Match the foam cascade so the layouts coincide (see OceanFoamSim).
    static constexpr uint32 kDynResolution = 256;
    static constexpr uint32 kDynLodCount = 4;
    static constexpr float kDynBaseScale = 64.0f;
    // At the default 0.2/s velocity damping this leaves less than 0.25% of the
    // original velocity energy before the stale camera-snapped cascade is hidden.
    static constexpr float32 kQuiescentSeconds = 30.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kDynResolution;
    uint32 m_LodCount = kDynLodCount;

    // Ping-pong cascades. m_WriteIndex is written this frame; the other is read
    // with its previous snapped layout.
    OceanCascadeArray m_State[2];
    uint32 m_WriteIndex = 0;

    // Simulated seconds since the last impulse (advanced by SetFrameInputs' dt,
    // reset by a non-empty list). Starts quiescent: a scene with no interaction
    // never dispatches.
    float32 m_SimulatedSecondsSinceImpulse = kQuiescentSeconds;
    bool m_UseFixedStep = false;
    float32 m_SimulationFrequency = 60.0f;
    uint32 m_MaximumSubsteps = 4u;
    float32 m_TimeAccumulator = 0.0f;
    float32 m_SubstepDelta = 0.0f;
    uint32 m_PendingSubsteps = 0u;
    uint32 m_PendingImpulseCount = 0u;
    bool m_ResetHistory = false;

    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for sim + surface read

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanDynWavesParamsGPU m_Params;
    ::GameEngine::Rendering::TextureHandle m_SeabedDepth;
    ::GameEngine::Rendering::SamplerHandle m_SeabedSampler;
};

} // namespace GameEngine::Ocean
