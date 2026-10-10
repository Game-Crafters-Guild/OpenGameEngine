#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanDepthCacheAsset.h"
#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
class CommandList;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

struct OceanSavedDepthCacheSource
{
    std::filesystem::path Path;
    uint32 SourceRevision = 0u;
    uint32 BakedRevision = 0u;
    uint32 CacheRevision = 0u;
    bool UseWhenStale = true;
};

// Sea-floor depth cascade. Each frame it bakes, per texel of a camera-snapped
// R16F LOD cascade, the distance from the calm sea level down to the seabed.
// It blends saved depth cache textures, an optional GPU raster water-depth
// capture, authored analytic seabeds, and per-frame dynamic depth contributors.
// With no depth sources present it degrades to "deep water everywhere".
//
// The cascade shares the foam cascade's resolution / LOD count / base scale, so
// the foam sim reads it texel-for-texel on the frames both snap to the same camera.
// The surface samples it with its own layout (OceanSampledCascade::SeabedDepth)
// and uses the depth for the shallow-water colour term; the foam sim and the
// surface use it for shoreline foam.
//
// Single persistent cascade (no ping-pong: the bake is a pure function of the
// snapped layout + seabed list, with no temporal accumulation). RecordDispatch()
// records the compute with manual barriers, mirroring OceanFoamSim. A failed init
// leaves the depth terms inert (SeabedDepthAvailable stays 0).
class OceanSeabedDepth
{
public:
    // Spline ribbons raster into this cascade after its bake. The bake reruns
    // only when a ribbon starts or stops covering the field or its triangles change.
    void SetSplineActive(bool active, bool trianglesChanged)
    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        if (m_HasSpline != active || (active && trianglesChanged))
            m_DispatchDirty = true;
        m_HasSpline = active;
    }

    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_Depth.RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution).
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        bool changed = false;
        if (baseScale > 0.0f && baseScale != m_Depth.GetBaseScale())
        {
            m_Depth.SetBaseScale(baseScale);
            changed = true;
        }
        if (resolution > 0u && resolution != m_Depth.GetResolution())
            changed = m_Depth.Resize(device, resolution) || changed;
        m_Resolution = m_Depth.GetResolution();
        if (changed)
        {
            std::lock_guard<std::mutex> lock(m_ParamsMutex);
            m_DispatchDirty = true;
        }
    }

    ::GameEngine::Rendering::TextureHandle GetDepthTexture() const { return m_Depth.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetDepthSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const { return m_Depth.GetLayout(); }
    // RG import of the depth cascade at its resting state (see OceanCascadeArray::ImportRG).
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

    // True only when at least one depth source was supplied this frame (so the
    // surface gate / foam shoreline term stays off with no depth content present).
    bool HasSeabeds() const;

    // Snap the cascade to the camera (call once per frame at schedule time, before
    // the contributor binds the depth texture). Kept aligned with the foam cascade
    // by using the same base scale + resolution.
    bool BeginFrame(float cameraX, float cameraZ);
    bool NeedsDispatch() const;

    // Per-frame seabed list + dynamic analytic depth contributors + the calm sea
    // level. Replaces the prior frame's set. depthBandSaturation is the depth at
    // which every shallow term saturates; contributor feathers ramp from it.
    void SetSeabeds(const OceanSeabedGPU* seabeds, uint32 count,
                    const OceanDepthContributorGPU* contributors, uint32 contributorCount,
                    float seaLevel, float depthBandSaturation);

    // CPU mirror of the depth bake. Returns the shallowest positive water depth
    // at world XZ for saved caches, authored seabeds, and dynamic contributors.
    // GPU raster captures are intentionally GPU-only unless also supplied as a
    // saved cache or CPU-readable source.
    bool SampleDepth(float worldX, float worldZ, float& outDepth) const;

    bool SetSavedDepthCache(const OceanDepthCacheAsset& cache);
    bool SetSavedDepthCaches(const OceanDepthCacheAsset* caches, uint32 count);
    bool LoadSavedDepthCaches(const OceanSavedDepthCacheSource* sources, uint32 count);
    bool LoadSavedDepthCacheFromFile(const std::filesystem::path& path);
    bool LoadSavedDepthCachesFromFiles(const std::filesystem::path* paths, uint32 count);
    void ClearSavedDepthCache();

    // External dynamic water-depth capture. The texture must be sampleable and
    // store depth in meters from calm sea level down to the nearest seabed.
    // Origin/size describe the covered world XZ rectangle.
    void SetRasterDepthCapture(::GameEngine::Rendering::TextureHandle texture,
                               ::GameEngine::Rendering::SamplerHandle sampler,
                               float originX, float originZ, float sizeX, float sizeZ,
                               float deepWaterDepth);
    void ClearRasterDepthCapture();

    // Finalize this frame's GPU params (the cascade layout BeginFrame snapped).
    // Called at DECLARE time; the caller writes the result into a render-graph
    // upload-ring allocation and hands the {buffer, offset} to RecordDispatch.
    void FillParams(OceanSeabedDepthParamsGPU& out);

    // Records the depth bake dispatch with barriers, leaving the cascade
    // sampleable by the world pass and the foam compute. Params come from the
    // frame's upload ring (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer,
                        uint64 paramsOffset);

private:
    // Match the foam cascade so the two layouts coincide (see OceanFoamSim).
    static constexpr uint32 kDepthResolution = 256;
    static constexpr uint32 kDepthLodCount = 4;
    static constexpr float kDepthBaseScale = 64.0f;

    bool UploadSavedDepthCacheTextureLocked(const OceanDepthCacheAsset& cache);
    bool UploadSavedDepthCacheAtlasLocked(const OceanDepthCacheAsset* caches, uint32 count);
    void ApplySavedDepthCacheParamsLocked();
    void ClearSavedDepthCacheLocked();

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kDepthResolution;
    uint32 m_LodCount = kDepthLodCount;

    OceanCascadeArray m_Depth;
    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for surface + foam read
    ::GameEngine::Rendering::TextureHandle m_DummySavedDepthTexture;
    ::GameEngine::Rendering::TextureHandle m_SavedDepthTexture;
    ::GameEngine::Rendering::SamplerHandle m_SavedDepthSampler;
    uint32 m_SavedDepthTextureWidth = 0;
    uint32 m_SavedDepthTextureHeight = 0;
    ::GameEngine::Rendering::TextureHandle m_RasterDepthCaptureTexture;
    ::GameEngine::Rendering::SamplerHandle m_RasterDepthCaptureSampler;

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanSeabedDepthParamsGPU m_Params;
    OceanDepthCacheAsset m_SavedDepthCache;
    std::vector<OceanDepthCacheAsset> m_SavedDepthCaches;
    std::vector<std::filesystem::path> m_SavedDepthCachePaths;
    std::vector<std::filesystem::path> m_FailedSavedDepthCachePaths;
    std::vector<std::filesystem::file_time_type> m_SavedDepthCacheWriteTimes;
    std::vector<std::filesystem::file_time_type> m_FailedSavedDepthCacheWriteTimes;
    std::vector<uint32> m_SavedDepthCacheSourceRevisions;
    std::vector<uint32> m_SavedDepthCacheBakedRevisions;
    std::vector<uint32> m_SavedDepthCacheRevisions;
    std::vector<uint32> m_FailedSavedDepthCacheSourceRevisions;
    std::vector<uint32> m_FailedSavedDepthCacheBakedRevisions;
    std::vector<uint32> m_FailedSavedDepthCacheRevisions;
    bool m_HasSavedDepthCache = false;
    bool m_HasRasterDepthCapture = false;
    bool m_HasSpline = false;
    bool m_DispatchDirty = true;
    mutable std::mutex m_ParamsMutex;
};

} // namespace GameEngine::Ocean
