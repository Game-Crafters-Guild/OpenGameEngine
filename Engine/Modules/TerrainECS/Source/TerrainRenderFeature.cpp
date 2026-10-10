#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainAtlas.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Core/Application.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace GameEngine::TerrainECS
{
namespace
{
// Extraction frames a texture may sit uninitialized before the missing-"TerrainUpload"-pass
// diagnostic fires. A pipeline that declares the pass flushes on its first declared frame, so
// this only has to outlast startup ordering (extraction runs ahead of the first graph declare).
constexpr uint32 kUploadPassAbsentFrameLimit = 8;
} // namespace

TerrainRenderFeature::~TerrainRenderFeature()
{
    if (m_Device)
        m_GrassWindVolumes.Destroy(*m_Device);
    // Flush any remaining deferred buffer destroys.
    for (auto& dd : m_DeferredDestroys)
    {
        if (m_Device && dd.Buffer.IsValid())
            m_Device->DestroyBuffer(dd.Buffer);
    }
    m_DeferredDestroys.clear();

    // Flush any remaining deferred texture destroys.
    for (auto& dd : m_DeferredTextureDestroys)
    {
        if (m_Device && dd.Texture.IsValid())
            m_Device->DestroyTexture(dd.Texture);
    }
    m_DeferredTextureDestroys.clear();

    // Release unflushed pending upload staging buffers.
    for (auto& upload : m_PendingUploads)
    {
        if (m_Device && upload.StagingBuffer.IsValid())
            m_Device->DestroyBuffer(upload.StagingBuffer);
    }
    m_PendingUploads.clear();

    // Destroy any remaining heightmap textures still held by the feature.
    for (auto& entry : m_Heightmaps)
    {
        if (m_Device && entry.Texture.IsValid())
            m_Device->DestroyTexture(entry.Texture);
    }
    m_Heightmaps.clear();

    // Destroy any remaining splatmap textures still held by the feature.
    for (auto& entry : m_Splatmaps)
    {
        if (m_Device && entry.Texture.IsValid())
            m_Device->DestroyTexture(entry.Texture);
    }
    m_Splatmaps.clear();
    for (auto& entries : m_GrassFields)
        for (auto& entry : entries)
            if (m_Device && entry.Texture.IsValid()) m_Device->DestroyTexture(entry.Texture);
}

void TerrainRenderFeature::DeferBufferDestroy(Rendering::BufferHandle buf)
{
    if (!buf.IsValid())
        return;
    m_DeferredDestroys.push_back({buf, m_MonotonicFrame});
}

void TerrainRenderFeature::DeferTextureDestroy(Rendering::TextureHandle tex, size_t bytes)
{
    if (!tex.IsValid())
        return;
    m_TexturesAwaitingGpuInit.erase(tex.id);
    m_DeferredTextureDestroys.push_back({tex, m_MonotonicFrame, bytes});
}

void TerrainRenderFeature::MarkAwaitingGpuInit(Rendering::TextureHandle tex)
{
    if (!tex.IsValid())
        return;
    m_TexturesAwaitingGpuInit.insert(tex.id);
}

bool TerrainRenderFeature::HasQueuedGpuInit(Rendering::TextureHandle tex) const
{
    if (std::any_of(m_PendingClears.begin(), m_PendingClears.end(),
                    [&](const PendingTextureClear& pending) { return pending.Texture.id == tex.id; }))
        return true;
    // A band or a sub-rect only preserves what it does not touch, so only a whole-texture copy
    // is an initializer — the same distinction FlushPendingUploads releases the gate on.
    return std::any_of(m_PendingUploads.begin(), m_PendingUploads.end(),
                       [&](const PendingTextureUpload& upload)
                       { return upload.FullTexture && upload.Texture.id == tex.id; });
}

void TerrainRenderFeature::DeferTextureViewDestroy(Rendering::TextureViewHandle view)
{
    if (!view.IsValid())
        return;
    m_DeferredViewDestroys.push_back({view, m_MonotonicFrame});
}

void TerrainRenderFeature::CopyLastTerrainParamsBytes(std::vector<uint8>& out) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    out.assign(m_LastTerrainParamsBytes.begin(), m_LastTerrainParamsBytes.end());
}

void TerrainRenderFeature::ReleaseTerrainResources(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Retiring a texture changes what grass would sample as surely as writing to one.
    m_GrassContentEpoch.fetch_add(1, std::memory_order_release);

    ReleaseGrassFieldResourcesLocked(handle);

    if (handle.Index < m_Heightmaps.size())
    {
        auto& entry = m_Heightmaps[handle.Index];
        if (entry.Texture.IsValid())
        {
            DeferTextureDestroy(entry.Texture, entry.Bytes);
            entry.Texture = {};
            entry.Width = 0;
            entry.Height = 0;
            entry.BindlessIndex = 0;
        }
    }

    if (handle.Index < m_Splatmaps.size())
    {
        auto& entry = m_Splatmaps[handle.Index];
        if (entry.Texture.IsValid())
        {
            DeferTextureDestroy(entry.Texture, entry.Bytes);
            entry.Texture = {};
            entry.Width = 0;
            entry.Height = 0;
            entry.BindlessIndex = 0;
        }
    }

    if (handle.Index < m_Normalmaps.size())
    {
        auto& entry = m_Normalmaps[handle.Index];
        if (entry.Texture.IsValid())
        {
            DeferTextureDestroy(entry.Texture, entry.Bytes);
            entry.Texture = {};
            entry.Width = 0;
            entry.Height = 0;
            entry.BindlessIndex = 0;
        }
    }

    if (handle.Index < m_AtlasHeights.size())
    {
        auto& entry = m_AtlasHeights[handle.Index];
        if (entry.Texture.IsValid())
        {
            DeferTextureDestroy(entry.Texture, entry.Bytes);
            entry.Texture = {};
            entry.Width = 0;
            entry.Height = 0;
            entry.BindlessIndex = 0;
        }
    }

    if (handle.Index < m_AtlasCoarse.size())
    {
        auto& entry = m_AtlasCoarse[handle.Index];
        if (entry.Texture.IsValid())
        {
            DeferTextureDestroy(entry.Texture, entry.Bytes);
            entry.Texture = {};
            entry.Width = 0;
            entry.Height = 0;
            entry.BindlessIndex = 0;
        }
    }

    // Phase E atlas splat/normal surface sources + their coarse fields share the same lifetime.
    for (std::vector<TextureEntry>* vec :
         {&m_AtlasSplats, &m_AtlasNormals, &m_AtlasSplatCoarse, &m_AtlasNormalCoarse})
    {
        if (handle.Index >= vec->size())
            continue;
        auto& entry = (*vec)[handle.Index];
        if (entry.Texture.IsValid())
        {
            DeferTextureDestroy(entry.Texture, entry.Bytes);
            entry.Texture = {};
            entry.Width = 0;
            entry.Height = 0;
            entry.BindlessIndex = 0;
        }
    }

    // Drop any accumulated dirty rect so a reused slot starts clean (a stale rect would
    // otherwise flag the new terrain's bisectors on its first frame).
    if (handle.Index < m_UnifiedHeightDirty.size())
        m_UnifiedHeightDirty[handle.Index] = UnifiedDirtyRect{};

    // Retire any pending GPU height-bake work targeting this slot. The atlas textures the
    // bakes dispatch into and the readbacks copy out of are retired above, and the slot index
    // is about to be recycled (DestroyTiledTerrain). A bake left queued would dispatch against
    // whatever terrain next reuses the index — FlushPendingBakes matches on Index only, so it
    // cannot tell them apart — writing the old slot/rect into a possibly-smaller new atlas
    // (out-of-bounds imageStore). A readback left queued would copy from the retired image and,
    // because DrainReadyHeightReadbacks matches on generation, would never drain — leaking its
    // buffer across every re-provision. Drop the bakes outright; defer-destroy the readback
    // buffers (their CopyTextureToBuffer may still be in flight).
    std::erase_if(m_PendingBakes,
                  [&](const PendingBake& b) { return b.Handle.Index == handle.Index; });
    std::erase_if(m_PendingReadbacks,
                  [&](const PendingReadback& rb)
                  {
                      if (rb.Handle.Index != handle.Index)
                          return false;
                      DeferBufferDestroy(rb.Buffer);
                      return true;
                  });

    // Arm the re-provision VRAM staging barrier: the retired world-sized unified set stays
    // GPU-allocated through the kMaxFrames frames-in-flight quarantine, so hold off the NEW set's
    // allocation (EnsureUnifiedTiledTextures) until the quarantine drains. That caps the transient
    // peak at max(old, new) instead of old + new. std::max extends the barrier if a second
    // re-provision retires while one is already draining. Consulted only by the non-atlas unified
    // path; the atlas path's window-sized set is small and never gated.
    m_UnifiedReprovisionDrainFrame = std::max(m_UnifiedReprovisionDrainFrame, m_MonotonicFrame + kMaxFrames);
    Logger::Log::Info("TerrainRenderFeature: re-provision staging armed at frame {} — new unified set "
                      "withheld until frame {} (retiring slot {} drains)",
                      m_MonotonicFrame, m_UnifiedReprovisionDrainFrame, handle.Index);

    // Signal the CBT renderer that a terrain texture set was retired, so it can drop the retired
    // heightmap from its own descriptor ring (a separate path from the bindless slots this frees)
    // before the frames-in-flight quarantine above frees the image. Bumped after the retires so a
    // consumer that observes the new value is guaranteed to see the cleared entries.
    m_TerrainTextureRetireGeneration.fetch_add(1, std::memory_order_release);
}

void TerrainRenderFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    if (!m_Initialized)
        return; // never brought up on the old device: nothing of ours is dead

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Device = device;

    for (std::vector<TextureEntry>* entries :
         {&m_Heightmaps, &m_Splatmaps, &m_Normalmaps, &m_AtlasHeights, &m_AtlasCoarse, &m_AtlasSplats,
          &m_AtlasNormals, &m_AtlasSplatCoarse, &m_AtlasNormalCoarse, &m_GrassFields[0], &m_GrassFields[1],
          &m_GrassFields[2]})
        for (TextureEntry& entry : *entries)
            entry = TextureEntry{};
    m_UnifiedHeightDirty.clear();
    m_CompatLayerTextures = {};
    // The terrains stay described; only their device handles go (the extraction republishes
    // them with live ones once it has uploaded again).
    for (TerrainInstanceInfo& terrain : m_ActiveTerrains)
    {
        terrain.HeightmapTexture = {};
        terrain.HeightmapBindlessIndex = 0;
        terrain.SplatmapTexture = {};
        terrain.SplatmapBindlessIndex = 0;
        terrain.NormalmapBindlessIndex = 0;
        terrain.AtlasHeightTexture = {};
        terrain.AtlasCoarseTexture = {};
        terrain.AtlasSplatBindlessIndex = 0;
        terrain.AtlasNormalBindlessIndex = 0;
        terrain.AtlasSplatCoarseBindlessIndex = 0;
        terrain.AtlasNormalCoarseBindlessIndex = 0;
        terrain.AtlasHeightBindlessIndex = 0;
        terrain.AtlasCoarseBindlessIndex = 0;
        terrain.AtlasGrassBindlessIndex = 0;
        terrain.AtlasGrassCoarseBindlessIndex = 0;
        terrain.GrassControlBindlessIndex = 0;
    }

    // Work queued against the dead resources, and the quarantine of resources already freed.
    m_DeferredDestroys.clear();
    m_DeferredTextureDestroys.clear();
    m_DeferredViewDestroys.clear();
    m_PendingUploads.clear();
    m_PendingClears.clear();
    m_TexturesAwaitingGpuInit.clear();
    m_PendingBakes.clear();
    m_PendingReadbacks.clear();
    m_UnifiedTextureBytesLive = 0;
    m_UnifiedReprovisionDrainFrame = 0;

    for (uint32 fi = 0; fi < kMaxFrames; ++fi)
    {
        m_TerrainParamsSSBO[fi] = {};
        m_TerrainParamsCapacity[fi] = 0;
        m_TerrainParamsCount[fi] = 0;
        m_TerrainMaterialTableSSBO[fi] = {};
        m_TerrainMaterialCapacity[fi] = 0;
        m_TerrainMaterialCount[fi] = 0;
    }
    m_GrassWindVolumes.ForgetAfterDeviceRebuild();
    m_GpuBakePipeline = {};
    m_GpuNormalPipeline = {};
    m_GpuSplatPipeline = {};
    m_GpuBakePipelineAttempted = false;

    // What Initialize creates: the samplers and the always-bound material tables.
    CreateSamplers(device);
    for (uint32 fi = 0; fi < kMaxFrames; ++fi)
        EnsureMaterialTableBuffer(fi, Terrain::kTerrainLayerRoleCount);

    // The grass and the CBT renderer key caches on these; the extraction re-uploads on the last.
    m_GrassContentEpoch.fetch_add(1, std::memory_order_release);
    m_TerrainTextureRetireGeneration.fetch_add(1, std::memory_order_release);
    m_DeviceResourceEpoch.fetch_add(1, std::memory_order_release);
}

void TerrainRenderFeature::FlushDeferredDestroys(Engine::Renderer::RenderServices& rs)
{
    if (!m_Device)
        return;

    ++m_MonotonicFrame;

    // Keep resources alive for at least kMaxFrames monotonic ticks.
    // This is safe because m_MonotonicFrame increments once per frame
    // and kMaxFrames >= the device's frames-in-flight count.
    m_DeferredDestroys.erase(
        std::remove_if(m_DeferredDestroys.begin(), m_DeferredDestroys.end(),
            [&](DeferredBufferDestroy& dd) {
                if (m_MonotonicFrame - dd.FrameRetired >= kMaxFrames)
                {
                    m_Device->DestroyBuffer(dd.Buffer);
                    return true;
                }
                return false;
            }),
        m_DeferredDestroys.end());

    m_DeferredTextureDestroys.erase(
        std::remove_if(m_DeferredTextureDestroys.begin(), m_DeferredTextureDestroys.end(),
            [&](DeferredTextureDestroy& dd) {
                if (m_MonotonicFrame - dd.FrameRetired >= kMaxFrames)
                {
                    // Free the global bindless slot + its view FIRST (no-op for a texture
                    // that was never registered), then the image. Both happen only now,
                    // after the >= kMaxFrames quarantine, so no in-flight frame samples the
                    // slot or the image. Skipping InvalidateBindless leaked the slot and left
                    // its descriptor pointing at the image this line frees — a dangling
                    // bindless descriptor that a reissued slot (or a robustness-off driver)
                    // faults on: the resolution-change teardown's device-lost path.
                    rs.Textures().InvalidateBindless(dd.Texture);
                    m_Device->DestroyTexture(dd.Texture);
                    if (dd.Bytes != 0)
                    {
                        m_UnifiedTextureBytesLive -= dd.Bytes;
                        Logger::Log::Info("TerrainRenderFeature: unified/atlas VRAM freed {} MiB "
                                          "— live {} MiB (peak {} MiB)",
                                          dd.Bytes / (1024u * 1024u),
                                          m_UnifiedTextureBytesLive / (1024u * 1024u),
                                          m_UnifiedTextureBytesPeak / (1024u * 1024u));
                    }
                    return true;
                }
                return false;
            }),
        m_DeferredTextureDestroys.end());

    m_DeferredViewDestroys.erase(
        std::remove_if(m_DeferredViewDestroys.begin(), m_DeferredViewDestroys.end(),
            [&](DeferredViewDestroy& dd) {
                if (m_MonotonicFrame - dd.FrameRetired >= kMaxFrames)
                {
                    m_Device->DestroyTextureView(dd.View);
                    return true;
                }
                return false;
            }),
        m_DeferredViewDestroys.end());
}

bool TerrainRenderFeature::Initialize(Rendering::IDevice* device)
{
    if (m_Initialized)
        return true;

    if (!device)
        return false;

    m_Device = device;

    CreateSamplers(device);

    // Terrain params SSBOs are created lazily in UploadTerrainParamsArray
    // when we know how many terrains exist. Heightmap/splatmap/normalmap
    // textures are created lazily on their first upload.
    //
    // The material table is NOT lazy: both terrain surfaces declare it on set 2, and an
    // unprovided set-2 buffer resolves to a null descriptor — a device fault, not a blank
    // material. One zero-filled minimum table per ring element makes the handle valid from
    // here on, whatever order provisioning and the first extraction happen in.
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (uint32 fi = 0; fi < kMaxFrames; ++fi)
            EnsureMaterialTableBuffer(fi, Terrain::kTerrainLayerRoleCount);
    }

    m_Initialized = true;
    return true;
}

void TerrainRenderFeature::CreateSamplers(Rendering::IDevice* device)
{
    m_HeightmapSampler = device->CreateSampler(
        Rendering::SamplerDesc::MaterialLinearClamp("Terrain_Heightmap_Sampler"));
    // The compat profile's ONE shared sampler for the terrain/grass layer maps. It must REPEAT:
    // those maps tile from a world-space UV whose magnitude is far outside [0,1], so a clamp
    // sampler collapses every tap onto one edge texel and the surface renders a single flat
    // colour. The six data maps share it too and clamp their UV explicitly at the tap, which is
    // what lets one sampler serve both and stay under WebGPU's per-stage sampler cap.
    m_LayerSampler = device->CreateSampler(
        Rendering::SamplerDesc::MaterialLinearRepeat("Terrain_Layer_Sampler"));
}

void TerrainRenderFeature::UploadHeightmap(TerrainHandle handle,
                                            const float32* samples, uint32 width, uint32 height)
{
    UploadHeightmap(handle, samples, width, height, UploadBand{});
}

void TerrainRenderFeature::UploadHeightmap(TerrainHandle handle,
                                            const float32* samples, uint32 width, uint32 height,
                                            const UploadBand& band)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (!m_Device || !samples)
        return;

    // Grow heightmap entries if needed
    if (handle.Index >= m_Heightmaps.size())
    {
        m_Heightmaps.resize(handle.Index + 1);
    }

    auto& entry = m_Heightmaps[handle.Index];

    // Recreate texture if size changed (defer old texture destruction for GPU safety)
    bool recreated = false;
    if (entry.Width != width || entry.Height != height)
    {
        if (entry.Texture.IsValid())
            DeferTextureDestroy(entry.Texture, entry.Bytes);

        Rendering::TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = static_cast<uint32>(Rendering::TextureFormat::R32_FLOAT);
        // TransferSrc lets the render-source oracle (and future GPU capture) read
        // the baked heights back for comparison against CPU truth.
        desc.usage = static_cast<uint32>(Rendering::TextureUsage::ShaderResource |
                                         Rendering::TextureUsage::TransferDst |
                                         Rendering::TextureUsage::TransferSrc);
        desc.debugName = "Terrain_Heightmap";
        entry.Texture = m_Device->CreateTexture(desc);
        MarkAwaitingGpuInit(entry.Texture);
        entry.Width = width;
        entry.Height = height;
        entry.BindlessIndex = 0; // force re-registration
        recreated = true;
    }

    const size_t rowPitchBytes = static_cast<size_t>(width) * sizeof(float32);
    QueueBandUpload(entry.Texture, reinterpret_cast<const uint8*>(samples), width, height,
                    rowPitchBytes, band, recreated, "Terrain_Heightmap_Staging");
}

Rendering::TextureHandle TerrainRenderFeature::GetHeightmapTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_Heightmaps.size())
        return {};
    return m_Heightmaps[handle.Index].Texture;
}

// ---- Splatmap upload / bindless ----

void TerrainRenderFeature::UploadSplatmap(TerrainHandle handle,
                                           const uint8* rgba8Data, uint32 width, uint32 height)
{
    UploadSplatmap(handle, rgba8Data, width, height, UploadBand{});
}

void TerrainRenderFeature::UploadSplatmap(TerrainHandle handle,
                                           const uint8* rgba8Data, uint32 width, uint32 height,
                                           const UploadBand& band)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (!m_Device || !rgba8Data)
        return;

    if (handle.Index >= m_Splatmaps.size())
        m_Splatmaps.resize(handle.Index + 1);

    auto& entry = m_Splatmaps[handle.Index];

    bool recreated = false;
    if (entry.Width != width || entry.Height != height)
    {
        if (entry.Texture.IsValid())
            DeferTextureDestroy(entry.Texture, entry.Bytes);

        Rendering::TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = static_cast<uint32>(Rendering::TextureFormat::RGBA8_UNORM);
        desc.usage = static_cast<uint32>(Rendering::TextureUsage::ShaderResource |
                                         Rendering::TextureUsage::TransferDst);
        desc.debugName = "Terrain_Splatmap";
        entry.Texture = m_Device->CreateTexture(desc);
        MarkAwaitingGpuInit(entry.Texture);
        entry.Width = width;
        entry.Height = height;
        entry.BindlessIndex = 0; // force re-registration
        recreated = true;
    }

    const size_t rowPitchBytes = static_cast<size_t>(width) * 4;
    QueueBandUpload(entry.Texture, rgba8Data, width, height,
                    rowPitchBytes, band, recreated, "Terrain_Splatmap_Staging");
}

Rendering::TextureHandle TerrainRenderFeature::GetSplatmapTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_Splatmaps.size())
        return {};
    return m_Splatmaps[handle.Index].Texture;
}

uint32 TerrainRenderFeature::GetSplatmapBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_Splatmaps.size())
        return 0;
    return m_Splatmaps[handle.Index].BindlessIndex;
}

void TerrainRenderFeature::RegisterBindless(std::vector<TextureEntry>& entries,
                                            TerrainHandle handle,
                                            Engine::Renderer::RenderServices& rs)
{
    if (handle.Index >= entries.size())
        return;

    auto& entry = entries[handle.Index];
    if (!entry.Texture.IsValid() || entry.BindlessIndex != 0)
        return;

    if (!rs.Textures().IsBindlessEnabled())
        return;

    // A slot published for a texture whose zero-clear / first whole-texture copy has not been
    // recorded yet points shaders at an image still in VK_IMAGE_LAYOUT_UNDEFINED. Publish one
    // anyway when this frame's flush is certain to record it first — the "TerrainUpload" pass is
    // live and this texture's initializer is queued (frame-order argument on
    // m_UploadPassSeenExtractionFrame). Otherwise withhold and let the caller re-register next
    // frame: an unpublished slot is not a free transient on the non-atlas path, where the CBT
    // surface skips its splat tap (cbt_surface.glsl) and grass roots collapse to the terrain's
    // origin height (terrain_grass_place.comp).
    if (AwaitsGpuInit(entry.Texture) && !(UploadPassIsLive() && HasQueuedGpuInit(entry.Texture)))
        return;

    // Texture-only index; terrain shaders select LinearClamp shader-side (GE_TS_CLAMP).
    entry.BindlessIndex = rs.Textures().GetBindlessIndex(entry.Texture);
}

void TerrainRenderFeature::RegisterSplatmapBindless(TerrainHandle handle,
                                                     Engine::Renderer::RenderServices& rs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    RegisterBindless(m_Splatmaps, handle, rs);
}

void TerrainRenderFeature::UploadNormalmap(TerrainHandle handle,
                                            const uint8* rg8Data, uint32 width, uint32 height)
{
    UploadNormalmap(handle, rg8Data, width, height, UploadBand{});
}

void TerrainRenderFeature::UploadNormalmap(TerrainHandle handle,
                                            const uint8* rg8Data, uint32 width, uint32 height,
                                            const UploadBand& band)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (!m_Device || !rg8Data)
        return;

    if (handle.Index >= m_Normalmaps.size())
        m_Normalmaps.resize(handle.Index + 1);

    auto& entry = m_Normalmaps[handle.Index];

    bool recreated = false;
    if (entry.Width != width || entry.Height != height)
    {
        if (entry.Texture.IsValid())
            DeferTextureDestroy(entry.Texture, entry.Bytes);

        Rendering::TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = static_cast<uint32>(Rendering::TextureFormat::R16G16_FLOAT);
        desc.usage = static_cast<uint32>(Rendering::TextureUsage::ShaderResource |
                                         Rendering::TextureUsage::TransferDst);
        desc.debugName = "Terrain_Normalmap";
        entry.Texture = m_Device->CreateTexture(desc);
        MarkAwaitingGpuInit(entry.Texture);
        entry.Width = width;
        entry.Height = height;
        entry.BindlessIndex = 0;
        recreated = true;
    }

    const size_t rowPitchBytes = static_cast<size_t>(width) * 4; // R16G16 = 4 bytes/texel
    QueueBandUpload(entry.Texture, rg8Data, width, height,
                    rowPitchBytes, band, recreated, "Terrain_Normalmap_Staging");
}

uint32 TerrainRenderFeature::GetNormalmapBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_Normalmaps.size())
        return 0;
    return m_Normalmaps[handle.Index].BindlessIndex;
}

void TerrainRenderFeature::RegisterNormalmapBindless(TerrainHandle handle,
                                                      Engine::Renderer::RenderServices& rs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    RegisterBindless(m_Normalmaps, handle, rs);
}

uint32 TerrainRenderFeature::GetHeightmapBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_Heightmaps.size())
        return 0;
    return m_Heightmaps[handle.Index].BindlessIndex;
}

void TerrainRenderFeature::RegisterHeightmapBindless(TerrainHandle handle,
                                                      Engine::Renderer::RenderServices& rs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    RegisterBindless(m_Heightmaps, handle, rs);
}

// ---- Unified tiled-terrain textures (C8) ----

bool TerrainRenderFeature::EnsureUnifiedEntry(std::vector<TextureEntry>& entries,
                                              TerrainHandle handle,
                                              uint32 width, uint32 height, uint32 textureFormat,
                                              size_t bytesPerTexel, const char* debugName,
                                              bool needsStorage, float32 clearValue)
{
    if (handle.Index >= entries.size())
        entries.resize(handle.Index + 1);

    auto& entry = entries[handle.Index];
    if (entry.Texture.IsValid() && entry.Width == width && entry.Height == height)
        return false; // already the right size — reuse (region patches keep it current)

    if (entry.Texture.IsValid())
        DeferTextureDestroy(entry.Texture, entry.Bytes);
    if (entry.StorageView.IsValid())
    {
        DeferTextureViewDestroy(entry.StorageView);
        entry.StorageView = {};
    }

    Rendering::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = textureFormat;
    uint32 usage = static_cast<uint32>(Rendering::TextureUsage::ShaderResource |
                                       Rendering::TextureUsage::TransferDst |
                                       Rendering::TextureUsage::TransferSrc);
    if (needsStorage)
        usage |= static_cast<uint32>(Rendering::TextureUsage::UnorderedAccess);
    desc.usage = usage;
    desc.debugName = debugName;
    entry.Texture = m_Device->CreateTexture(desc);
    MarkAwaitingGpuInit(entry.Texture);
    entry.Width = width;
    entry.Height = height;
    entry.BindlessIndex = 0; // force re-registration

    const size_t bytes = static_cast<size_t>(width) * height * bytesPerTexel;
    if (!entry.Texture.IsValid())
    {
        // A failed allocation would otherwise no-op silently and render the terrain
        // invisible — the exact symptom C8 exists to kill. Fail loudly + reset the entry
        // so a later frame retries instead of caching a dead handle.
        Logger::Log::Error("TerrainRenderFeature: unified {} {}x{} ({} MiB) CreateTexture FAILED "
                           "— tiled terrain will not render",
                           debugName, width, height, bytes / (1024u * 1024u));
        entry = TextureEntry{};
        return false;
    }
    entry.Bytes = bytes;
    m_UnifiedTextureBytesLive += bytes;
    if (m_UnifiedTextureBytesLive > m_UnifiedTextureBytesPeak)
    {
        m_UnifiedTextureBytesPeak = m_UnifiedTextureBytesLive;
        Logger::Log::Info("TerrainRenderFeature: unified/atlas VRAM peak now {} MiB (live {} MiB)",
                          m_UnifiedTextureBytesPeak / (1024u * 1024u),
                          m_UnifiedTextureBytesLive / (1024u * 1024u));
    }
    Logger::Log::Info("TerrainRenderFeature: unified {} {}x{} ({} MiB) created — live {} MiB",
                      debugName, width, height, bytes / (1024u * 1024u),
                      m_UnifiedTextureBytesLive / (1024u * 1024u));

    if (needsStorage)
    {
        // Name the storage view after its texture (height / normal / splat) so a GPU capture
        // distinguishes the three compute-bake targets rather than labelling all three "height".
        const std::string storageName = std::string(debugName) + "_Storage";
        Rendering::TextureViewDesc vd{};
        vd.viewType = Rendering::TextureViewType::View2D;
        vd.baseMip = 0;
        vd.levelCount = 1;
        vd.baseLayer = 0;
        vd.layerCount = 1;
        vd.debugName = storageName.c_str();
        entry.StorageView = m_Device->CreateTextureView(entry.Texture, vd);
    }

    // Zero the fresh texture with a GPU clear (recorded in FlushPendingUploads) — no CPU
    // scratch, no full-texture staging buffer. It moves the texture out of UNDEFINED
    // cleanly so the per-tile region patches (content-preserving) land on defined texels.
    m_PendingClears.push_back({entry.Texture, clearValue});
    // Grass placement samples this texture; the epoch is bumped where the copy is QUEUED so a
    // reader consulting it during this frame's declare already sees the change.
    m_GrassContentEpoch.fetch_add(1, std::memory_order_release);
    return true;
}

bool TerrainRenderFeature::EnsureUnifiedTiledTextures(TerrainHandle handle,
                                                      uint32 width, uint32 height)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || width == 0 || height == 0)
        return false;

    // VRAM staging barrier: after a re-provision retires the old world-sized set, withhold the new
    // world-sized allocation until the old set clears its frames-in-flight quarantine (freed in
    // FlushDeferredDestroys). The terrain renders flat via the bindless-slot-0 sentinel until then
    // — the same transient a re-provision's clear-to-zero already shows — and the extraction
    // system re-invokes this each frame, so the set allocates the frame the barrier passes.
    // GE_TERRAIN_NO_VRAM_STAGING disables the barrier (allocate immediately, old + new coexist):
    // the kill-switch, and the way to measure the un-staged transient peak with this same
    // instrumentation for the before/after pair.
    static const bool kNoVramStaging = std::getenv("GE_TERRAIN_NO_VRAM_STAGING") != nullptr;
    if (!kNoVramStaging && m_UnifiedReprovisionDrainFrame != 0)
    {
        if (m_MonotonicFrame < m_UnifiedReprovisionDrainFrame)
            return false; // old set still quarantined — withhold the new world-sized allocation
        Logger::Log::Info("TerrainRenderFeature: re-provision drain barrier passed at frame {} "
                          "— allocating new unified set", m_MonotonicFrame);
        m_UnifiedReprovisionDrainFrame = 0;
    }

    // OR (not short-circuit) so all three allocate on the recreate frame — a || would skip the
    // splat/normal EnsureUnifiedEntry once the heightmap reported created.
    bool created = EnsureUnifiedEntry(m_Heightmaps, handle, width, height,
                                      static_cast<uint32>(Rendering::TextureFormat::R32_FLOAT),
                                      sizeof(float32), "Terrain_UnifiedHeightmap");
    created |= EnsureUnifiedEntry(m_Splatmaps, handle, width, height,
                                  static_cast<uint32>(Rendering::TextureFormat::RGBA8_UNORM),
                                  4, "Terrain_UnifiedSplatmap");
    created |= EnsureUnifiedEntry(m_Normalmaps, handle, width, height,
                                  static_cast<uint32>(Rendering::TextureFormat::R16G16_FLOAT),
                                  4, "Terrain_UnifiedNormalmap");
    return created;
}

void TerrainRenderFeature::UploadHeightmapRegion(TerrainHandle handle, const float32* src,
                                                 uint32 srcW, uint32 srcH, uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !src || handle.Index >= m_Heightmaps.size())
        return;
    const auto& entry = m_Heightmaps[handle.Index];
    if (!entry.Texture.IsValid() || dstX + srcW > entry.Width || dstY + srcH > entry.Height)
        return;
    // Flag the CBT bisectors overlapping this streamed / re-baked region for VertexEval
    // re-sampling (drained by CBTUpdateSystem). Gated on the same API as QueueRegionUpload
    // (Vulkan-only; the D3D12 tiled draw is a stub), so the rect only accrues where a copy
    // actually lands — an idle / other-API frame stays empty (the quiescence law).
    if (m_Device->GetAPI() != Rendering::GraphicsAPI::DirectX12)
        AccumulateUnifiedHeightDirty(handle, dstX, dstY, srcW, srcH, entry.Width, entry.Height);
    QueueRegionUpload(entry.Texture, reinterpret_cast<const uint8*>(src), srcW, srcH,
                      sizeof(float32), dstX, dstY, "Terrain_UnifiedHeightmap_Region");
}

void TerrainRenderFeature::UploadSplatmapRegion(TerrainHandle handle, const uint8* src,
                                                uint32 srcW, uint32 srcH, uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !src || handle.Index >= m_Splatmaps.size())
        return;
    const auto& entry = m_Splatmaps[handle.Index];
    if (!entry.Texture.IsValid() || dstX + srcW > entry.Width || dstY + srcH > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, src, srcW, srcH, 4, dstX, dstY,
                      "Terrain_UnifiedSplatmap_Region");
}

void TerrainRenderFeature::UploadNormalmapRegion(TerrainHandle handle, const uint8* src,
                                                 uint32 srcW, uint32 srcH, uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !src || handle.Index >= m_Normalmaps.size())
        return;
    const auto& entry = m_Normalmaps[handle.Index];
    if (!entry.Texture.IsValid() || dstX + srcW > entry.Width || dstY + srcH > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, src, srcW, srcH, 4, dstX, dstY,
                      "Terrain_UnifiedNormalmap_Region");
}

// ---- Phase E resident-window atlas height texture ----

void TerrainRenderFeature::EnsureAtlasHeightTexture(TerrainHandle handle, uint32 atlasDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || atlasDim == 0)
        return;
    EnsureUnifiedEntry(m_AtlasHeights, handle, atlasDim, atlasDim,
                       static_cast<uint32>(Rendering::TextureFormat::R32_FLOAT),
                       sizeof(float32), "Terrain_AtlasHeight",
                       /*needsStorage*/ IsGpuHeightBakeEnabled());
}

Rendering::TextureViewHandle TerrainRenderFeature::GetAtlasHeightStorageView(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasHeights.size())
        return {};
    return m_AtlasHeights[handle.Index].StorageView;
}

void TerrainRenderFeature::UploadAtlasHeightSlot(TerrainHandle handle, const float32* slotData,
                                                 uint32 slotStride, uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !slotData || handle.Index >= m_AtlasHeights.size())
        return;
    const auto& entry = m_AtlasHeights[handle.Index];
    if (!entry.Texture.IsValid() || dstX + slotStride > entry.Width || dstY + slotStride > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, reinterpret_cast<const uint8*>(slotData), slotStride, slotStride,
                      sizeof(float32), dstX, dstY, "Terrain_AtlasHeight_Slot");
}

void TerrainRenderFeature::UploadAtlasHeightSlotRegion(TerrainHandle handle, const uint8* data,
                                                       uint32 regionW, uint32 regionH,
                                                       uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !data || handle.Index >= m_AtlasHeights.size())
        return;
    const auto& entry = m_AtlasHeights[handle.Index];
    if (!entry.Texture.IsValid() || dstX + regionW > entry.Width || dstY + regionH > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, data, regionW, regionH, sizeof(float32), dstX, dstY,
                      "Terrain_AtlasHeight_Region");
}

void TerrainRenderFeature::SetCompatLayerTextures(const CompatLayerTextures& textures)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CompatLayerTextures = textures;
}

void TerrainRenderFeature::SetCompatGrassTextures(Rendering::TextureHandle albedo,
                                                  Rendering::TextureHandle alpha,
                                                  Rendering::TextureHandle normal)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CompatLayerTextures.GrassAlbedo = albedo;
    m_CompatLayerTextures.GrassAlpha = alpha;
    m_CompatLayerTextures.GrassNormal = normal;
}

TerrainRenderFeature::CompatLayerTextures TerrainRenderFeature::GetCompatLayerTextures() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_CompatLayerTextures;
}

// The maps a profile with no binding arrays must bind by name (the bindless arm reaches the same
// images through the indices beside them). Same shape as the getters above, one per registry.
Rendering::TextureHandle TerrainRenderFeature::GetNormalmapTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_Normalmaps.size())
        return {};
    return m_Normalmaps[handle.Index].Texture;
}

Rendering::TextureHandle TerrainRenderFeature::GetAtlasNormalTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasNormals.size())
        return {};
    return m_AtlasNormals[handle.Index].Texture;
}

Rendering::TextureHandle TerrainRenderFeature::GetAtlasNormalCoarseTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasNormalCoarse.size())
        return {};
    return m_AtlasNormalCoarse[handle.Index].Texture;
}

Rendering::TextureHandle TerrainRenderFeature::GetAtlasSplatTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasSplats.size())
        return {};
    return m_AtlasSplats[handle.Index].Texture;
}

Rendering::TextureHandle TerrainRenderFeature::GetAtlasSplatCoarseTexture(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasSplatCoarse.size())
        return {};
    return m_AtlasSplatCoarse[handle.Index].Texture;
}

void TerrainRenderFeature::EnsureAtlasCoarseTexture(TerrainHandle handle, uint32 coarseDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || coarseDim == 0)
        return;
    EnsureUnifiedEntry(m_AtlasCoarse, handle, coarseDim, coarseDim,
                       static_cast<uint32>(Rendering::TextureFormat::R32_FLOAT),
                       sizeof(float32), "Terrain_AtlasCoarse");
}

void TerrainRenderFeature::UploadAtlasCoarse(TerrainHandle handle, const float32* data,
                                             uint32 coarseDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !data || handle.Index >= m_AtlasCoarse.size())
        return;
    const auto& entry = m_AtlasCoarse[handle.Index];
    if (!entry.Texture.IsValid() || entry.Width != coarseDim || entry.Height != coarseDim)
        return;
    // Whole-texture (content-preserving is unnecessary — it is fully rewritten). A band upload
    // of rows [0,coarseDim) from the resting ShaderResource layout.
    QueueBandUpload(entry.Texture, reinterpret_cast<const uint8*>(data), coarseDim, coarseDim,
                    static_cast<size_t>(coarseDim) * sizeof(float32), UploadBand{},
                    /*forceFullTexture*/ false, "Terrain_AtlasCoarse_Upload");
}

// ---- Phase E atlas splat + normal surface sources (quality-sweep slice 1) ----

void TerrainRenderFeature::EnsureAtlasSplatTexture(TerrainHandle handle, uint32 atlasDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || atlasDim == 0)
        return;
    // Storage usage (+ a storage view) is needed only for the slice-3 GPU splat imageStore; the
    // surface still samples it bindless. RGBA8_UNORM carries the STORAGE_IMAGE format feature.
    EnsureUnifiedEntry(m_AtlasSplats, handle, atlasDim, atlasDim,
                       static_cast<uint32>(Rendering::TextureFormat::RGBA8_UNORM),
                       4u, "Terrain_AtlasSplat",
                       /*needsStorage*/ IsGpuHeightBakeEnabled());
}

Rendering::TextureViewHandle TerrainRenderFeature::GetAtlasSplatStorageView(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasSplats.size())
        return {};
    return m_AtlasSplats[handle.Index].StorageView;
}

void TerrainRenderFeature::UploadAtlasSplatSlot(TerrainHandle handle, const uint8* rgba8Data,
                                                uint32 slotStride, uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !rgba8Data || handle.Index >= m_AtlasSplats.size())
        return;
    const auto& entry = m_AtlasSplats[handle.Index];
    if (!entry.Texture.IsValid() || dstX + slotStride > entry.Width || dstY + slotStride > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, rgba8Data, slotStride, slotStride, 4u, dstX, dstY,
                      "Terrain_AtlasSplat_Slot");
}

void TerrainRenderFeature::UploadAtlasSplatSlotRegion(TerrainHandle handle, const uint8* data,
                                                      uint32 regionW, uint32 regionH,
                                                      uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !data || handle.Index >= m_AtlasSplats.size())
        return;
    const auto& entry = m_AtlasSplats[handle.Index];
    if (!entry.Texture.IsValid() || dstX + regionW > entry.Width || dstY + regionH > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, data, regionW, regionH, 4u, dstX, dstY,
                      "Terrain_AtlasSplat_Region");
}

void TerrainRenderFeature::EnsureAtlasNormalTexture(TerrainHandle handle, uint32 atlasDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || atlasDim == 0)
        return;
    EnsureUnifiedEntry(m_AtlasNormals, handle, atlasDim, atlasDim,
                       static_cast<uint32>(Rendering::TextureFormat::R16G16_FLOAT),
                       4u, "Terrain_AtlasNormal",
                       /*needsStorage*/ IsGpuHeightBakeEnabled());
}

Rendering::TextureViewHandle TerrainRenderFeature::GetAtlasNormalStorageView(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_AtlasNormals.size())
        return {};
    return m_AtlasNormals[handle.Index].StorageView;
}

void TerrainRenderFeature::UploadAtlasNormalSlot(TerrainHandle handle, const uint8* rg16Data,
                                                 uint32 slotStride, uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !rg16Data || handle.Index >= m_AtlasNormals.size())
        return;
    const auto& entry = m_AtlasNormals[handle.Index];
    if (!entry.Texture.IsValid() || dstX + slotStride > entry.Width || dstY + slotStride > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, rg16Data, slotStride, slotStride, 4u, dstX, dstY,
                      "Terrain_AtlasNormal_Slot");
}

void TerrainRenderFeature::UploadAtlasNormalSlotRegion(TerrainHandle handle, const uint8* data,
                                                       uint32 regionW, uint32 regionH,
                                                       uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !data || handle.Index >= m_AtlasNormals.size())
        return;
    const auto& entry = m_AtlasNormals[handle.Index];
    if (!entry.Texture.IsValid() || dstX + regionW > entry.Width || dstY + regionH > entry.Height)
        return;
    QueueRegionUpload(entry.Texture, data, regionW, regionH, 4u, dstX, dstY,
                      "Terrain_AtlasNormal_Region");
}

void TerrainRenderFeature::EnsureAtlasSplatCoarseTexture(TerrainHandle handle, uint32 coarseDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || coarseDim == 0)
        return;
    EnsureUnifiedEntry(m_AtlasSplatCoarse, handle, coarseDim, coarseDim,
                       static_cast<uint32>(Rendering::TextureFormat::RGBA8_UNORM),
                       4u, "Terrain_AtlasSplatCoarse");
}

void TerrainRenderFeature::UploadAtlasSplatCoarse(TerrainHandle handle, const uint8* rgba8Data,
                                                  uint32 coarseDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !rgba8Data || handle.Index >= m_AtlasSplatCoarse.size())
        return;
    const auto& entry = m_AtlasSplatCoarse[handle.Index];
    if (!entry.Texture.IsValid() || entry.Width != coarseDim || entry.Height != coarseDim)
        return;
    QueueBandUpload(entry.Texture, rgba8Data, coarseDim, coarseDim,
                    static_cast<size_t>(coarseDim) * 4u, UploadBand{},
                    /*forceFullTexture*/ false, "Terrain_AtlasSplatCoarse_Upload");
}

void TerrainRenderFeature::EnsureAtlasNormalCoarseTexture(TerrainHandle handle, uint32 coarseDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || coarseDim == 0)
        return;
    EnsureUnifiedEntry(m_AtlasNormalCoarse, handle, coarseDim, coarseDim,
                       static_cast<uint32>(Rendering::TextureFormat::R16G16_FLOAT),
                       4u, "Terrain_AtlasNormalCoarse");
}

void TerrainRenderFeature::UploadAtlasNormalCoarse(TerrainHandle handle, const uint8* rg16Data,
                                                   uint32 coarseDim)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !rg16Data || handle.Index >= m_AtlasNormalCoarse.size())
        return;
    const auto& entry = m_AtlasNormalCoarse[handle.Index];
    if (!entry.Texture.IsValid() || entry.Width != coarseDim || entry.Height != coarseDim)
        return;
    QueueBandUpload(entry.Texture, rg16Data, coarseDim, coarseDim,
                    static_cast<size_t>(coarseDim) * 4u, UploadBand{},
                    /*forceFullTexture*/ false, "Terrain_AtlasNormalCoarse_Upload");
}

void TerrainRenderFeature::RegisterAtlasSurfaceBindless(TerrainHandle handle,
                                                        Engine::Renderer::RenderServices& rs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!rs.Textures().IsBindlessEnabled())
        return;
    // Register (once) each of the four surface atlas textures. BindlessIndex == 0 means
    // unregistered; EnsureUnifiedEntry resets it to 0 on (re)creation so a resize re-registers.
    // A texture still awaiting its GPU zero-clear publishes under the same rule RegisterBindless
    // applies: the "TerrainUpload" pass is live and this texture's initializer is queued, so the
    // flush records it ahead of every consumer in this same frame. Otherwise its slot stays
    // unpublished and the caller re-registers next frame — on the atlas path that shows as the
    // zero-cleared atlas the surface already samples across a re-provision.
    auto reg = [&](std::vector<TextureEntry>& entries) {
        if (handle.Index >= entries.size())
            return;
        auto& e = entries[handle.Index];
        if (!e.Texture.IsValid() || e.BindlessIndex != 0)
            return;
        if (AwaitsGpuInit(e.Texture) && !(UploadPassIsLive() && HasQueuedGpuInit(e.Texture)))
            return;
        e.BindlessIndex = rs.Textures().GetBindlessIndex(e.Texture);
    };
    reg(m_AtlasSplats);
    reg(m_AtlasNormals);
    reg(m_AtlasSplatCoarse);
    reg(m_AtlasNormalCoarse);
    // Grass samples height (rootY snap) + coarse-height (out-of-window fallback) bindless too.
    reg(m_AtlasHeights);
    reg(m_AtlasCoarse);
}

uint32 TerrainRenderFeature::GetAtlasSplatBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return handle.Index < m_AtlasSplats.size() ? m_AtlasSplats[handle.Index].BindlessIndex : 0u;
}

uint32 TerrainRenderFeature::GetAtlasNormalBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return handle.Index < m_AtlasNormals.size() ? m_AtlasNormals[handle.Index].BindlessIndex : 0u;
}

uint32 TerrainRenderFeature::GetAtlasSplatCoarseBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return handle.Index < m_AtlasSplatCoarse.size() ? m_AtlasSplatCoarse[handle.Index].BindlessIndex : 0u;
}

uint32 TerrainRenderFeature::GetAtlasNormalCoarseBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return handle.Index < m_AtlasNormalCoarse.size() ? m_AtlasNormalCoarse[handle.Index].BindlessIndex : 0u;
}

uint32 TerrainRenderFeature::GetAtlasHeightBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return handle.Index < m_AtlasHeights.size() ? m_AtlasHeights[handle.Index].BindlessIndex : 0u;
}

uint32 TerrainRenderFeature::GetAtlasCoarseBindlessIndex(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return handle.Index < m_AtlasCoarse.size() ? m_AtlasCoarse[handle.Index].BindlessIndex : 0u;
}

bool TerrainRenderFeature::EnsureGrassFieldTexture(TerrainHandle handle, TerrainGrassMap map,
                                                  uint32 width, uint32 height)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device || !width || !height) return false;
    return EnsureUnifiedEntry(m_GrassFields.at(static_cast<size_t>(map)), handle, width, height,
                              static_cast<uint32>(Rendering::TextureFormat::R8G8_UNORM),
                              kTerrainGrassFieldChannels, "Terrain_GrassControl", false, 1.0f);
}

bool TerrainRenderFeature::UploadGrassField(TerrainHandle handle, TerrainGrassMap map,
                                           const uint8* data, uint32 width, uint32 height,
                                           uint32 dstX, uint32 dstY)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto& entries = m_GrassFields.at(static_cast<size_t>(map));
    if (!m_Device || !data || !width || !height || handle.Index >= entries.size()) return false;
    const auto& entry = entries[handle.Index];
    if (!entry.Texture.IsValid() || dstX > entry.Width || width > entry.Width - dstX
        || dstY > entry.Height || height > entry.Height - dstY) return false;
    const size_t before = m_PendingUploads.size();
    QueueRegionUpload(entry.Texture, data, width, height, kTerrainGrassFieldChannels, dstX, dstY,
                      "Terrain_GrassControl_Upload");
    return m_PendingUploads.size() != before;
}

Rendering::TextureHandle TerrainRenderFeature::GetGrassFieldTexture(TerrainHandle handle, TerrainGrassMap map) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto& entries = m_GrassFields.at(static_cast<size_t>(map));
    return handle.Index < entries.size() ? entries[handle.Index].Texture : Rendering::TextureHandle{};
}

uint32 TerrainRenderFeature::GetGrassFieldBindlessIndex(TerrainHandle handle, TerrainGrassMap map) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto& entries = m_GrassFields.at(static_cast<size_t>(map));
    return handle.Index < entries.size() ? entries[handle.Index].BindlessIndex : 0u;
}

void TerrainRenderFeature::RegisterGrassFieldsBindless(TerrainHandle handle, Engine::Renderer::RenderServices& rs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& entries : m_GrassFields) RegisterBindless(entries, handle, rs);
}

Rendering::TextureHandle TerrainRenderFeature::GetInitializedCbtHeightTexture(TerrainHandle handle,
                                                                         TerrainCbtHeightTexture source) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const std::vector<TextureEntry>& entries = source == TerrainCbtHeightTexture::Heightmap   ? m_Heightmaps
                                               : source == TerrainCbtHeightTexture::AtlasHeight ? m_AtlasHeights
                                                                                            : m_AtlasCoarse;
    if (handle.Index >= entries.size())
        return {};
    const auto texture = entries[handle.Index].Texture;
    return AwaitsGpuInit(texture) ? Rendering::TextureHandle{} : texture;
}

bool TerrainRenderFeature::GrassFieldCanBeSampled(TerrainHandle handle, TerrainGrassMap map) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto& entries = m_GrassFields.at(static_cast<size_t>(map));
    if (handle.Index >= entries.size()) return false;
    const auto texture = entries[handle.Index].Texture;
    return texture.IsValid() && (!AwaitsGpuInit(texture) || (UploadPassIsLive() && HasQueuedGpuInit(texture)));
}

void TerrainRenderFeature::ReleaseGrassFieldResourcesLocked(TerrainHandle handle)
{
    bool retired = false;
    for (auto& entries : m_GrassFields)
    {
        if (handle.Index >= entries.size()) continue;
        auto& entry = entries[handle.Index];
        if (!entry.Texture.IsValid()) continue;
        DeferTextureDestroy(entry.Texture, entry.Bytes);
        entry = {};
        retired = true;
    }
    if (retired) m_GrassContentEpoch.fetch_add(1, std::memory_order_release);
}

void TerrainRenderFeature::ReleaseGrassFieldResources(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ReleaseGrassFieldResourcesLocked(handle);
}

bool TerrainRenderFeature::TryGetUnifiedGrassMaps(Rendering::TextureHandle& height,
                                                  Rendering::TextureHandle& normal,
                                                  Rendering::TextureHandle& splat, Rendering::TextureHandle* controls) const
{
    TerrainHandle chosen{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const auto& t : m_ActiveTerrains)
        {
            if (t.AtlasBacked)
                continue;
            chosen = t.Handle;
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    height = GetHeightmapTexture(chosen);
    normal = GetNormalmapTexture(chosen);
    splat = GetSplatmapTexture(chosen);
    if (controls) *controls = GetGrassFieldTexture(chosen, TerrainGrassMap::Unified);
    return true;
}

bool TerrainRenderFeature::TryGetAtlasGrassSource(AtlasGrassSource& out) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const TerrainInstanceInfo* chosen = nullptr;
    uint32 atlasCount = 0;
    for (const auto& t : m_ActiveTerrains)
    {
        if (!t.AtlasBacked || t.AtlasDim == 0)
            continue;
        ++atlasCount;
        if (!chosen)
            chosen = &t;
    }
    if (atlasCount > 1 && !m_MultiAtlasGrassWarned.exchange(true))
        Logger::Log::Warning("TerrainGrass: {} atlas-backed terrains active; grass resolves ALL of "
                             "them through the first terrain's atlas — other atlas terrains' blades "
                             "would snap to the wrong heights. Single atlas terrain is supported.",
                             atlasCount);
    if (chosen)
    {
        const TerrainInstanceInfo& t = *chosen;
        out.Valid = true;
        out.Identity = (static_cast<uint64>(t.Handle.Index) << 32) | t.Handle.Generation;
        out.AtlasDim = t.AtlasDim;
        out.AtlasSlotStride = t.AtlasSlotStride;
        out.AtlasSlotsPerRow = t.AtlasSlotsPerRow;
        out.AtlasTileRes = t.AtlasTileRes;
        out.AtlasTilesPerAxisX = t.AtlasTilesPerAxisX;
        out.AtlasTilesPerAxisZ = t.AtlasTilesPerAxisZ;
        out.AtlasCoarseDim = t.AtlasCoarseDim;
        out.HeightBindless = t.AtlasHeightBindlessIndex;
        out.HeightCoarseBindless = t.AtlasCoarseBindlessIndex;
        out.NormalBindless = t.AtlasNormalBindlessIndex;
        out.NormalCoarseBindless = t.AtlasNormalCoarseBindlessIndex;
        out.SplatBindless = t.AtlasSplatBindlessIndex;
        out.SplatCoarseBindless = t.AtlasSplatCoarseBindlessIndex;
        out.GrassRegionsActive = t.GrassRegionsActive;
        out.GrassBindless = t.AtlasGrassBindlessIndex;
        out.GrassCoarseBindless = t.AtlasGrassCoarseBindlessIndex;
        // Read the registries directly rather than through GetAtlas*Texture: m_Mutex is already
        // held here and those getters take it again.
        const auto textureAt = [&](const std::vector<TextureEntry>& entries) {
            return t.Handle.Index < entries.size() ? entries[t.Handle.Index].Texture
                                                   : Rendering::TextureHandle{};
        };
        out.HeightTexture = textureAt(m_AtlasHeights);
        out.HeightCoarseTexture = textureAt(m_AtlasCoarse);
        out.NormalTexture = textureAt(m_AtlasNormals);
        out.NormalCoarseTexture = textureAt(m_AtlasNormalCoarse);
        out.SplatTexture = textureAt(m_AtlasSplats);
        out.SplatCoarseTexture = textureAt(m_AtlasSplatCoarse);
        out.GrassTexture = textureAt(m_GrassFields[static_cast<size_t>(TerrainGrassMap::Atlas)]);
        out.GrassCoarseTexture = textureAt(m_GrassFields[static_cast<size_t>(TerrainGrassMap::Coarse)]);
        out.TableVersion = t.AtlasTableVersion;
        out.RowCount = t.AtlasRowCount;
        out.RowBytes = t.AtlasRowBytes;
        return true;
    }
    out.Valid = false;
    return false;
}

void TerrainRenderFeature::AccumulateAtlasHeightDirtyUV(TerrainHandle handle, float32 minU,
                                                        float32 minV, float32 maxU, float32 maxV)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_UnifiedHeightDirty.size())
        m_UnifiedHeightDirty.resize(handle.Index + 1);
    auto& d = m_UnifiedHeightDirty[handle.Index];
    const float32 u0 = std::clamp(minU, 0.0f, 1.0f);
    const float32 v0 = std::clamp(minV, 0.0f, 1.0f);
    const float32 u1 = std::clamp(maxU, 0.0f, 1.0f);
    const float32 v1 = std::clamp(maxV, 0.0f, 1.0f);
    if (!d.HasRect)
    {
        d.MinU = u0;
        d.MinV = v0;
        d.MaxU = u1;
        d.MaxV = v1;
        d.HasRect = true;
    }
    else
    {
        d.MinU = std::min(d.MinU, u0);
        d.MinV = std::min(d.MinV, v0);
        d.MaxU = std::max(d.MaxU, u1);
        d.MaxV = std::max(d.MaxV, v1);
    }
    ++d.Version;
}

// ---- Unified-texture height dirty-region accumulator ----

void TerrainRenderFeature::AccumulateUnifiedHeightDirty(TerrainHandle handle, uint32 dstX,
                                                        uint32 dstY, uint32 srcW, uint32 srcH,
                                                        uint32 texW, uint32 texH)
{
    if (texW == 0u || texH == 0u)
        return;
    if (handle.Index >= m_UnifiedHeightDirty.size())
        m_UnifiedHeightDirty.resize(handle.Index + 1);
    auto& d = m_UnifiedHeightDirty[handle.Index];

    // Texel rect -> UV [0,1], padded ONE texel on each side before clamping. CBT samples
    // the height via linear-clamp filtering, so texels [dstX, dstX+srcW-1] influence every
    // uv out to (dstX-0.5)/W .. (dstX+srcW+0.5)/W — half a texel past the raw texel rect on
    // each side. A fine bisector whose corner-UV AABB lies in that half-texel band (routine
    // at Full-tile borders, which share exactly one texel column with a neighbor) would
    // sample re-uploaded heights yet never be flagged MODIFIED -> a stale-corner seam around
    // every re-baked tile. Padding one full texel (conservative, covers the half-texel
    // footprint) closes it; the clamp keeps an edge-touching rect inside [0,1].
    const float32 invW = 1.0f / static_cast<float32>(texW);
    const float32 invH = 1.0f / static_cast<float32>(texH);
    const float32 u0 = std::clamp((static_cast<float32>(dstX) - 1.0f) * invW, 0.0f, 1.0f);
    const float32 v0 = std::clamp((static_cast<float32>(dstY) - 1.0f) * invH, 0.0f, 1.0f);
    const float32 u1 = std::clamp((static_cast<float32>(dstX + srcW) + 1.0f) * invW, 0.0f, 1.0f);
    const float32 v1 = std::clamp((static_cast<float32>(dstY + srcH) + 1.0f) * invH, 0.0f, 1.0f);

    if (!d.HasRect)
    {
        d.MinU = u0;
        d.MinV = v0;
        d.MaxU = u1;
        d.MaxV = v1;
        d.HasRect = true;
    }
    else
    {
        d.MinU = std::min(d.MinU, u0);
        d.MinV = std::min(d.MinV, v0);
        d.MaxU = std::max(d.MaxU, u1);
        d.MaxV = std::max(d.MaxV, v1);
    }
    ++d.Version;
}

bool TerrainRenderFeature::TakeUnifiedHeightDirtyRect(TerrainHandle handle, float32& minU,
                                                      float32& minV, float32& maxU, float32& maxV,
                                                      uint64& version)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_UnifiedHeightDirty.size())
        return false;
    auto& d = m_UnifiedHeightDirty[handle.Index];
    version = d.Version;
    if (!d.HasRect)
        return false;
    minU = d.MinU;
    minV = d.MinV;
    maxU = d.MaxU;
    maxV = d.MaxV;
    // Drain: reset to the inverted-empty sentinel (Version stays monotonic).
    d.MinU = 1.0f;
    d.MinV = 1.0f;
    d.MaxU = 0.0f;
    d.MaxV = 0.0f;
    d.HasRect = false;
    return true;
}

bool TerrainRenderFeature::PeekUnifiedHeightDirtyRect(TerrainHandle handle, float32& minU,
                                                      float32& minV, float32& maxU, float32& maxV,
                                                      uint64& version) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (handle.Index >= m_UnifiedHeightDirty.size())
        return false;
    const auto& d = m_UnifiedHeightDirty[handle.Index];
    version = d.Version;
    if (!d.HasRect)
        return false;
    minU = d.MinU;
    minV = d.MinV;
    maxU = d.MaxU;
    maxV = d.MaxV;
    return true;
}

// ---- Deferred texture uploads ----

bool TerrainRenderFeature::HasPendingUploads() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return !m_PendingUploads.empty() || !m_PendingClears.empty();
}

bool TerrainRenderFeature::TryClaimHeightmapUploadFrameRG(uint64_t rgFrameIndex, const void* graph)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_RGHeightmapUploadFrame == rgFrameIndex)
        return false;
    m_RGHeightmapUploadFrame = rgFrameIndex;
    m_RGHeightmapUploadGraph = graph;
    m_RGHeightmapUploadPass = Rendering::RenderGraph::kInvalidId;
    return true;
}

void TerrainRenderFeature::RecordHeightmapUploadPassRG(uint64_t rgFrameIndex, const void* graph,
                                                       Rendering::RenderGraph::RGPassId pass)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_RGHeightmapUploadFrame != rgFrameIndex || m_RGHeightmapUploadGraph != graph)
        return;
    m_RGHeightmapUploadPass = pass;
}

Rendering::RenderGraph::RGPassId
TerrainRenderFeature::GetHeightmapUploadPassRG(uint64_t rgFrameIndex, const void* graph) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_RGHeightmapUploadFrame != rgFrameIndex || m_RGHeightmapUploadGraph != graph)
        return Rendering::RenderGraph::kInvalidId;
    return m_RGHeightmapUploadPass;
}

bool TerrainRenderFeature::HeightmapUploadClaimedInOtherGraph(uint64_t rgFrameIndex,
                                                              const void* graph) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_RGHeightmapUploadFrame == rgFrameIndex && m_RGHeightmapUploadGraph != nullptr &&
           m_RGHeightmapUploadGraph != graph;
}

void TerrainRenderFeature::QueueBandUpload(Rendering::TextureHandle texture,
                                           const uint8* srcBytes, uint32 width, uint32 height,
                                           size_t rowPitchBytes, const UploadBand& band,
                                           bool forceFullTexture, const char* stagingName)
{
    if (!texture.IsValid() || srcBytes == nullptr || width == 0 || height == 0)
        return;

    // D3D12 requires a 256-aligned row pitch and 512-aligned source offset. The
    // terrain formats do not satisfy this (a 2049-wide R32F row is 8196 B), so the
    // D3D12 lane keeps the whole-texture path until a repack lane exists. Production
    // runs Vulkan and D3D12 terrain indirect draws are stubs, so this only defers a
    // future optimization, it does not change today's D3D12 behavior.
    const bool d3d12 = m_Device->GetAPI() == Rendering::GraphicsAPI::DirectX12;

    uint32 dstRow = band.DstRow;
    uint32 rowCount = band.RowCount;
    bool fullTexture = forceFullTexture || d3d12 || rowCount == 0 ||
                       (dstRow == 0 && rowCount >= height);
    if (fullTexture)
    {
        dstRow = 0;
        rowCount = height;
    }
    else
    {
        if (dstRow >= height)
            return; // band entirely past the texture — nothing to upload
        rowCount = std::min(rowCount, height - dstRow);
    }

    // The staging buffer holds exactly the band's rows, tightly packed, so the
    // copy's source offset is 0 and the source pointer is offset by DstRow rows.
    const size_t uploadSize = static_cast<size_t>(rowCount) * rowPitchBytes;
    // The one-shot test seam yields the same invalid handle the allocator does on failure, so the
    // arm below is the production return, not a parallel one.
    const bool failStaging = std::exchange(m_FailNextStagingAllocation, false);
    auto staging = failStaging ? Rendering::BufferHandle{}
                               : m_Device->CreateUploadBuffer(uploadSize, stagingName);
    if (!staging.IsValid())
        return;
    m_Device->UpdateBuffer(staging, 0, uploadSize,
                           srcBytes + static_cast<size_t>(dstRow) * rowPitchBytes);

    PendingTextureUpload pending{};
    pending.StagingBuffer = staging;
    pending.Texture = texture;
    pending.CopyWidth = width;      // full-width band: copy spans the whole row
    pending.CopyHeight = rowCount;
    pending.RowPitchBytes = rowPitchBytes;
    pending.DstX = 0;
    pending.DstY = dstRow;
    pending.FullTexture = fullTexture;
    m_PendingUploads.push_back(pending);
    // Grass placement samples this texture; the epoch is bumped where the copy is QUEUED so a
    // reader consulting it during this frame's declare already sees the change.
    m_GrassContentEpoch.fetch_add(1, std::memory_order_release);
}

void TerrainRenderFeature::QueueRegionUpload(Rendering::TextureHandle texture,
                                             const uint8* srcBytes, uint32 srcW, uint32 srcH,
                                             size_t bytesPerTexel, uint32 dstX, uint32 dstY,
                                             const char* stagingName)
{
    if (!texture.IsValid() || srcBytes == nullptr || srcW == 0 || srcH == 0)
        return;

    // D3D12 requires 256-aligned row pitch / 512-aligned source offset that the
    // terrain formats do not satisfy; the tiled render path is Vulkan-only today
    // (D3D12 terrain indirect draws are stubs), so skip the sub-rect copy there.
    if (m_Device->GetAPI() == Rendering::GraphicsAPI::DirectX12)
        return;

    const size_t rowPitchBytes = static_cast<size_t>(srcW) * bytesPerTexel;
    const size_t uploadSize = static_cast<size_t>(srcH) * rowPitchBytes;
    auto staging = m_Device->CreateUploadBuffer(uploadSize, stagingName);
    if (!staging.IsValid())
        return;
    m_Device->UpdateBuffer(staging, 0, uploadSize, srcBytes);

    PendingTextureUpload pending{};
    pending.StagingBuffer = staging;
    pending.Texture = texture;
    pending.CopyWidth = srcW;
    pending.CopyHeight = srcH;
    pending.RowPitchBytes = rowPitchBytes;
    pending.DstX = dstX;
    pending.DstY = dstY;
    pending.FullTexture = false; // preserve untouched texels (content-preserving)
    m_PendingUploads.push_back(pending);
    // Grass placement samples this texture; the epoch is bumped where the copy is QUEUED so a
    // reader consulting it during this frame's declare already sees the change.
    m_GrassContentEpoch.fetch_add(1, std::memory_order_release);
}

void TerrainRenderFeature::FlushPendingUploads(Rendering::CommandList* cl)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (!cl)
        return;

    ++m_FlushCount;

    // Zero-clear freshly (re)created unified textures first: UNDEFINED -> CopyDest ->
    // clear(0) -> ShaderResource, so the per-tile region patches below (which preserve
    // untouched texels from the ShaderResource resting layout) start from a defined
    // texture. A GPU clear avoids a full-texture CPU/staging buffer entirely.
    for (const auto& clear : m_PendingClears)
    {
        const auto tex = clear.Texture;
        if (!tex.IsValid())
            continue;
        cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            tex, Rendering::ResourceState::Undefined, Rendering::ResourceState::CopyDest));
        const float value[4] = {clear.Value, clear.Value, clear.Value, clear.Value};
        cl->ClearColorImageSubresource(tex, 0, 0, value);
        cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            tex, Rendering::ResourceState::CopyDest, Rendering::ResourceState::ShaderResource));
        m_TexturesAwaitingGpuInit.erase(tex.id);
    }
    m_PendingClears.clear();

    for (auto& upload : m_PendingUploads)
    {
        if (!upload.StagingBuffer.IsValid())
            continue;

        // A whole-texture write starts UNDEFINED and may discard freely (today's
        // path). A partial band must preserve the untouched texels, so we tell the
        // copy the texture's resting layout (ShaderResource): the internal pre-copy
        // barrier then transitions FROM it — content-preserving, and correctly
        // serialized against in-flight shader reads of the same texture.
        const Rendering::ResourceState currentState =
            upload.FullTexture ? Rendering::ResourceState::Undefined
                               : Rendering::ResourceState::ShaderResource;

        cl->CopyBufferToTextureSubresource(
            upload.StagingBuffer, upload.Texture, 0, 0,
            upload.CopyWidth, upload.CopyHeight, 0, upload.RowPitchBytes,
            /*depth=*/1, /*srcSlicePitchBytes=*/0,
            upload.DstX, upload.DstY, currentState);

        // Transition to ShaderResource for sampling in subsequent passes.
        cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            upload.Texture, Rendering::ResourceState::CopyDest, Rendering::ResourceState::ShaderResource));

        // A whole-texture copy defines every texel, so it is the initialization a band or a
        // tile sub-rect (which only preserves) is not: it releases the bindless gate.
        if (upload.FullTexture)
            m_TexturesAwaitingGpuInit.erase(upload.Texture.id);

        // Defer staging buffer destruction until GPU has completed the copy.
        DeferBufferDestroy(upload.StagingBuffer);
    }

    m_PendingUploads.clear();
}

void TerrainRenderFeature::TryCleanupStaleViews(uint32 frameIndex, Engine::Renderer::RenderServices& rs)
{
    uint32 expected = m_LastCleanupFrame.load(std::memory_order_relaxed);
    if (expected == frameIndex)
        return; // already retired this frame
    if (!m_LastCleanupFrame.compare_exchange_strong(expected, frameIndex, std::memory_order_relaxed))
        return;

    FlushDeferredDestroys(rs);

    // Reaching here IS "the active render pipeline declares a TerrainUpload pass": this function
    // has exactly one caller, TerrainUploadNode::DeclareForView. Date it against the extraction
    // clock the registrars read, so a registration made before the next declare knows a flush is
    // coming this frame (see m_UploadPassSeenExtractionFrame).
    m_UploadPassSeenExtractionFrame.store(m_ExtractionFrame.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
}

void TerrainRenderFeature::SetActiveTerrains(std::vector<TerrainInstanceInfo> terrains)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ActiveTerrains = std::move(terrains);
}

std::vector<TerrainInstanceInfo> TerrainRenderFeature::GetActiveTerrains() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_ActiveTerrains;
}

TerrainRenderFeature::ActiveHeightSource TerrainRenderFeature::GetActiveHeightSource() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_ActiveTerrains.empty())
        return {};
    const TerrainInstanceInfo& terrain = m_ActiveTerrains.front();
    return {true, terrain.HeightmapTexture, terrain.AtlasBacked, terrain.AtlasHeightTexture};
}

void TerrainRenderFeature::EnsureMaterialTableBuffer(uint32 fi, uint32 capacity)
{
    if (!m_Device || capacity <= m_TerrainMaterialCapacity[fi])
        return;

    DeferBufferDestroy(m_TerrainMaterialTableSSBO[fi]);

    Rendering::BufferDesc desc{};
    desc.size = capacity * sizeof(Terrain::TerrainMaterialRecord);
    desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage);
    desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
    desc.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "Terrain_MaterialTable_SSBO";
    m_TerrainMaterialTableSSBO[fi] = m_Device->CreateBuffer(desc);
    m_TerrainMaterialCapacity[fi] = capacity;

    // A record read before it is authored must shade a definite (black, untextured) material
    // rather than reinterpret whatever the allocation held: every texture slot has to be the
    // reserved bindless sentinel, which only a zeroed buffer guarantees.
    if (void* mapped = m_Device->MapBuffer(m_TerrainMaterialTableSSBO[fi]))
    {
        std::memset(mapped, 0, static_cast<size_t>(desc.size));
        m_Device->UnmapBuffer(m_TerrainMaterialTableSSBO[fi]);
    }
}

void TerrainRenderFeature::WarnIfUploadPassMissing()
{
    if (m_MissingUploadPassReported || m_FlushCount != 0)
        return;
    if (m_TexturesAwaitingGpuInit.empty())
        return;
    if (++m_FramesAwaitingUploadPass < kUploadPassAbsentFrameLimit)
        return;

    m_MissingUploadPassReported = true;
    Logger::Log::Error(
        "TerrainRenderFeature: {} terrain texture(s) have waited {} frames for their GPU "
        "zero-clear/upload and FlushPendingUploads has never run — the active render pipeline "
        "declares no \"TerrainUpload\" pass. Their bindless slots stay unpublished, so every "
        "bindless tap takes its no-texture branch: the surface skips its splat and grass roots "
        "collapse to the terrain's origin height. The CBT height sample is bound directly rather "
        "than bindlessly, so it is not gated at all and reads an image nothing has written — "
        "undefined per driver, which on MoltenVK comes back zero and renders as flat terrain. "
        "Add a TerrainUpload node to the pipeline's .rendergraph (see "
        "Assets/RenderPipelines/ForwardPlus.rendergraph).",
        m_TexturesAwaitingGpuInit.size(), m_FramesAwaitingUploadPass);
}

void TerrainRenderFeature::UploadTerrainParamsArray(const Terrain::TerrainGPUParams* params, uint32 count,
                                                    const Terrain::TerrainMaterialRecord* materials,
                                                    uint32 materialCount, uint32 deviceFrameIndex)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // The extraction system's once-per-frame tick, and the only one that still runs when the
    // render graph has no terrain pass at all — so it is where a never-draining upload queue
    // becomes observable.
    WarnIfUploadPassMissing();

    // One element per device frame, not one per value of the device's wrapped slot — see the
    // reuse-distance argument on the declaration. The unwrapped counter doubles as the
    // extraction clock the bindless gate dates the upload pass against: published here, at the
    // END of the tick, so the registrars earlier in the same tick still read the previous value.
    const uint32 extractionFrame = m_ParamsFrameCounter.Tick(deviceFrameIndex);
    m_ExtractionFrame.store(extractionFrame, std::memory_order_relaxed);
    const uint32 fi = extractionFrame % kMaxFrames;
    m_TerrainParamsCount[fi] = count;
    // Kept so a reader can compare the array's CONTENT. The ring slot rotates every frame whether
    // or not anything in it changed, so the slot index says nothing about whether a consumer's
    // answer would differ.
    {
        const size_t paramsBytes =
            params ? static_cast<size_t>(count) * sizeof(Terrain::TerrainGPUParams) : 0u;
        m_LastTerrainParamsBytes.resize(paramsBytes);
        if (paramsBytes != 0)
            std::memcpy(m_LastTerrainParamsBytes.data(), params, paramsBytes);
    }
    // Published from the ARGUMENTS, ahead of the upload: it says a table was offered for this
    // frame's params, not that its bytes reached the buffer. A params entry addresses its
    // materials by absolute table index, so the guard that matters here is that params and
    // materials arrived together — a count without params would name a table nothing indexes.
    // The upload can still fail afterwards (no device, or an unmappable buffer), and the frame's
    // records then stay whatever the element last held; the buffer is zero-filled on creation so
    // that reads as the definite black material rather than as reinterpreted memory.
    m_TerrainMaterialCount[fi] = (params && count != 0 && materials) ? materialCount : 0u;
    uint32 activeGrassCount = 0;
    uint32 maxBladeSegments = 5u;
    GrassPlacementSummary placement{};
    bool firstGrassRow = true;
    if (params)
    {
        for (uint32 i = 0; i < count; ++i)
        {
            const auto& p = params[i];
            // The placement compute selects the MODE's density — cards/m² for a texture-card
            // terrain, blades/m² otherwise (terrain_grass_place.comp) — so the gate and the budget
            // fit have to reason about the same number. Charging a card terrain for a blade
            // density it never places over-states demand and shortens its range for grass that was
            // never going to be spawned; gating on the blade density alone would also drop a card
            // terrain whose blade density happens to be zero.
            const float32 nearDensity = Terrain::TerrainGrassNearDensity(p);
            // The same predicate extraction cleared bit 0 with, so this reduction and the GPU
            // classify cover the identical set of rows and the bound below binds every cell.
            if (Terrain::TerrainGrassRowPlaces(p))
            {
                ++activeGrassCount;
                maxBladeSegments = std::max(maxBladeSegments, p.GrassBladeSegments);

                placement.MaxNearDensity = std::max(placement.MaxNearDensity, nearDensity);
                placement.MaxRange = std::max(placement.MaxRange, p.GrassRange);
                placement.MaxBladeHeight = std::max(placement.MaxBladeHeight, p.GrassBladeHeight);
                placement.MinFalloff = firstGrassRow
                    ? p.GrassDensityFalloff
                    : std::min(placement.MinFalloff, p.GrassDensityFalloff);
                placement.WorldMinY = firstGrassRow
                    ? p.WorldOriginY
                    : std::min(placement.WorldMinY, p.WorldOriginY);
                placement.WorldMaxY = firstGrassRow
                    ? p.WorldOriginY + p.HeightScale
                    : std::max(placement.WorldMaxY, p.WorldOriginY + p.HeightScale);
                const bool rowBlendMode =
                    (p.GrassEnabled & Terrain::kTerrainGrassBitBlendMode) != 0u;
                placement.AllBlendMode =
                    rowBlendMode && (firstGrassRow || placement.AllBlendMode);
                placement.AnyAlphaNeeded = placement.AnyAlphaNeeded ||
                    (p.GrassEnabled & Terrain::kTerrainGrassBitAlphaNeeded) != 0u;
                firstGrassRow = false;
            }
        }
    }
    m_TerrainGrassActiveCount[fi] = activeGrassCount;
    m_TerrainGrassMaxBladeSegments[fi] = activeGrassCount > 0 ? maxBladeSegments : 5u;
    m_TerrainGrassPlacement[fi] = placement;
    m_LastTerrainParamsSlot.store(fi, std::memory_order_release);

    // GE_TERRAIN_GRASS_DEBUG: the authoritative upload — the slot the node then reads via
    // GetLastTerrainParamsSlot + the count it derives. If the grass node logs a DIFFERENT slot/active
    // than the LAST line here, a later UploadTerrainParamsArray (a second world/registry, or the
    // single-terrain path) overwrote the slot with 0 grass -> the node declares nothing. Throttled.
    static const bool kUpDbg = std::getenv("GE_TERRAIN_GRASS_DEBUG") != nullptr;
    static uint32 s_LastUpKey = 0xFFFFFFFFu;
    // Key on (count, activeGrass) only — NOT the cyclic frame slot fi, which changes every frame and
    // would re-log the idle count==0 case forever, flooding the log ring at boot.
    const uint32 upKey = (count << 8) ^ activeGrassCount;
    if (kUpDbg && upKey != s_LastUpKey)
    {
        s_LastUpKey = upKey;
        Logger::Log::Info("TerrainRenderFeature.Upload slot={} count={} activeGrass={}",
                          fi, count, activeGrassCount);
    }

    if (!m_Device || count == 0 || !params)
        return;

    const uint32 requiredSize = count * sizeof(Terrain::TerrainGPUParams);

    if (count > m_TerrainParamsCapacity[fi])
    {
        DeferBufferDestroy(m_TerrainParamsSSBO[fi]);

        const uint32 newCapacity = count + count / 2 + 1;

        Rendering::BufferDesc desc{};
        desc.size = newCapacity * sizeof(Terrain::TerrainGPUParams);
        desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage);
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
        desc.debugName = "Terrain_Params_SSBO";
        m_TerrainParamsSSBO[fi] = m_Device->CreateBuffer(desc);
        m_TerrainParamsCapacity[fi] = newCapacity;
    }

    void* mapped = m_Device->MapBuffer(m_TerrainParamsSSBO[fi]);
    if (mapped)
    {
        std::memcpy(mapped, params, requiredSize);
        m_Device->UnmapBuffer(m_TerrainParamsSSBO[fi]);
    }

    if (materials && materialCount != 0)
    {
        EnsureMaterialTableBuffer(fi, materialCount);
        if (void* mappedMaterials = m_Device->MapBuffer(m_TerrainMaterialTableSSBO[fi]))
        {
            std::memcpy(mappedMaterials, materials,
                        static_cast<size_t>(materialCount) * sizeof(Terrain::TerrainMaterialRecord));
            m_Device->UnmapBuffer(m_TerrainMaterialTableSSBO[fi]);
        }
    }
}

// ---------------------------------------------------------------------------
// GPU height bake (slice-1b, GE_TERRAIN_GPU_BAKE)
// ---------------------------------------------------------------------------

// ONE definition of the splat pass's descriptor layout: the pipeline is created from it and the
// per-bake descriptor set is allocated against it, and a layout that disagrees with the pipeline
// it is bound to is a validation error at dispatch time rather than at the edit that caused it.
static Rendering::DescriptorSetLayoutDesc MakeSplatBakeLayoutDesc()
{
    Rendering::DescriptorSetLayoutDesc l;
    l.debugName = "TerrainSplatBake_DSLayout";
    l.bindings = {
        {0, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasHeightRead"},
        {1, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasSplatStorage"},
        {2, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "SurfaceRuleBuffer"},
        {3, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "SurfaceRuleConditionBuffer"},
    };
    return l;
}

bool TerrainRenderFeature::EnsureGpuBakePipeline()
{
    if (m_GpuBakePipeline.IsValid())
        return true;
    if (m_GpuBakePipelineAttempted)
        return false;
    m_GpuBakePipelineAttempted = true;
    if (!m_Device)
        return false;

    namespace fs = std::filesystem;
    const fs::path shaderDir = PathUtils::GetInstallAssetsRoot() / "Shaders" / "CBT";
    std::error_code ec;
    if (!fs::exists(shaderDir / "terrain_height_bake.comp", ec))
    {
        Logger::Log::Warning("TerrainRenderFeature: terrain_height_bake.comp not found — GPU bake disabled");
        return false;
    }

    Rendering::ShaderProgramCompileRequest req{};
    req.debugName = "terrain_height_bake";
    req.baseDirectory = shaderDir;
    req.cacheRoot = fs::path(".Cache") / "Shaders";
    req.includeDirs = {shaderDir};
    req.stages = {{"cs", "terrain_height_bake.comp", "main", {}}};
    Rendering::ShaderProgramCompileResult result{};
    std::string err;
    if (!Rendering::ShaderCompileService::CompileProgramToCache(req, m_Device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("TerrainRenderFeature: height bake shader compile failed: {}", err);
        return false;
    }
    auto itCs = result.stageBytes.find("cs");
    if (itCs == result.stageBytes.end())
    {
        Logger::Log::Error("TerrainRenderFeature: height bake shader produced no compute SPIR-V");
        return false;
    }

    Rendering::DescriptorSetLayoutDesc layout;
    layout.debugName = "TerrainHeightBake_DSLayout";
    layout.bindings = {
        {0, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "ModifierBuffer"},
        {1, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasHeightStorage"},
    };

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8>>(std::move(itCs->second));
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    cd.PushConstants.Size = sizeof(TerrainHeightBakePush);
    cd.PushConstants.StageMask = Rendering::kShaderStageCompute;
    cd.DebugName = "TerrainHeightBake_Pipeline";
    m_GpuBakePipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
    if (!m_GpuBakePipeline.IsValid())
    {
        Logger::Log::Error("TerrainRenderFeature: height bake compute pipeline creation failed");
        return false;
    }
    Logger::Log::Info("TerrainRenderFeature: GPU height bake pipeline ready");

    // Slice-2 normal-derive pipeline: same source, KERNEL_NORMAL variant. Layout: height atlas
    // storage image (read) + normal atlas storage image (write). Optional — the height bake still
    // works if this fails (the CPU normal regen at settle covers it).
    Rendering::ShaderProgramCompileRequest nreq = req;
    nreq.debugName = "terrain_normal_bake";
    nreq.stages = {{"cs", "terrain_height_bake.comp", "main", {"KERNEL_NORMAL"}}};
    Rendering::ShaderProgramCompileResult nresult{};
    std::string nerr;
    if (Rendering::ShaderCompileService::CompileProgramToCache(nreq, m_Device->PreferredShaderSource(), nresult, &nerr))
    {
        auto itN = nresult.stageBytes.find("cs");
        if (itN != nresult.stageBytes.end())
        {
            Rendering::DescriptorSetLayoutDesc nlayout;
            nlayout.debugName = "TerrainNormalBake_DSLayout";
            nlayout.bindings = {
                {0, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasHeightRead"},
                {1, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasNormalStorage"},
            };
            Rendering::ComputePipelineDesc nd{};
            nd.ComputeShader = std::make_shared<const std::vector<uint8>>(std::move(itN->second));
            nd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(nlayout));
            nd.PushConstants.Size = sizeof(TerrainHeightBakePush);
            nd.PushConstants.StageMask = Rendering::kShaderStageCompute;
            nd.DebugName = "TerrainNormalBake_Pipeline";
            m_GpuNormalPipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(nd));
            if (m_GpuNormalPipeline.IsValid())
                Logger::Log::Info("TerrainRenderFeature: GPU normal bake pipeline ready");
        }
    }
    else
    {
        Logger::Log::Warning("TerrainRenderFeature: normal bake shader compile failed: {}", nerr);
    }

    // Splat-derive pipeline: same source, KERNEL_SPLAT variant. Layout: height atlas storage image
    // (read) + RGBA8 splat atlas storage image (write) + the surface-rule row and condition
    // buffers. Optional — the height/normal bake still works if this fails (the CPU splat regen at
    // settle covers it).
    Rendering::ShaderProgramCompileRequest sreq = req;
    sreq.debugName = "terrain_splat_bake";
    sreq.stages = {{"cs", "terrain_height_bake.comp", "main", {"KERNEL_SPLAT"}}};
    Rendering::ShaderProgramCompileResult sresult{};
    std::string serr;
    if (Rendering::ShaderCompileService::CompileProgramToCache(sreq, m_Device->PreferredShaderSource(), sresult, &serr))
    {
        auto itS = sresult.stageBytes.find("cs");
        if (itS != sresult.stageBytes.end())
        {
            Rendering::DescriptorSetLayoutDesc slayout = MakeSplatBakeLayoutDesc();
            Rendering::ComputePipelineDesc sd{};
            sd.ComputeShader = std::make_shared<const std::vector<uint8>>(std::move(itS->second));
            sd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(slayout));
            sd.PushConstants.Size = sizeof(TerrainHeightBakePush);
            sd.PushConstants.StageMask = Rendering::kShaderStageCompute;
            sd.DebugName = "TerrainSplatBake_Pipeline";
            m_GpuSplatPipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(sd));
            if (m_GpuSplatPipeline.IsValid())
                Logger::Log::Info("TerrainRenderFeature: GPU splat bake pipeline ready");
        }
    }
    else
    {
        Logger::Log::Warning("TerrainRenderFeature: splat bake shader compile failed: {}", serr);
    }
    return true;
}

void TerrainRenderFeature::QueueGpuHeightBake(TerrainHandle handle, std::vector<ModifierGpu> modifiers,
                                              std::vector<GpuHeightBakeDispatch> dispatches, bool settle,
                                              float splatMinH, float splatMaxH, bool splatEligible,
                                              std::vector<SurfaceRuleGpu> surfaceRules,
                                              std::vector<SurfaceRuleConditionGpu> surfaceRuleConditions)
{
    if (dispatches.empty())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    // A bake writes the atlas height, normal and splat textures straight from a compute kernel
    // instead of through m_PendingUploads, and grass samples all three. The epoch is bumped where
    // the work is QUEUED so a reader consulting it during this frame's declare already sees it.
    m_GrassContentEpoch.fetch_add(1, std::memory_order_release);
    m_PendingBakes.push_back({handle, std::move(modifiers), std::move(dispatches), settle,
                              splatMinH, splatMaxH, splatEligible, std::move(surfaceRules),
                              std::move(surfaceRuleConditions)});
}

bool TerrainRenderFeature::HasPendingBakes() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return !m_PendingBakes.empty();
}

void TerrainRenderFeature::SeedPendingReadbackForTests(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    PendingReadback rb{};
    rb.Handle = handle; // invalid Buffer: the drain defer-destroys it, which no-ops on an invalid handle
    m_PendingReadbacks.push_back(rb);
}

uint32 TerrainRenderFeature::GetPendingReadbackCountForTests() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_PendingReadbacks.size());
}

size_t TerrainRenderFeature::GetUnifiedTextureLiveBytes() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_UnifiedTextureBytesLive;
}

size_t TerrainRenderFeature::GetUnifiedTexturePeakBytes() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_UnifiedTextureBytesPeak;
}

uint32 TerrainRenderFeature::GetReprovisionDrainFrameForTests() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_UnifiedReprovisionDrainFrame;
}

void TerrainRenderFeature::FailNextStagingAllocationForTests()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_FailNextStagingAllocation = true;
}

void TerrainRenderFeature::FlushPendingBakes(Rendering::CommandList* cl)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!cl || m_PendingBakes.empty())
        return;
    if (!EnsureGpuBakePipeline())
    {
        m_PendingBakes.clear();
        return;
    }

    const Rendering::DescriptorSetLayoutDesc layout = [] {
        Rendering::DescriptorSetLayoutDesc l;
        l.debugName = "TerrainHeightBake_DSLayout";
        l.bindings = {
            {0, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "ModifierBuffer"},
            {1, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasHeightStorage"},
        };
        return l;
    }();
    const Rendering::DescriptorSetLayoutDesc normalLayout = [] {
        Rendering::DescriptorSetLayoutDesc l;
        l.debugName = "TerrainNormalBake_DSLayout";
        l.bindings = {
            {0, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasHeightRead"},
            {1, Rendering::DescriptorType::StorageImage, 1, Rendering::kShaderStageCompute, "AtlasNormalStorage"},
        };
        return l;
    }();
    const Rendering::DescriptorSetLayoutDesc splatLayout = MakeSplatBakeLayoutDesc();

    for (auto& bake : m_PendingBakes)
    {
        if (bake.Handle.Index >= m_AtlasHeights.size())
            continue;
        const auto& atlas = m_AtlasHeights[bake.Handle.Index];
        if (!atlas.Texture.IsValid() || !atlas.StorageView.IsValid())
            continue;

        // Modifier SSBO: >= 1 element so the binding is never zero-sized, while the kernel is
        // told the authored count (set into the push by the extraction system, from the same
        // function) — see ModifierBindingSizes for why those must not be conflated.
        const ModifierBindingSizes modSizes = ComputeModifierBindingSizes(bake.Modifiers.size());
        const size_t modBytes = modSizes.ModifierBytes;
        Rendering::BufferHandle modBuf = m_Device->CreateUploadBuffer(modBytes, "TerrainHeightBake_Modifiers");
        if (!modBuf.IsValid())
            continue;
        if (modSizes.ModifiersArePadding)
        {
            ModifierGpu zero{};
            m_Device->UpdateBuffer(modBuf, 0, modBytes, &zero);
        }
        else
        {
            m_Device->UpdateBuffer(modBuf, 0, modBytes, bake.Modifiers.data());
        }

        Rendering::DescriptorSetDesc dsDesc{};
        dsDesc.layout = layout;
        dsDesc.transient = true;
        dsDesc.debugName = "TerrainHeightBake_DS";
        Rendering::DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, modBuf, 0, modBytes);
        m_Device->UpdateStorageImageBinding(ds, 1, atlas.StorageView);

        // Atlas height: resting ShaderResource -> UnorderedAccess (GENERAL) for imageStore.
        cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            atlas.Texture, Rendering::ResourceState::ShaderResource,
            Rendering::ResourceState::UnorderedAccess));

        cl->SetPipeline(m_GpuBakePipeline);
        cl->BindDescriptorSet(0, ds, m_GpuBakePipeline);
        for (const auto& disp : bake.Dispatches)
        {
            cl->SetPushConstants(disp.Push);
            const uint32 total = disp.Push.RectW * disp.Push.RectH;
            if (total == 0)
                continue;
            cl->Dispatch((total + 63u) / 64u, 1u, 1u);
        }

        // Slice-2 normal derive + slice-3 splat derive: both read the just-baked height (kept in
        // UnorderedAccess / GENERAL so they can imageLoad it) and write their own atlas (RG16F
        // normal, RGBA8 splat) so the mid-stroke lit surface has correct relief AND material
        // classification (the CPU normal/splat regen is skipped mid-stroke by 1c). ONE memory
        // barrier orders the height writes before their reads (an image dependency, not a CPU
        // fence); normal and splat are independent (both read height, write disjoint targets) so
        // they SHARE that post-height barrier. At settle the CPU re-derives both from the adopted
        // heights via MarkRegionDirty — no normal/splat readback.
        const bool doNormal = m_GpuNormalPipeline.IsValid() &&
                              bake.Handle.Index < m_AtlasNormals.size() &&
                              m_AtlasNormals[bake.Handle.Index].Texture.IsValid() &&
                              m_AtlasNormals[bake.Handle.Index].StorageView.IsValid();
        // Splat runs only when the bake is splat-eligible — when PackSurfaceRulesForGpuSplat could
        // express every splat writer in it. The pass overwrites the whole texel, so a bake carrying
        // a writer the kernel has no twin for keeps the CPU splat until settle: paint of any kind
        // (no mask sampler, no zone payload) and a spline-shaped rules volume (no SDF).
        // Also gated on the pipeline + storage view existing.
        const bool doSplat = m_GpuSplatPipeline.IsValid() && bake.SplatEligible &&
                             bake.Handle.Index < m_AtlasSplats.size() &&
                             m_AtlasSplats[bake.Handle.Index].Texture.IsValid() &&
                             m_AtlasSplats[bake.Handle.Index].StorageView.IsValid();

        if (doNormal || doSplat)
        {
            cl->Barrier(Rendering::ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
                static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
                static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite),
                static_cast<uint64>(Rendering::ResourceAccessMask::ShaderRead)));
        }

        if (doNormal)
        {
            const auto& normalEntry = m_AtlasNormals[bake.Handle.Index];
            cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
                normalEntry.Texture, Rendering::ResourceState::ShaderResource,
                Rendering::ResourceState::UnorderedAccess));

            Rendering::DescriptorSetDesc nds{};
            nds.layout = normalLayout;
            nds.transient = true;
            nds.debugName = "TerrainNormalBake_DS";
            Rendering::DescriptorSetHandle nset = m_Device->CreateDescriptorSet(nds);
            m_Device->UpdateStorageImageBinding(nset, 0, atlas.StorageView);       // height read
            m_Device->UpdateStorageImageBinding(nset, 1, normalEntry.StorageView); // normal write

            cl->SetPipeline(m_GpuNormalPipeline);
            cl->BindDescriptorSet(0, nset, m_GpuNormalPipeline);
            for (const auto& disp : bake.Dispatches)
            {
                cl->SetPushConstants(disp.Push);
                const uint32 total = disp.Push.RectW * disp.Push.RectH;
                if (total == 0)
                    continue;
                cl->Dispatch((total + 63u) / 64u, 1u, 1u);
            }
            cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
                normalEntry.Texture, Rendering::ResourceState::UnorderedAccess,
                Rendering::ResourceState::ShaderResource));
        }

        if (doSplat)
        {
            const auto& splatEntry = m_AtlasSplats[bake.Handle.Index];
            cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
                splatEntry.Texture, Rendering::ResourceState::ShaderResource,
                Rendering::ResourceState::UnorderedAccess));

            // Surface rule rows + their conditions. Both buffers are allocated with >= 1 element
            // so a rules-free bake still binds something (a zero-sized binding is invalid), and
            // the kernel reads neither when SurfaceRuleCount is 0.
            const SurfaceRuleBindingSizes sizes = ComputeSurfaceRuleBindingSizes(
                bake.SurfaceRules.size(), bake.SurfaceRuleConditions.size());
            const size_t ruleBytes = sizes.RuleBytes;
            const size_t condBytes = sizes.ConditionBytes;
            Rendering::BufferHandle ruleBuf =
                m_Device->CreateUploadBuffer(ruleBytes, "TerrainSplatBake_SurfaceRules");
            Rendering::BufferHandle condBuf =
                m_Device->CreateUploadBuffer(condBytes, "TerrainSplatBake_SurfaceRuleConditions");
            if (ruleBuf.IsValid() && condBuf.IsValid())
            {
                if (sizes.RulesArePadding)
                {
                    SurfaceRuleGpu zero{};
                    m_Device->UpdateBuffer(ruleBuf, 0, ruleBytes, &zero);
                }
                else
                {
                    m_Device->UpdateBuffer(ruleBuf, 0, ruleBytes, bake.SurfaceRules.data());
                }
                if (sizes.ConditionsArePadding)
                {
                    SurfaceRuleConditionGpu zero{};
                    m_Device->UpdateBuffer(condBuf, 0, condBytes, &zero);
                }
                else
                {
                    m_Device->UpdateBuffer(condBuf, 0, condBytes, bake.SurfaceRuleConditions.data());
                }

                Rendering::DescriptorSetDesc sds{};
                sds.layout = splatLayout;
                sds.transient = true;
                sds.debugName = "TerrainSplatBake_DS";
                Rendering::DescriptorSetHandle sset = m_Device->CreateDescriptorSet(sds);
                m_Device->UpdateStorageImageBinding(sset, 0, atlas.StorageView);      // height read
                m_Device->UpdateStorageImageBinding(sset, 1, splatEntry.StorageView); // splat write
                m_Device->UpdateStorageBufferBinding(sset, 2, ruleBuf, 0, ruleBytes);
                m_Device->UpdateStorageBufferBinding(sset, 3, condBuf, 0, condBytes);

                cl->SetPipeline(m_GpuSplatPipeline);
                cl->BindDescriptorSet(0, sset, m_GpuSplatPipeline);
                for (const auto& disp : bake.Dispatches)
                {
                    // The splat kernel needs the committed global range (altitude classification)
                    // and the rule row count; the rest of the push (rect + slot + geometry) is
                    // shared with the height/normal passes.
                    TerrainHeightBakePush sp = disp.Push;
                    sp.SplatMinH = bake.SplatMinH;
                    sp.SplatMaxH = bake.SplatMaxH;
                    // The AUTHORED count, never the padded element count — see
                    // SurfaceRuleBindingSizes for why those must not be conflated.
                    sp.SurfaceRuleCount = sizes.KernelRuleCount;
                    const uint32 total = sp.RectW * sp.RectH;
                    if (total == 0)
                        continue;
                    cl->SetPushConstants(sp);
                    cl->Dispatch((total + 63u) / 64u, 1u, 1u);
                }
            }
            DeferBufferDestroy(ruleBuf);
            DeferBufferDestroy(condBuf);

            cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
                splatEntry.Texture, Rendering::ResourceState::UnorderedAccess,
                Rendering::ResourceState::ShaderResource));
        }

        // Make the writes visible to CBT VertexEval's sampled read (binding 18) and to any
        // settle readback copy: UnorderedAccess -> ShaderResource.
        cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            atlas.Texture, Rendering::ResourceState::UnorderedAccess,
            Rendering::ResourceState::ShaderResource));

        DeferBufferDestroy(modBuf);

        if (bake.Settle)
        {
            for (const auto& disp : bake.Dispatches)
            {
                const TerrainHeightBakePush& p = disp.Push;
                if (p.RectW == 0 || p.RectH == 0 || p.SlotsPerRow == 0)
                    continue;
                const uint32 apron = (p.SlotStride - p.TileRes) / 2u;
                const uint32 slotCol = p.Slot % p.SlotsPerRow;
                const uint32 slotRow = p.Slot / p.SlotsPerRow;
                const uint32 atlasX0 = slotCol * p.SlotStride + apron + static_cast<uint32>(p.RectMinX);
                const uint32 atlasZ0 = slotRow * p.SlotStride + apron + static_cast<uint32>(p.RectMinZ);
                const size_t rbBytes = static_cast<size_t>(p.RectW) * p.RectH * sizeof(float32);
                Rendering::BufferHandle rbBuf =
                    m_Device->CreateReadbackBuffer(rbBytes, "TerrainHeightBake_Readback");
                if (!rbBuf.IsValid())
                    continue;
                cl->CopyTextureSubresourceToBuffer(atlas.Texture, 0, 0, rbBuf, p.RectW, p.RectH,
                                                   atlasX0, atlasZ0, 0, 0);
                PendingReadback rb{};
                rb.Buffer = rbBuf;
                rb.Handle = bake.Handle;
                rb.TileX = disp.TileX;
                rb.TileZ = disp.TileZ;
                rb.RectW = p.RectW;
                rb.RectH = p.RectH;
                rb.RectMinX = p.RectMinX;
                rb.RectMinZ = p.RectMinZ;
                rb.ReadyFrame = m_MonotonicFrame + kMaxFrames;
                m_PendingReadbacks.push_back(rb);
            }
        }
    }
    m_PendingBakes.clear();
}

void TerrainRenderFeature::DrainReadyHeightReadbacks(TerrainHandle handle, const ReadbackApply& cb)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_PendingReadbacks.empty())
        return;
    std::vector<float32> scratch;
    m_PendingReadbacks.erase(
        std::remove_if(m_PendingReadbacks.begin(), m_PendingReadbacks.end(),
            [&](PendingReadback& rb) {
                if (rb.Handle.Index != handle.Index || rb.Handle.Generation != handle.Generation)
                    return false;
                if (m_MonotonicFrame < rb.ReadyFrame)
                    return false;
                const size_t count = static_cast<size_t>(rb.RectW) * rb.RectH;
                if (const void* mapped = m_Device->MapBuffer(rb.Buffer))
                {
                    scratch.resize(count);
                    std::memcpy(scratch.data(), mapped, count * sizeof(float32));
                    m_Device->UnmapBuffer(rb.Buffer);
                    if (cb)
                        cb(rb.TileX, rb.TileZ, scratch.data(), rb.RectW, rb.RectH,
                           rb.RectMinX, rb.RectMinZ);
                }
                m_Device->DestroyBuffer(rb.Buffer);
                return true;
            }),
        m_PendingReadbacks.end());
}

} // namespace GameEngine::TerrainECS
