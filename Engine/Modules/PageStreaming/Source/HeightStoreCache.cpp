#include "PageStreaming/HeightStoreCache.h"

#include "PageStreaming/HeightPageCooker.h"
#include "PageStreaming/HeightStoreKey.h"
#include "PageStreaming/PageStoreReader.h"

#include "FileSystem/FileSystem.h"

#include <optional>
#include <system_error>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr const char* kStoreExtension = ".gepage";

// The asset's stores in the folder, under any key: <guid>-<16 hex digits>.gepage.
std::vector<std::filesystem::path> StoresOf(const std::filesystem::path& directory, const GUID& asset)
{
    const std::string prefix = asset.ToString() + "-";
    std::vector<std::filesystem::path> stores;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == kStoreExtension && name.compare(0, prefix.size(), prefix) == 0)
            stores.push_back(entry.path());
    }
    return stores;
}

// A store already at `store` that opens and was cooked under `key`.
bool IsCurrentStore(const std::filesystem::path& store, uint64 key)
{
    std::error_code ec;
    if (!std::filesystem::exists(store, ec))
        return false;
    PageStoreReader reader;
    return reader.Open(store).empty() && reader.Layout().Header.Key == key;
}

std::string ResolveIdentity(const std::filesystem::path& directory, const GUID& asset, const HeightCookSource& source,
                            AssetIOService* io, const std::atomic<bool>* cancel, HeightCookProgress* progress,
                            HeightSourceIdentity& out)
{
    if (const std::optional<HeightSourceIdentity> memo = ReadHeightSourceMemo(directory, asset, source.File))
    {
        out = *memo;
        return {};
    }
    if (std::string reason = ScanHeightSource(source, io, out, cancel, progress); !reason.empty())
        return reason;
    WriteHeightSourceMemo(directory, asset, source.File, out);
    return {};
}

} // namespace

HeightStoreCache::HeightStoreCache(std::filesystem::path directory)
    : m_Directory(std::move(directory))
{
}

HeightStoreCache::Result HeightStoreCache::Ensure(const GUID& asset, const std::filesystem::path& source, uint32 samplesX,
                                                  uint32 samplesZ, JobSystem::WorkStealingThreadPool* pool,
                                                  AssetIOService* io, const std::atomic<bool>* cancel,
                                                  HeightCookProgress* progress) const
{
    Result result;
    HeightCookRequest request;
    if (result.Error = ResolveHeightCookSource(source, samplesX, samplesZ, request.Source); !result.Error.empty())
        return result;
    std::error_code ec;
    std::filesystem::create_directories(m_Directory, ec);
    if (progress)
        progress->RowsTotal.store(2ull * request.Source.SamplesZ);
    if (result.Error = ResolveIdentity(m_Directory, asset, request.Source, io, cancel, progress, request.Identity);
        !result.Error.empty())
        return result;
    if (progress)
        progress->RowsDone.store(request.Source.SamplesZ); // identified (or remembered): the build reads it again

    request.Key = ComputeHeightStoreKey(
        {request.Identity.ContentHash, request.Source.Format, request.Source.SamplesX, request.Source.SamplesZ});
    request.Output = HeightStoreFile(m_Directory, asset, request.Key);
    result.Store = request.Output;
    if (IsCurrentStore(request.Output, request.Key))
        return result;

    FileSystem::RemoveOrphanedTemporaryFiles(m_Directory, kStoreExtension);
    for (const std::filesystem::path& store : StoresOf(m_Directory, asset))
        if (store != request.Output)
        {
            request.Previous = store;
            break;
        }
    request.Pool = pool;
    request.Io = io;
    request.Cancel = cancel;
    request.Progress = progress;
    const HeightCookResult cook = CookHeightPageStore(request);
    if (!cook.Error.empty())
    {
        result.Error = cook.Error;
        return result;
    }
    result.Cooked = true;
    for (const std::filesystem::path& store : StoresOf(m_Directory, asset))
        if (store != request.Output)
            std::filesystem::remove(store, ec);
    return result;
}

} // namespace GameEngine::PageStreaming
