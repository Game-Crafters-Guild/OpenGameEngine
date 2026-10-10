#include "TerrainECS/TerrainHeightPageFeature.h"

#include "PageStreaming/PageStoreFormat.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{

namespace
{

constexpr std::size_t kPageBytes = static_cast<std::size_t>(PageStreaming::kPageSampleCount) * sizeof(float32);

} // namespace

TerrainHeightPageFeature::~TerrainHeightPageFeature()
{
    if (!m_Device)
        return;
    for (const RetiringBuffer& retiring : m_Retiring)
        m_Device->DestroyBuffer(retiring.Buffer);
    if (m_Staging.IsValid())
        m_Device->DestroyBuffer(m_Staging);
    if (m_Cache.IsValid())
        m_Device->DestroyTexture(m_Cache);
}

void TerrainHeightPageFeature::Publish(TerrainHeightPages& pages)
{
    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CacheDim = pages.Geometry().CacheDim;
    for (HeightPageUpload& upload : pages.Uploads())
        m_PublishedUploads.push_back(std::move(upload));
    pages.Uploads().clear();
    std::erase_if(m_PublishedTables, [&](const auto& entry) {
        return std::find(paged.begin(), paged.end(), entry.first) == paged.end();
    });
    for (uint32 terrain : paged)
    {
        uint64 version = 0;
        const std::vector<uint32>* words = pages.TableWords(terrain, version);
        Table& table = m_PublishedTables[terrain];
        if (words == nullptr || (table.Version == version && table.Words))
            continue;
        table.Words = std::make_shared<const std::vector<uint32>>(*words);
        table.Version = version;
    }
}

void TerrainHeightPageFeature::Latch(Rendering::IDevice& device, uint64 rgFrame)
{
    if (rgFrame == m_LatchedFrame)
        return;
    m_LatchedFrame = rgFrame;
    m_Device = &device;
    m_CacheReadable = m_Cache.IsValid() && m_CacheCleared;

    const uint64 retireAfter = static_cast<uint64>(device.GetFramesInFlight()) + 1u;
    std::erase_if(m_Retiring, [&](const RetiringBuffer& retiring) {
        if (rgFrame < retiring.Frame + retireAfter)
            return false;
        device.DestroyBuffer(retiring.Buffer);
        return true;
    });

    bool newUploads = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        newUploads = !m_PublishedUploads.empty();
        for (HeightPageUpload& upload : m_PublishedUploads)
            m_LatchedUploads.push_back(std::move(upload));
        m_PublishedUploads.clear();
        m_LatchedTables = m_PublishedTables; // one shared pointer per paged terrain
        m_LatchedCacheDim = m_CacheDim;
    }
    if (m_LatchedUploads.empty())
    {
        m_CommittedTables = m_LatchedTables;
        return;
    }
    if (newUploads || !m_Staging.IsValid())
        StageUploads();
}

void TerrainHeightPageFeature::StageUploads()
{
    RetireStaging();
    EnsureCache();
    if (!m_Cache.IsValid())
        return;
    const std::size_t bytes = m_LatchedUploads.size() * kPageBytes;
    m_Staging = m_Device->CreateUploadBuffer(bytes, "Terrain.HeightPageStaging");
    if (!m_Staging.IsValid())
    {
        Logger::Log::Warning("Terrain pages: no staging for {} page uploads ({} bytes); the page tables keep their "
                             "last uploaded state and the uploads retry next frame",
                             m_LatchedUploads.size(), bytes);
        return;
    }
    for (std::size_t i = 0; i < m_LatchedUploads.size(); ++i)
        m_Device->UpdateBuffer(m_Staging, i * kPageBytes, kPageBytes, m_LatchedUploads[i].Samples.data());
}

void TerrainHeightPageFeature::RetireStaging()
{
    if (m_Staging.IsValid())
        m_Retiring.push_back({m_Staging, m_LatchedFrame});
    m_Staging = {};
}

void TerrainHeightPageFeature::CommitLatch()
{
    if (m_Staging.IsValid())
        m_CommittedTables = m_LatchedTables;
}

void TerrainHeightPageFeature::EnsureCache()
{
    if (m_Cache.IsValid() || m_LatchedCacheDim == 0)
        return;
    Rendering::TextureDesc desc{};
    desc.width = m_LatchedCacheDim;
    desc.height = m_LatchedCacheDim;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R32_FLOAT);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                 static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    desc.persistent = true;
    desc.debugName = "Terrain.HeightPageCache";
    m_Cache = m_Device->CreateTexture(desc);
    m_CacheCleared = false;
    m_CacheReadable = false;
}

void TerrainHeightPageFeature::FlushUploads(Rendering::CommandList& cl)
{
    if (!m_Staging.IsValid() || !m_Cache.IsValid())
        return;
    Rendering::ResourceState resting = Rendering::ResourceState::ShaderResource;
    if (!m_CacheCleared)
    {
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cl.ClearColorImageSubresource(m_Cache, 0, 0, zero);
        resting = Rendering::ResourceState::CopyDest;
        m_CacheCleared = true;
    }

    const uint32 slotsPerRow = m_LatchedCacheDim / PageStreaming::kPageStrideSamples;
    for (std::size_t i = 0; i < m_LatchedUploads.size(); ++i)
    {
        const uint32 slot = m_LatchedUploads[i].Slot;
        cl.CopyBufferToTextureSubresource(m_Staging, m_Cache, 0, 0, PageStreaming::kPageStrideSamples,
                                          PageStreaming::kPageStrideSamples, i * kPageBytes,
                                          PageStreaming::kPageStrideSamples * sizeof(float32), 1, 0,
                                          (slot % slotsPerRow) * PageStreaming::kPageStrideSamples,
                                          (slot / slotsPerRow) * PageStreaming::kPageStrideSamples, resting);
        resting = Rendering::ResourceState::CopyDest;
    }
    cl.Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(m_Cache, Rendering::ResourceState::CopyDest,
                                                                Rendering::ResourceState::ShaderResource));
    RetireStaging();
    m_LatchedUploads.clear();
}

bool TerrainHeightPageFeature::LatchedTable(uint32 terrain, std::span<const uint32>& words, uint64& version) const
{
    const auto it = m_CommittedTables.find(terrain);
    if (it == m_CommittedTables.end() || !it->second.Words || it->second.Words->empty())
        return false;
    words = *it->second.Words;
    version = it->second.Version;
    return true;
}

void TerrainHeightPageFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    // Every handle died with the old device: forget them without destroying them.
    m_Device = device;
    m_Cache = {};
    m_CacheCleared = false;
    m_CacheReadable = false;
    m_Staging = {};
    m_Retiring.clear();
    m_LatchedUploads.clear();
    m_LatchedTables.clear();
    m_CommittedTables.clear();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PublishedUploads.clear();
    }
    m_DeviceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace GameEngine::TerrainECS
