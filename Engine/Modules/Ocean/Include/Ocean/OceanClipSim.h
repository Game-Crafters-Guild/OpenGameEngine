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

// Clip-surface bake. Each frame it writes, per texel of a camera-snapped R8_UNORM
// LOD cascade, the surface clip state (1 = clipped/hole, 0 = solid water)
// composited from tagged rectangular and polygon clip sources over a default
// clip state. Like the flow bake this is a stateless closed-form evaluation (no
// ping-pong, no temporal accumulation): a pure function of the snapped layout +
// the clip-source list. RecordDispatch() records the compute with manual barriers,
// mirroring OceanFlowSim. A failed init leaves the surface unclipped (ClipAvailable
// stays 0).
//
// The cascade has the foam cascade's resolution and base scale and one more
// layer. Its four camera layers snap to the camera every frame the bake is
// considered; while any clip source exists the fifth layer is anchored in the
// world over the union of every source's bounds (rectangles, polygons, spline
// ribbons), so the clip covers all authored water wherever the camera is and the
// finer layers refine the shoreline near it. Beyond the camera layers the clip
// resolves at that span / 0.88 / resolution per texel, and the sampler switches
// from the last camera layer to the anchored one without blending (a blend would
// fade thin water over the camera layers' outer band). The foam cascade snaps only on the frames its simulation
// steps, so the surface samples the clip with its own layout
// (OceanSampledCascade::Clip).
class OceanClipSim
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
    // XZ bounds of the ribbons that raster into this cascade (OceanSplineRaster::
    // ClipBoundsXZ); part of the area the anchored coarsest layer covers.
    void SetSplineBounds(bool hasBounds, float minX, float minZ, float maxX, float maxZ)
    {
        m_HasSplineBounds = hasBounds;
        m_SplineBounds[0] = minX;
        m_SplineBounds[1] = minZ;
        m_SplineBounds[2] = maxX;
        m_SplineBounds[3] = maxZ;
    }

    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_Clip.RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution).
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        bool changed = false;
        if (baseScale > 0.0f && baseScale != m_Clip.GetBaseScale())
        {
            m_Clip.SetBaseScale(baseScale);
            changed = true;
        }
        if (resolution > 0u && resolution != m_Clip.GetResolution())
            changed = m_Clip.Resize(device, resolution) || changed;
        m_Resolution = m_Clip.GetResolution();
        if (changed)
            m_DispatchDirty = true;
    }

    ::GameEngine::Rendering::TextureHandle GetClipTexture() const { return m_Clip.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetClipSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const { return m_Clip.GetLayout(); }
    // RG import of the clip cascade at its resting state (see OceanCascadeArray::ImportRG).
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

    // True only when at least one rectangular or polygon clip source was supplied
    // this frame (so the surface stays solid with no source present).
    bool HasSources() const
    {
        return m_HasSpline || m_Params.SourceCount > 0 || m_Params.PolygonCount > 0;
    }

    // The clip value used outside every source (mirrored into OceanParamsGPU so the
    // surface gate matches the bake). Authored by OceanSurface.DefaultClippingState.
    float GetDefaultClippingState() const { return m_Params.DefaultClippingState; }
    void SetDefaultClippingState(float state)
    {
        if (m_Params.DefaultClippingState != state)
        {
            m_Params.DefaultClippingState = state;
            m_DispatchDirty = true;
        }
    }

    // Anchor the coarsest layer over every clip source this frame and snap the
    // finer layers to the camera (call once per frame at schedule time, before the
    // contributor binds the clip texture). Returns true when the bake must run.
    bool BeginFrame(float cameraX, float cameraZ);
    bool NeedsDispatch() const { return m_DispatchDirty; }

    // Per-frame clip-source list. Replaces the prior frame's set.
    void SetSources(const OceanClipSourceGPU* sources, uint32 count);
    void SetPolygonSources(const OceanClipPolygonGPU* polygons, uint32 count);

    // Finalize this frame's GPU params (the layout BeginFrame snapped). Called at
    // DECLARE time; the caller writes the result into a render-graph upload-ring
    // allocation and hands the {buffer, offset} to RecordDispatch.
    void FillParams(OceanClipParamsGPU& out);

    // Records the clip bake dispatch with barriers, leaving the cascade sampleable
    // by the world pass. Params come from the frame's upload ring (FillParams
    // above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

private:
    // The foam cascade's resolution, base scale and four camera layers (see
    // OceanFoamSim), plus the anchored layer over all authored water.
    static constexpr uint32 kClipResolution = 256;
    static constexpr uint32 kClipLodCount = 5;
    static constexpr float kClipBaseScale = 64.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kClipResolution;
    uint32 m_LodCount = kClipLodCount;

    OceanCascadeArray m_Clip;
    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for the surface read

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanClipParamsGPU m_Params;
    bool m_HasSpline = false;
    bool m_HasSplineBounds = false;
    float m_SplineBounds[4] = {};
    bool m_DispatchDirty = true;
};

} // namespace GameEngine::Ocean
