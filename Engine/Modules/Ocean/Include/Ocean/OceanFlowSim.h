#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanCpuTexture.h"
#include "Ocean/OceanQuery.h"
#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
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

// Flow-field bake. Each frame it writes, per texel of a camera-snapped RG16F LOD
// cascade, the horizontal current (world XZ velocity, meters/second) accumulated
// from tagged rectangular and polygon flow sources.
// Like the seabed depth bake this is a stateless closed-form evaluation (no
// ping-pong, no temporal accumulation): a pure function of the snapped layout +
// the flow-source list. RecordDispatch() records the compute with manual
// barriers, mirroring OceanSeabedDepth. A failed init leaves the flow inert
// (FlowAvailable stays 0).
//
// The cascade shares the foam cascade's resolution / LOD count / base scale, so
// the foam sim reads it texel-for-texel on the frames both snap to the same camera.
// The surface (which scrolls its detail UVs by it) samples it with its own layout
// (OceanSampledCascade::Flow).
class OceanFlowSim
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
        m_Flow.RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution).
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        bool changed = false;
        if (baseScale > 0.0f && baseScale != m_Flow.GetBaseScale())
        {
            m_Flow.SetBaseScale(baseScale);
            changed = true;
        }
        if (resolution > 0u && resolution != m_Flow.GetResolution())
            changed = m_Flow.Resize(device, resolution) || changed;
        m_Resolution = m_Flow.GetResolution();
        if (changed)
            m_DispatchDirty = true;
    }

    ::GameEngine::Rendering::TextureHandle GetFlowTexture() const { return m_Flow.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetFlowSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const { return m_Flow.GetLayout(); }
    // RG import of the flow cascade at its resting state (see OceanCascadeArray::ImportRG).
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

    // True only when at least one flow source was supplied this frame (so the
    // foam advection / detail-UV scroll stays off with no source present).
    bool HasSources() const
    {
        return m_HasSpline || m_Params.SourceCount > 0 || m_Params.PolygonCount > 0 ||
               m_Params.FlowMapCount > 0;
    }

    // Snap the cascade to the camera (call once per frame at schedule time, before
    // the contributor binds the flow texture). Kept aligned with the foam cascade
    // by using the same base scale + resolution.
    bool BeginFrame(float cameraX, float cameraZ);
    bool NeedsDispatch() const { return m_DispatchDirty; }

    // Per-frame flow-source list. Replaces the prior frame's set.
    void SetSources(const OceanFlowSourceGPU* sources, uint32 count);
    void SetPolygonSources(const OceanFlowPolygonGPU* polygons, uint32 count);
    void SetFlowMapSources(const OceanFlowMapSourceGPU* sources,
                           const ::GameEngine::Rendering::TextureHandle* textures,
                           uint32 count,
                           const OceanCpuTextureRG* cpuTextures = nullptr);

    // CPU mirror of the closed-form flow bake. Samples analytic rectangle/polygon
    // flow and texture flow-map pixels with the same closed-form source order as
    // the GPU cascade.
    OceanCurrentSample SampleFlow(float worldX, float worldZ) const;

    // Finalize this frame's GPU params (the cascade layout BeginFrame snapped).
    // Called at DECLARE time; the caller writes the result into a render-graph
    // upload-ring allocation and hands the {buffer, offset} to RecordDispatch.
    void FillParams(OceanFlowParamsGPU& out);

    // Records the flow bake dispatch with barriers, leaving the cascade
    // sampleable by the world pass and the foam compute. Params come from the
    // frame's upload ring (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

private:
    // Match the foam cascade so the two layouts coincide (see OceanFoamSim).
    static constexpr uint32 kFlowResolution = 256;
    static constexpr uint32 kFlowLodCount = 4;
    static constexpr float kFlowBaseScale = 64.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kFlowResolution;
    uint32 m_LodCount = kFlowLodCount;

    OceanCascadeArray m_Flow;
    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for surface + foam read
    ::GameEngine::Rendering::TextureHandle m_DummyFlowMapTexture;
    std::array<::GameEngine::Rendering::TextureHandle, kMaxOceanFlowMapSources> m_FlowMapTextures{};
    std::array<OceanCpuTextureRG, kMaxOceanFlowMapSources> m_FlowMapCpuTextures{};
    bool m_HasSpline = false;
    bool m_DispatchDirty = true;

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanFlowParamsGPU m_Params;
};

} // namespace GameEngine::Ocean
