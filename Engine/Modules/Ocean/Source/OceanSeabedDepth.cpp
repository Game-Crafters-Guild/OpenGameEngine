#include "Ocean/OceanSeabedDepth.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{

constexpr uint32 kMaxDepthCacheAtlasResolution = 8192u;

TextureHandle CreateDepthCacheTexture(IDevice* device, const float32* depths,
                                      uint32 width, uint32 height, const char* debugName)
{
    if (!device || !depths || width == 0 || height == 0)
        return {};

    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32>(TextureUsage::ShaderResource) |
               static_cast<uint32>(TextureUsage::TransferDst);
    td.sampleCount = 1;
    td.initialState = ResourceState::Undefined;
    td.debugName = debugName;

    TextureHandle texture = device->CreateTexture(td);
    if (!texture.IsValid())
        return {};

    UploadTexture2D(device, texture, depths, width, height, sizeof(float32) * width,
                    "Ocean_DepthCache_Upload");
    return texture;
}

struct DepthCacheAtlasPage
{
    uint32 CacheIndex = 0;
    uint32 X = 0;
    uint32 Y = 0;
    uint32 Width = 0;
    uint32 Height = 0;
};

bool PackDepthCacheAtlas(const OceanDepthCacheAsset* caches, uint32 count,
                         std::vector<DepthCacheAtlasPage>& outPages,
                         uint32& outWidth, uint32& outHeight)
{
    outPages.clear();
    outWidth = 0;
    outHeight = 0;
    if (!caches || count == 0 || count > kMaxOceanSavedDepthCachePages)
        return false;

    uint64 totalArea = 0;
    uint32 maxWidth = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        if (!caches[i].IsValid())
            continue;
        const auto& desc = caches[i].GetDesc();
        if (desc.Width > kMaxDepthCacheAtlasResolution ||
            desc.Height > kMaxDepthCacheAtlasResolution)
        {
            return false;
        }
        totalArea += static_cast<uint64>(desc.Width) * static_cast<uint64>(desc.Height);
        maxWidth = std::max(maxWidth, desc.Width);
    }
    if (totalArea == 0 || maxWidth == 0)
        return false;

    const uint32 targetWidth = std::min<uint32>(
        kMaxDepthCacheAtlasResolution,
        std::max<uint32>(maxWidth, static_cast<uint32>(std::ceil(std::sqrt(double(totalArea))))));

    uint32 cursorX = 0;
    uint32 cursorY = 0;
    uint32 rowHeight = 0;
    uint32 atlasWidth = 0;
    outPages.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        if (!caches[i].IsValid())
            continue;
        const auto& desc = caches[i].GetDesc();
        if (cursorX != 0 && cursorX + desc.Width > targetWidth)
        {
            cursorX = 0;
            cursorY += rowHeight;
            rowHeight = 0;
        }

        DepthCacheAtlasPage page{};
        page.CacheIndex = i;
        page.X = cursorX;
        page.Y = cursorY;
        page.Width = desc.Width;
        page.Height = desc.Height;
        outPages.push_back(page);

        cursorX += desc.Width;
        rowHeight = std::max(rowHeight, desc.Height);
        atlasWidth = std::max(atlasWidth, cursorX);
    }

    outWidth = std::max(atlasWidth, 1u);
    outHeight = std::max(cursorY + rowHeight, 1u);
    return outWidth <= kMaxDepthCacheAtlasResolution && outHeight <= kMaxDepthCacheAtlasResolution;
}

} // anonymous namespace

bool OceanSeabedDepth::Initialize(IDevice* device)
{
    // The cascade bakes into a narrow storage format, which this backend's
    // storage-image list does not include; the texture would be refused at
    // creation and its invalid handle would poison every submit that binds
    // it. Declining here keeps the surface: the caller already degrades.
    if (device && !device->GetCapabilities().supportsNarrowStorageFormats)
        return false;
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // R16F (single channel) seabed-depth cascade. Single persistent array (the
    // bake is stateless, no ping-pong).
    if (!m_Depth.Initialize(device, kDepthResolution, kDepthLodCount, TextureFormat::R16_FLOAT,
                            kDepthBaseScale, "Ocean_SeabedDepth"))
    {
        Logger::Log::Warning("OceanSeabedDepth: depth cascade allocation failed");
        return false;
    }
    m_Resolution = m_Depth.GetResolution();
    m_LodCount = m_Depth.GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_SeabedDepth_Sampler"));
    if (!m_Sampler.IsValid())
        return false;
    m_SavedDepthSampler =
        device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_SavedDepthCache_Sampler"));
    if (!m_SavedDepthSampler.IsValid())
        return false;

    const float32 deep = 60000.0f;
    m_DummySavedDepthTexture =
        CreateDepthCacheTexture(device, &deep, 1, 1, "Ocean_SavedDepthCache_Dummy");
    if (!m_DummySavedDepthTexture.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_seafloor_depth.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanSeabedDepth: ocean_seafloor_depth.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_seafloor_depth";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_seafloor_depth.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanSeabedDepth: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanSeabedDepth: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), output-depth storage image(1), saved depth cache sampler(2),
    // dynamic raster water-depth capture sampler(3).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanSeabedDepth_Set0";
    auto addBinding = [&](uint32 binding, DescriptorType type) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = type;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        m_Layout.bindings.push_back(b);
    };
    addBinding(0, DescriptorType::UniformBuffer);
    addBinding(1, DescriptorType::StorageImage);
    addBinding(2, DescriptorType::CombinedImageSampler);
    addBinding(3, DescriptorType::CombinedImageSampler);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanSeabedDepth";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanSeabedDepth: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution,
                      m_LodCount);
    return true;
}

bool OceanSeabedDepth::BeginFrame(float cameraX, float cameraZ)
{
    const bool snapped = m_Depth.SnapToCamera(cameraX, cameraZ);
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    if (snapped)
        m_DispatchDirty = true;
    return m_DispatchDirty;
}

bool OceanSeabedDepth::NeedsDispatch() const
{
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    return m_DispatchDirty;
}

bool OceanSeabedDepth::HasSeabeds() const
{
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    return m_HasSpline || m_Params.SeabedCount > 0 || m_Params.DepthContributorCount > 0 ||
           m_HasSavedDepthCache || m_HasRasterDepthCapture;
}

void OceanSeabedDepth::SetSeabeds(const OceanSeabedGPU* seabeds, uint32 count,
                                  const OceanDepthContributorGPU* contributors,
                                  uint32 contributorCount, float seaLevel, float depthBandSaturation)
{
    const uint32 n = seabeds ? std::min(count, kMaxOceanSeabeds) : 0u;
    const uint32 c = contributors ? std::min(contributorCount, kMaxOceanDepthContributors) : 0u;
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    const bool changed =
        m_Params.SeaLevel != seaLevel ||
        m_Params.DepthBandSaturation != depthBandSaturation ||
        m_Params.SeabedCount != n ||
        m_Params.DepthContributorCount != c ||
        (n > 0 && std::memcmp(m_Params.Seabeds, seabeds,
                              static_cast<size_t>(n) * sizeof(OceanSeabedGPU)) != 0) ||
        (c > 0 && std::memcmp(m_Params.DepthContributors, contributors,
                              static_cast<size_t>(c) * sizeof(OceanDepthContributorGPU)) != 0);
    m_Params.SeabedCount = n;
    m_Params.DepthContributorCount = c;
    m_Params.SeaLevel = seaLevel;
    m_Params.DepthBandSaturation = depthBandSaturation;
    if (seabeds && n > 0)
        std::memcpy(m_Params.Seabeds, seabeds, static_cast<size_t>(n) * sizeof(OceanSeabedGPU));
    if (contributors && c > 0)
    {
        std::memcpy(m_Params.DepthContributors, contributors,
                    static_cast<size_t>(c) * sizeof(OceanDepthContributorGPU));
    }
    if (changed)
        m_DispatchDirty = true;
}

void OceanSeabedDepth::ApplySavedDepthCacheParamsLocked()
{
    if (!m_HasSavedDepthCache || !m_SavedDepthCache.IsValid())
    {
        m_Params.SavedDepthCacheAvailable = 0;
        m_Params.SavedDepthCachePageCount = 0;
        m_Params.SavedDepthCacheOriginSize[0] = 0.0f;
        m_Params.SavedDepthCacheOriginSize[1] = 0.0f;
        m_Params.SavedDepthCacheOriginSize[2] = 1.0f;
        m_Params.SavedDepthCacheOriginSize[3] = 1.0f;
        m_Params.SavedDepthCacheInvSizeDeep[0] = 1.0f;
        m_Params.SavedDepthCacheInvSizeDeep[1] = 1.0f;
        m_Params.SavedDepthCacheInvSizeDeep[2] = 60000.0f;
        m_Params.SavedDepthCacheInvSizeDeep[3] = 0.0f;
        for (auto& page : m_Params.SavedDepthCachePages)
            page = OceanSavedDepthCachePageGPU{};
        return;
    }

    const OceanDepthCacheAssetDesc& desc = m_SavedDepthCache.GetDesc();
    m_Params.SavedDepthCacheAvailable = 1;
    if (m_Params.SavedDepthCachePageCount > kMaxOceanSavedDepthCachePages)
        m_Params.SavedDepthCachePageCount = kMaxOceanSavedDepthCachePages;
    m_Params.SavedDepthCacheOriginSize[0] = desc.OriginX;
    m_Params.SavedDepthCacheOriginSize[1] = desc.OriginZ;
    m_Params.SavedDepthCacheOriginSize[2] = desc.SizeX;
    m_Params.SavedDepthCacheOriginSize[3] = desc.SizeZ;
    m_Params.SavedDepthCacheInvSizeDeep[0] = desc.SizeX > 0.0f ? 1.0f / desc.SizeX : 1.0f;
    m_Params.SavedDepthCacheInvSizeDeep[1] = desc.SizeZ > 0.0f ? 1.0f / desc.SizeZ : 1.0f;
    m_Params.SavedDepthCacheInvSizeDeep[2] = desc.DeepWaterDepth;
    m_Params.SavedDepthCacheInvSizeDeep[3] = 0.0f;
}

bool OceanSeabedDepth::UploadSavedDepthCacheTextureLocked(const OceanDepthCacheAsset& cache)
{
    if (!m_Device)
        return true;
    if (!cache.IsValid())
        return false;

    const OceanDepthCacheAssetDesc& desc = cache.GetDesc();
    TextureHandle texture = CreateDepthCacheTexture(m_Device, cache.GetDepths().data(),
                                                    desc.Width, desc.Height,
                                                    "Ocean_SavedDepthCache");
    if (!texture.IsValid())
        return false;

    if (m_SavedDepthTexture.IsValid())
        m_Device->DestroyTexture(m_SavedDepthTexture);
    m_SavedDepthTexture = texture;
    m_SavedDepthTextureWidth = desc.Width;
    m_SavedDepthTextureHeight = desc.Height;
    m_Params.SavedDepthCachePageCount = 0;
    for (auto& page : m_Params.SavedDepthCachePages)
        page = OceanSavedDepthCachePageGPU{};
    return true;
}

bool OceanSeabedDepth::UploadSavedDepthCacheAtlasLocked(const OceanDepthCacheAsset* caches,
                                                        uint32 count)
{
    if (!m_Device)
        return true;
    if (!caches || count == 0 || count > kMaxOceanSavedDepthCachePages)
        return false;

    std::vector<DepthCacheAtlasPage> pages;
    uint32 atlasWidth = 0;
    uint32 atlasHeight = 0;
    if (!PackDepthCacheAtlas(caches, count, pages, atlasWidth, atlasHeight))
        return false;
    if (pages.empty())
        return false;

    float32 deep = 60000.0f;
    for (const DepthCacheAtlasPage& page : pages)
        deep = std::max(deep, caches[page.CacheIndex].GetDesc().DeepWaterDepth);

    std::vector<float32> atlas(static_cast<size_t>(atlasWidth) * atlasHeight, deep);
    for (const DepthCacheAtlasPage& page : pages)
    {
        const auto& desc = caches[page.CacheIndex].GetDesc();
        const auto& depths = caches[page.CacheIndex].GetDepths();
        for (uint32 y = 0; y < page.Height; ++y)
        {
            const size_t dst = (static_cast<size_t>(page.Y + y) * atlasWidth) + page.X;
            const size_t src = static_cast<size_t>(y) * desc.Width;
            std::memcpy(atlas.data() + dst, depths.data() + src,
                        static_cast<size_t>(page.Width) * sizeof(float32));
        }
    }

    TextureHandle texture = CreateDepthCacheTexture(m_Device, atlas.data(), atlasWidth, atlasHeight,
                                                    "Ocean_SavedDepthCache_Atlas");
    if (!texture.IsValid())
        return false;

    if (m_SavedDepthTexture.IsValid())
        m_Device->DestroyTexture(m_SavedDepthTexture);
    m_SavedDepthTexture = texture;
    m_SavedDepthTextureWidth = atlasWidth;
    m_SavedDepthTextureHeight = atlasHeight;

    m_Params.SavedDepthCachePageCount = std::min<uint32>(
        static_cast<uint32>(pages.size()), kMaxOceanSavedDepthCachePages);
    for (auto& gpuPage : m_Params.SavedDepthCachePages)
        gpuPage = OceanSavedDepthCachePageGPU{};
    for (uint32 i = 0; i < m_Params.SavedDepthCachePageCount; ++i)
    {
        const DepthCacheAtlasPage& page = pages[i];
        const auto& desc = caches[page.CacheIndex].GetDesc();
        OceanSavedDepthCachePageGPU& gpuPage = m_Params.SavedDepthCachePages[i];
        gpuPage.OriginSize[0] = desc.OriginX;
        gpuPage.OriginSize[1] = desc.OriginZ;
        gpuPage.OriginSize[2] = desc.SizeX;
        gpuPage.OriginSize[3] = desc.SizeZ;
        gpuPage.InvSizeDeep[0] = desc.SizeX > 0.0f ? 1.0f / desc.SizeX : 1.0f;
        gpuPage.InvSizeDeep[1] = desc.SizeZ > 0.0f ? 1.0f / desc.SizeZ : 1.0f;
        gpuPage.InvSizeDeep[2] = desc.DeepWaterDepth;
        gpuPage.InvSizeDeep[3] = 0.0f;
        gpuPage.AtlasRect[0] = static_cast<float32>(page.X) / static_cast<float32>(atlasWidth);
        gpuPage.AtlasRect[1] = static_cast<float32>(page.Y) / static_cast<float32>(atlasHeight);
        gpuPage.AtlasRect[2] = static_cast<float32>(page.Width) / static_cast<float32>(atlasWidth);
        gpuPage.AtlasRect[3] = static_cast<float32>(page.Height) / static_cast<float32>(atlasHeight);
    }
    return true;
}

bool OceanSeabedDepth::SetSavedDepthCache(const OceanDepthCacheAsset& cache)
{
    if (!cache.IsValid())
        return false;

    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    if (!UploadSavedDepthCacheTextureLocked(cache))
    {
        m_HasSavedDepthCache = false;
        m_SavedDepthCaches.clear();
        m_SavedDepthCachePaths.clear();
        m_FailedSavedDepthCachePaths.clear();
        m_SavedDepthCacheWriteTimes.clear();
        m_FailedSavedDepthCacheWriteTimes.clear();
        m_SavedDepthCacheSourceRevisions.clear();
        m_SavedDepthCacheBakedRevisions.clear();
        m_SavedDepthCacheRevisions.clear();
        m_FailedSavedDepthCacheSourceRevisions.clear();
        m_FailedSavedDepthCacheBakedRevisions.clear();
        m_FailedSavedDepthCacheRevisions.clear();
        ApplySavedDepthCacheParamsLocked();
        m_DispatchDirty = true;
        return false;
    }

    m_SavedDepthCache = cache;
    m_SavedDepthCaches.clear();
    m_SavedDepthCaches.push_back(cache);
    m_SavedDepthCachePaths.clear();
    m_FailedSavedDepthCachePaths.clear();
    m_SavedDepthCacheWriteTimes.clear();
    m_FailedSavedDepthCacheWriteTimes.clear();
    m_SavedDepthCacheSourceRevisions.clear();
    m_SavedDepthCacheBakedRevisions.clear();
    m_SavedDepthCacheRevisions.clear();
    m_FailedSavedDepthCacheSourceRevisions.clear();
    m_FailedSavedDepthCacheBakedRevisions.clear();
    m_FailedSavedDepthCacheRevisions.clear();
    m_HasSavedDepthCache = true;
    ApplySavedDepthCacheParamsLocked();
    m_DispatchDirty = true;
    return true;
}

bool OceanSeabedDepth::SetSavedDepthCaches(const OceanDepthCacheAsset* caches, uint32 count)
{
    if (!caches || count == 0)
        return false;

    std::vector<OceanDepthCacheAsset> validCaches;
    validCaches.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        if (caches[i].IsValid())
            validCaches.push_back(caches[i]);
    }
    if (validCaches.empty())
        return false;

    if (validCaches.size() <= kMaxOceanSavedDepthCachePages)
    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        if (!UploadSavedDepthCacheAtlasLocked(validCaches.data(), static_cast<uint32>(validCaches.size())))
        {
            ClearSavedDepthCacheLocked();
            return false;
        }

        m_SavedDepthCache = validCaches.front();
        m_SavedDepthCaches = std::move(validCaches);
        m_SavedDepthCachePaths.clear();
        m_FailedSavedDepthCachePaths.clear();
        m_SavedDepthCacheWriteTimes.clear();
        m_FailedSavedDepthCacheWriteTimes.clear();
        m_SavedDepthCacheSourceRevisions.clear();
        m_SavedDepthCacheBakedRevisions.clear();
        m_SavedDepthCacheRevisions.clear();
        m_FailedSavedDepthCacheSourceRevisions.clear();
        m_FailedSavedDepthCacheBakedRevisions.clear();
        m_FailedSavedDepthCacheRevisions.clear();
        m_HasSavedDepthCache = true;
        ApplySavedDepthCacheParamsLocked();
        m_DispatchDirty = true;
        return true;
    }

    OceanDepthCacheAsset composed;
    if (!ComposeOceanDepthCaches(validCaches.data(), static_cast<uint32>(validCaches.size()), composed))
        return false;
    if (!SetSavedDepthCache(composed))
        return false;
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    m_SavedDepthCaches.clear();
    return true;
}

bool OceanSeabedDepth::LoadSavedDepthCacheFromFile(const std::filesystem::path& path)
{
    OceanSavedDepthCacheSource source{};
    source.Path = path;
    return LoadSavedDepthCaches(&source, 1);
}

bool OceanSeabedDepth::LoadSavedDepthCachesFromFiles(const std::filesystem::path* paths, uint32 count)
{
    if (!paths || count == 0)
    {
        ClearSavedDepthCache();
        return false;
    }

    std::vector<OceanSavedDepthCacheSource> sources;
    sources.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        OceanSavedDepthCacheSource source{};
        source.Path = paths[i];
        sources.push_back(std::move(source));
    }
    return LoadSavedDepthCaches(sources.data(), static_cast<uint32>(sources.size()));
}

bool OceanSeabedDepth::LoadSavedDepthCaches(const OceanSavedDepthCacheSource* sources, uint32 count)
{
    if (!sources || count == 0)
    {
        ClearSavedDepthCache();
        return false;
    }

    std::vector<fs::path> filteredPaths;
    std::vector<uint32> sourceRevisions;
    std::vector<uint32> bakedRevisions;
    std::vector<uint32> cacheRevisions;
    filteredPaths.reserve(count);
    sourceRevisions.reserve(count);
    bakedRevisions.reserve(count);
    cacheRevisions.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        if (sources[i].Path.empty())
            continue;
        if (!sources[i].UseWhenStale && sources[i].SourceRevision != sources[i].BakedRevision)
            continue;
        filteredPaths.push_back(sources[i].Path);
        sourceRevisions.push_back(sources[i].SourceRevision);
        bakedRevisions.push_back(sources[i].BakedRevision);
        cacheRevisions.push_back(sources[i].CacheRevision);
    }
    if (filteredPaths.empty())
    {
        ClearSavedDepthCache();
        return false;
    }

    std::vector<fs::file_time_type> writeTimes;
    writeTimes.reserve(filteredPaths.size());
    bool allWriteTimesValid = true;
    for (const fs::path& p : filteredPaths)
    {
        std::error_code timeEc;
        fs::file_time_type writeTime = fs::last_write_time(p, timeEc);
        if (timeEc)
        {
            writeTime = {};
            allWriteTimesValid = false;
        }
        writeTimes.push_back(writeTime);
    }

    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        if (m_HasSavedDepthCache && filteredPaths == m_SavedDepthCachePaths &&
            allWriteTimesValid && writeTimes == m_SavedDepthCacheWriteTimes &&
            sourceRevisions == m_SavedDepthCacheSourceRevisions &&
            bakedRevisions == m_SavedDepthCacheBakedRevisions &&
            cacheRevisions == m_SavedDepthCacheRevisions)
            return true;
        if (!m_FailedSavedDepthCachePaths.empty() && filteredPaths == m_FailedSavedDepthCachePaths &&
            writeTimes == m_FailedSavedDepthCacheWriteTimes &&
            sourceRevisions == m_FailedSavedDepthCacheSourceRevisions &&
            bakedRevisions == m_FailedSavedDepthCacheBakedRevisions &&
            cacheRevisions == m_FailedSavedDepthCacheRevisions)
            return false;
    }

    std::vector<OceanDepthCacheAsset> caches;
    caches.reserve(filteredPaths.size());
    for (const fs::path& p : filteredPaths)
    {
        OceanDepthCacheAsset cache;
        std::string error;
        if (!cache.LoadBinary(p, &error))
        {
            std::lock_guard<std::mutex> lock(m_ParamsMutex);
            if (filteredPaths == m_SavedDepthCachePaths)
                ClearSavedDepthCacheLocked();
            m_FailedSavedDepthCachePaths = filteredPaths;
            m_FailedSavedDepthCacheWriteTimes = writeTimes;
            m_FailedSavedDepthCacheSourceRevisions = sourceRevisions;
            m_FailedSavedDepthCacheBakedRevisions = bakedRevisions;
            m_FailedSavedDepthCacheRevisions = cacheRevisions;
            Logger::Log::Warning("OceanSeabedDepth: failed to load saved depth cache '{}': {}",
                                 p.string(), error);
            return false;
        }
        caches.push_back(std::move(cache));
    }

    if (!SetSavedDepthCaches(caches.data(), static_cast<uint32>(caches.size())))
    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        if (filteredPaths == m_SavedDepthCachePaths)
            ClearSavedDepthCacheLocked();
        m_FailedSavedDepthCachePaths = filteredPaths;
        m_FailedSavedDepthCacheWriteTimes = writeTimes;
        m_FailedSavedDepthCacheSourceRevisions = sourceRevisions;
        m_FailedSavedDepthCacheBakedRevisions = bakedRevisions;
        m_FailedSavedDepthCacheRevisions = cacheRevisions;
        Logger::Log::Warning("OceanSeabedDepth: failed to upload saved depth cache set");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    m_SavedDepthCachePaths = std::move(filteredPaths);
    m_SavedDepthCacheWriteTimes = std::move(writeTimes);
    m_SavedDepthCacheSourceRevisions = std::move(sourceRevisions);
    m_SavedDepthCacheBakedRevisions = std::move(bakedRevisions);
    m_SavedDepthCacheRevisions = std::move(cacheRevisions);
    m_FailedSavedDepthCachePaths.clear();
    m_FailedSavedDepthCacheWriteTimes.clear();
    m_FailedSavedDepthCacheSourceRevisions.clear();
    m_FailedSavedDepthCacheBakedRevisions.clear();
    m_FailedSavedDepthCacheRevisions.clear();
    return true;
}

void OceanSeabedDepth::ClearSavedDepthCacheLocked()
{
    const bool hadSavedDepth = m_HasSavedDepthCache || m_SavedDepthTexture.IsValid();
    m_SavedDepthCache = {};
    m_SavedDepthCaches.clear();
    m_SavedDepthCachePaths.clear();
    m_FailedSavedDepthCachePaths.clear();
    m_SavedDepthCacheWriteTimes.clear();
    m_FailedSavedDepthCacheWriteTimes.clear();
    m_SavedDepthCacheSourceRevisions.clear();
    m_SavedDepthCacheBakedRevisions.clear();
    m_SavedDepthCacheRevisions.clear();
    m_FailedSavedDepthCacheSourceRevisions.clear();
    m_FailedSavedDepthCacheBakedRevisions.clear();
    m_FailedSavedDepthCacheRevisions.clear();
    m_HasSavedDepthCache = false;
    ApplySavedDepthCacheParamsLocked();
    if (hadSavedDepth)
        m_DispatchDirty = true;
    if (m_Device && m_SavedDepthTexture.IsValid())
    {
        m_Device->DestroyTexture(m_SavedDepthTexture);
        m_SavedDepthTexture = {};
        m_SavedDepthTextureWidth = 0;
        m_SavedDepthTextureHeight = 0;
    }
}

void OceanSeabedDepth::ClearSavedDepthCache()
{
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    ClearSavedDepthCacheLocked();
}

void OceanSeabedDepth::SetRasterDepthCapture(TextureHandle texture, SamplerHandle sampler,
                                             float originX, float originZ, float sizeX,
                                             float sizeZ, float deepWaterDepth)
{
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    const bool wasAvailable = m_HasRasterDepthCapture;
    const TextureHandle oldTexture = m_RasterDepthCaptureTexture;
    const SamplerHandle oldSampler = m_RasterDepthCaptureSampler;
    const float oldOriginX = m_Params.RasterDepthCaptureOriginSize[0];
    const float oldOriginZ = m_Params.RasterDepthCaptureOriginSize[1];
    const float oldSizeX = m_Params.RasterDepthCaptureOriginSize[2];
    const float oldSizeZ = m_Params.RasterDepthCaptureOriginSize[3];
    const float oldDeep = m_Params.RasterDepthCaptureInvSizeDeep[2];
    if (!texture.IsValid() || sizeX <= 0.0f || sizeZ <= 0.0f)
    {
        m_RasterDepthCaptureTexture = {};
        m_RasterDepthCaptureSampler = {};
        m_HasRasterDepthCapture = false;
        m_Params.RasterDepthCaptureAvailable = 0;
        m_Params.RasterDepthCaptureOriginSize[0] = 0.0f;
        m_Params.RasterDepthCaptureOriginSize[1] = 0.0f;
        m_Params.RasterDepthCaptureOriginSize[2] = 1.0f;
        m_Params.RasterDepthCaptureOriginSize[3] = 1.0f;
        m_Params.RasterDepthCaptureInvSizeDeep[0] = 1.0f;
        m_Params.RasterDepthCaptureInvSizeDeep[1] = 1.0f;
        m_Params.RasterDepthCaptureInvSizeDeep[2] = 60000.0f;
        m_Params.RasterDepthCaptureInvSizeDeep[3] = 0.0f;
        if (wasAvailable)
            m_DispatchDirty = true;
        return;
    }

    const float32 deep = deepWaterDepth > 0.0f ? deepWaterDepth : 60000.0f;
    m_RasterDepthCaptureTexture = texture;
    m_RasterDepthCaptureSampler = sampler;
    m_HasRasterDepthCapture = true;
    m_Params.RasterDepthCaptureAvailable = 1;
    m_Params.RasterDepthCaptureOriginSize[0] = originX;
    m_Params.RasterDepthCaptureOriginSize[1] = originZ;
    m_Params.RasterDepthCaptureOriginSize[2] = sizeX;
    m_Params.RasterDepthCaptureOriginSize[3] = sizeZ;
    m_Params.RasterDepthCaptureInvSizeDeep[0] = 1.0f / sizeX;
    m_Params.RasterDepthCaptureInvSizeDeep[1] = 1.0f / sizeZ;
    m_Params.RasterDepthCaptureInvSizeDeep[2] = deep;
    m_Params.RasterDepthCaptureInvSizeDeep[3] = 0.0f;
    if (!wasAvailable || oldTexture != texture || oldSampler != sampler ||
        oldOriginX != originX || oldOriginZ != originZ ||
        oldSizeX != sizeX || oldSizeZ != sizeZ || oldDeep != deep)
    {
        m_DispatchDirty = true;
    }
}

void OceanSeabedDepth::ClearRasterDepthCapture()
{
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    const bool wasAvailable = m_HasRasterDepthCapture;
    m_RasterDepthCaptureTexture = {};
    m_RasterDepthCaptureSampler = {};
    m_HasRasterDepthCapture = false;
    m_Params.RasterDepthCaptureAvailable = 0;
    m_Params.RasterDepthCaptureOriginSize[0] = 0.0f;
    m_Params.RasterDepthCaptureOriginSize[1] = 0.0f;
    m_Params.RasterDepthCaptureOriginSize[2] = 1.0f;
    m_Params.RasterDepthCaptureOriginSize[3] = 1.0f;
    m_Params.RasterDepthCaptureInvSizeDeep[0] = 1.0f;
    m_Params.RasterDepthCaptureInvSizeDeep[1] = 1.0f;
    m_Params.RasterDepthCaptureInvSizeDeep[2] = 60000.0f;
    m_Params.RasterDepthCaptureInvSizeDeep[3] = 0.0f;
    if (wasAvailable)
        m_DispatchDirty = true;
}

bool OceanSeabedDepth::SampleDepth(float worldX, float worldZ, float& outDepth) const
{
    OceanSeabedDepthParamsGPU params{};
    std::vector<OceanDepthCacheAsset> savedCaches;
    OceanDepthCacheAsset savedCache;
    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        params = m_Params;
        if (m_HasSavedDepthCache)
        {
            savedCaches = m_SavedDepthCaches;
            savedCache = m_SavedDepthCache;
        }
    }

    constexpr float32 kDeepWater = 60000.0f;
    float32 depth = kDeepWater;
    if (!savedCaches.empty())
    {
        for (const OceanDepthCacheAsset& cache : savedCaches)
        {
            float32 savedDepth = 0.0f;
            if (cache.SampleDepth(worldX, worldZ, savedDepth))
                depth = std::min(depth, std::clamp(savedDepth, 0.0f,
                                                   cache.GetDesc().DeepWaterDepth));
        }
    }
    else
    {
        float32 savedDepth = 0.0f;
        if (savedCache.SampleDepth(worldX, worldZ, savedDepth))
            depth = std::min(depth, std::clamp(savedDepth, 0.0f,
                                               params.SavedDepthCacheInvSizeDeep[2]));
    }

    const uint32 seabedCount = std::min(params.SeabedCount, kMaxOceanSeabeds);
    for (uint32 i = 0; i < seabedCount; ++i)
    {
        const OceanSeabedGPU& sb = params.Seabeds[i];
        const float32 dx = std::fabs(worldX - sb.OriginExtent[0]);
        const float32 dz = std::fabs(worldZ - sb.OriginExtent[1]);
        if (dx > sb.OriginExtent[2] || dz > sb.OriginExtent[3])
            continue;

        const float32 relX = worldX - sb.OriginExtent[0];
        const float32 relZ = worldZ - sb.OriginExtent[1];
        const float32 floorY = sb.HeightSlope[0] + relX * sb.HeightSlope[1] +
                               relZ * sb.HeightSlope[2];
        depth = std::min(depth, std::max(params.SeaLevel - floorY, 0.0f));
    }

    const uint32 contributorCount =
        std::min(params.DepthContributorCount, kMaxOceanDepthContributors);
    for (uint32 i = 0; i < contributorCount; ++i)
    {
        const OceanDepthContributorGPU& dc = params.DepthContributors[i];
        const float32 extentX = std::max(dc.OriginExtent[2], 1e-3f);
        const float32 extentZ = std::max(dc.OriginExtent[3], 1e-3f);
        const float32 relX = worldX - dc.OriginExtent[0];
        const float32 relZ = worldZ - dc.OriginExtent[1];
        const float32 absX = std::fabs(relX);
        const float32 absZ = std::fabs(relZ);

        const float32 rectEdge = std::min(extentX - absX, extentZ - absZ);
        const float32 ellipseEdge =
            (1.0f - std::sqrt((relX / extentX) * (relX / extentX) +
                              (relZ / extentZ) * (relZ / extentZ))) *
            std::min(extentX, extentZ);
        const float32 roundness = std::clamp(dc.DepthShape[2], 0.0f, 1.0f);
        const float32 edge = rectEdge * (1.0f - roundness) + ellipseEdge * roundness;
        const float32 feather = std::max(dc.DepthShape[1], 0.0f);
        depth = std::min(depth, OceanDepthBandDepth(std::max(dc.DepthShape[0], 0.0f), params.DepthBandSaturation,
                                                    edge, feather));
    }

    if (depth >= kDeepWater)
        return false;
    outDepth = depth;
    return true;
}

void OceanSeabedDepth::FillParams(OceanSeabedDepthParamsGPU& out)
{
    std::lock_guard<std::mutex> lock(m_ParamsMutex);
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    // The layout BeginFrame snapped: fixed for the frame, so the declare-time
    // copy matches what the dispatch binds.
    m_Params.Cascade = m_Depth.GetLayout();
    out = m_Params;
}

void OceanSeabedDepth::RecordDispatch(IDevice* device, CommandList* cl,
                                      BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;

    const TextureHandle depth = m_Depth.GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // Sync is graph-derived: the pass declares Write on the cascade, the first
    // declared reader restores ShaderReadOnly, and MarkOutput covers reader-less
    // frames. No manual barriers in here.

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanSeabedDepth_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanSeabedDepthParamsGPU));
    device->UpdateStorageImageBinding(ds, 1, depth);
    TextureHandle savedCacheTexture = m_DummySavedDepthTexture;
    SamplerHandle savedCacheSampler = m_SavedDepthSampler;
    TextureHandle rasterDepthTexture = m_DummySavedDepthTexture;
    SamplerHandle rasterDepthSampler = m_SavedDepthSampler;
    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        if (m_HasSavedDepthCache && m_SavedDepthTexture.IsValid())
            savedCacheTexture = m_SavedDepthTexture;
        if (m_HasRasterDepthCapture && m_RasterDepthCaptureTexture.IsValid())
        {
            rasterDepthTexture = m_RasterDepthCaptureTexture;
            if (m_RasterDepthCaptureSampler.IsValid())
                rasterDepthSampler = m_RasterDepthCaptureSampler;
        }
    }
    if (!savedCacheTexture.IsValid() || !savedCacheSampler.IsValid() ||
        !rasterDepthTexture.IsValid() || !rasterDepthSampler.IsValid())
        return;
    device->UpdateCombinedImageSamplerBinding(ds, 2, savedCacheTexture, savedCacheSampler);
    device->UpdateCombinedImageSamplerBinding(ds, 3, rasterDepthTexture, rasterDepthSampler);

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);

    {
        std::lock_guard<std::mutex> lock(m_ParamsMutex);
        m_DispatchDirty = false;
    }
}

RenderGraph::RGTexture OceanSeabedDepth::ImportRG(RenderGraph::RGFrame& frame) const
{
    return m_Depth.ImportRG(frame);
}

} // namespace GameEngine::Ocean
