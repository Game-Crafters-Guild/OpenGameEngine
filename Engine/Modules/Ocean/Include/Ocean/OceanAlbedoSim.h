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

// Albedo bake. Each frame it writes, per texel of a camera-snapped RGBA8_UNORM LOD
// cascade, the surface paint (rgb = colour, a = coverage) composited from the
// tagged albedo sources (rectangular footprints). Like the flow bake this is a
// stateless closed-form evaluation (no ping-pong, no temporal accumulation): a
// pure function of the snapped layout + the albedo-source list. RecordDispatch()
// records the compute with manual barriers, mirroring OceanFlowSim. A failed init
// leaves the surface unpainted (AlbedoAvailable stays 0).
//
// The cascade shares the foam cascade's resolution / LOD count / base scale; it
// snaps on its own schedule, so the surface samples it with its own layout
// (OceanSampledCascade::Albedo).
class OceanAlbedoSim
{
public:
    // Spline ribbons raster into this cascade after its bake. The bake reruns
    // only when a ribbon starts or stops covering the field or its triangles change.
    void SetSplineActive(bool active, bool trianglesChanged)
    {
        if (m_HasSpline != active || (active && trianglesChanged))
            m_DispatchDirty = true;
        m_HasSpline = active;
    }

    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_Albedo.RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution).
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        bool changed = false;
        if (baseScale > 0.0f && baseScale != m_Albedo.GetBaseScale())
        {
            m_Albedo.SetBaseScale(baseScale);
            changed = true;
        }
        if (resolution > 0u && resolution != m_Albedo.GetResolution())
            changed = m_Albedo.Resize(device, resolution) || changed;
        m_Resolution = m_Albedo.GetResolution();
        if (changed)
            m_DispatchDirty = true;
    }

    ::GameEngine::Rendering::TextureHandle GetAlbedoTexture() const { return m_Albedo.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetAlbedoSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const { return m_Albedo.GetLayout(); }
    // RG import of the albedo cascade at its resting state (see OceanCascadeArray::ImportRG).
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

    // True only when at least one albedo source was supplied this frame (so the
    // surface keeps its base water colour with no source present).
    bool HasSources() const
    {
        return m_HasSpline || m_Params.SourceCount > 0;
    }

    // Snap the cascade to the camera (call once per frame at schedule time, before
    // the contributor binds the albedo texture). Kept aligned with the foam cascade
    // by using the same base scale + resolution.
    bool BeginFrame(float cameraX, float cameraZ);
    bool NeedsDispatch() const { return m_DispatchDirty; }

    // Per-frame albedo-source list. Replaces the prior frame's set.
    void SetSources(const OceanAlbedoSourceGPU* sources, uint32 count);

    // Finalize this frame's GPU params (the cascade layout BeginFrame snapped).
    // Called at DECLARE time; the caller writes the result into a render-graph
    // upload-ring allocation and hands the {buffer, offset} to RecordDispatch.
    void FillParams(OceanAlbedoParamsGPU& out);

    // Records the albedo bake dispatch with barriers, leaving the cascade
    // sampleable by the world pass. Params come from the frame's upload ring
    // (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

private:
    // Match the foam cascade so the two layouts coincide (see OceanFoamSim).
    static constexpr uint32 kAlbedoResolution = 256;
    static constexpr uint32 kAlbedoLodCount = 4;
    static constexpr float kAlbedoBaseScale = 64.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kAlbedoResolution;
    uint32 m_LodCount = kAlbedoLodCount;

    OceanCascadeArray m_Albedo;
    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for the surface read

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanAlbedoParamsGPU m_Params;
    bool m_HasSpline = false;
    bool m_DispatchDirty = true;
};

} // namespace GameEngine::Ocean
