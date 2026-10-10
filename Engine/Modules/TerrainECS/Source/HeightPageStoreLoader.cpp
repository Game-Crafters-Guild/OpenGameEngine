#include "TerrainECS/HeightPageStoreLoader.h"

#include "TerrainECS/RawHeightmap.h"
#include "PageStreaming/HeightPageCooker.h"
#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/PageStoreReader.h"
#include "PageStreaming/TerrainPageContainer.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

namespace GameEngine::TerrainECS
{

namespace
{

// What a cook thread needs, resolved on the main thread.
struct CookJob
{
    GUID Asset;
    uint64 ContentVersion = 0;
    std::filesystem::path Source;
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    std::filesystem::path CacheDirectory;
};

// Marks the cook finished however its thread leaves.
struct FinishOnExit
{
    std::atomic<bool>& Finished;
    ~FinishOnExit() { Finished.store(true); }
};

// Cooks (or finds) the store of `job`, opens it and hands it over. Logs the start and the end
// with the store's size and the time taken; reports its rows to `progress`.
void CookStore(CookJob job, std::shared_ptr<std::atomic<bool>> cancel, std::shared_ptr<std::mutex> mutex,
               std::shared_ptr<std::vector<HeightPageStoreLoader::Cooked>> out,
               std::shared_ptr<PageStreaming::HeightCookProgress> progress, std::shared_ptr<std::atomic<bool>> finished)
{
    const FinishOnExit finish{*finished};
    const std::string name = job.Source.filename().string();
    Logger::Log::Info("Terrain pages: preparing the height store of {} ({} x {} samples)", name, job.SamplesX,
                      job.SamplesZ);
    const auto start = std::chrono::steady_clock::now();
    const PageStreaming::HeightStoreCache cache(job.CacheDirectory);
    const PageStreaming::HeightStoreCache::Result result =
        cache.Ensure(job.Asset, job.Source, job.SamplesX, job.SamplesZ, &EngineCore::GetInstance().GetJobSystem(),
                     nullptr, cancel.get(), progress.get());
    const float64 seconds = std::chrono::duration<float64>(std::chrono::steady_clock::now() - start).count();
    if (cancel->load())
        return;
    if (!result.Error.empty())
    {
        Logger::Log::Warning("Terrain pages: {} keeps its height texture: {}", name, result.Error);
        return;
    }
    auto reader = std::make_shared<PageStreaming::PageStoreReader>();
    if (const std::string error = reader->Open(result.Store); !error.empty())
    {
        Logger::Log::Warning("Terrain pages: {} keeps its height texture: {}", name, error);
        return;
    }
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(result.Store, ec);
    Logger::Log::Info("Terrain pages: the height store of {} is ready ({} in {:.1f} s, {:.1f} MiB)", name,
                      result.Cooked ? "cooked" : "found", seconds,
                      ec ? 0.0 : static_cast<float64>(bytes) / (1024.0 * 1024.0));
    std::lock_guard<std::mutex> lock(*mutex);
    out->push_back({job.Asset, job.ContentVersion, std::move(reader)});
}

// The heightmap file and its decode grid, as the terrain decodes it (raw files only: a PNG
// heightmap is a texture asset and keeps its texture path). Empty source when it has none.
CookJob ResolveCookJob(const GUID& asset, uint64 contentVersion)
{
    CookJob job;
    job.Asset = asset;
    job.ContentVersion = contentVersion;
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return job;
    AssetMetadata meta{};
    const AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
    if (!registry.TryGetAssetMetadata(asset, meta) || meta.Path.empty())
        return job;
    std::string ext = meta.Path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".r16" && ext != ".r32")
        return job;
    RawHeightmapLayout declared;
    RawHeightmapLayout grid;
    if (!ResolveDeclaredRawHeightmapLayout(ReadRawHeightmapSettings(registry, meta.Path), declared).empty() ||
        !ResolveRawHeightmapFileLayout(meta.Path, declared.Width, declared.Height, grid).empty())
        return job;
    job.Source = meta.Path;
    job.SamplesX = grid.Width;
    job.SamplesZ = grid.Height;
    return job;
}

} // namespace

HeightPageStoreLoader::HeightPageStoreLoader() = default;

HeightPageStoreLoader::~HeightPageStoreLoader()
{
    StopCooks();
}

void HeightPageStoreLoader::StopCooks()
{
    for (auto& [asset, entry] : m_Entries)
        entry.Cancel->store(true);
    for (auto& [asset, entry] : m_Entries)
        if (entry.Cook.joinable())
            entry.Cook.join();
    m_Entries.clear();
    std::lock_guard<std::mutex> lock(*m_CookedMutex);
    m_Cooked->clear();
}

void HeightPageStoreLoader::SetLocation(HeightPageStoreLocation location)
{
    StopCooks();
    m_Location = std::move(location);
}

void HeightPageStoreLoader::StartCook(const GUID& asset, Entry& entry)
{
    CookJob job = ResolveCookJob(asset, entry.ContentVersion);
    if (job.Source.empty())
    {
        entry.Failed = true;
        return;
    }
    job.CacheDirectory = m_Location.CacheDirectory;
    entry.Progress = std::make_shared<PageStreaming::HeightCookProgress>();
    entry.CookFinished = std::make_shared<std::atomic<bool>>(false);
    entry.Cook = std::thread(CookStore, std::move(job), entry.Cancel, m_CookedMutex, m_Cooked, entry.Progress,
                             entry.CookFinished);
}

void HeightPageStoreLoader::OpenPackaged(const GUID& asset, Entry& entry) const
{
    const std::filesystem::path container = m_Location.PackagedDirectory / (asset.ToString() + ".geterrain");
    std::error_code ec;
    if (!std::filesystem::exists(container, ec))
    {
        entry.Failed = true;
        Logger::Log::Info("Terrain pages: no packaged height store for {}; the terrain keeps its height texture",
                          asset.ToString());
        return;
    }
    auto reader = std::make_shared<PageStreaming::PageStoreReader>();
    if (const std::string error =
            PageStreaming::OpenTerrainContainerField(container, PageStreaming::PageFieldKind::Height, *reader);
        !error.empty())
    {
        entry.Failed = true;
        Logger::Log::Warning("Terrain pages: {} keeps its height texture: {}", container.string(), error);
        return;
    }
    entry.Reader = std::move(reader);
}

void HeightPageStoreLoader::AdoptCooked()
{
    std::lock_guard<std::mutex> lock(*m_CookedMutex);
    for (Cooked& cooked : *m_Cooked)
    {
        const auto it = m_Entries.find(cooked.Asset);
        if (it != m_Entries.end() && it->second.ContentVersion == cooked.ContentVersion)
            it->second.Reader = std::move(cooked.Reader);
    }
    m_Cooked->clear();
}

std::shared_ptr<const PageStreaming::PageStoreReader> HeightPageStoreLoader::Store(const GUID& asset,
                                                                                    uint64 contentVersion,
                                                                                    std::string_view terrain)
{
    AdoptCooked();
    auto [it, inserted] = m_Entries.try_emplace(asset);
    Entry& entry = it->second;
    if (!inserted && entry.ContentVersion != contentVersion)
    {
        // The heightmap changed: drop its store and prepare the new one.
        entry.Cancel->store(true);
        if (entry.Cook.joinable())
            entry.Cook.join(); // one cook per asset at a time (HeightStoreCache)
        entry = Entry{};
        inserted = true;
    }
    if (inserted)
    {
        entry.ContentVersion = contentVersion;
        entry.Terrain = terrain;
        if (!m_Location.PackagedDirectory.empty())
            OpenPackaged(asset, entry);
        else if (!m_Location.CacheDirectory.empty())
            StartCook(asset, entry);
        else
            entry.Failed = true;
    }
    return entry.Reader;
}

void HeightPageStoreLoader::CookStatuses(std::vector<CookStatus>& out) const
{
    out.clear();
    for (const auto& [asset, entry] : m_Entries)
    {
        if (!entry.CookFinished || entry.CookFinished->load())
            continue;
        const uint64 total = entry.Progress->RowsTotal.load();
        const float32 fraction =
            total == 0 ? 0.0f : static_cast<float32>(entry.Progress->RowsDone.load()) / static_cast<float32>(total);
        out.push_back({entry.Terrain, std::min(fraction, 1.0f)});
    }
    std::sort(out.begin(), out.end(),
              [](const CookStatus& a, const CookStatus& b) { return a.Terrain < b.Terrain; });
}

} // namespace GameEngine::TerrainECS
