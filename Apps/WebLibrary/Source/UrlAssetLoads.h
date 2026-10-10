#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameEngine::WebLibrary
{

/// What a load's status reads, numbered as abi.ts's AssetStatus.
enum class UrlAssetStatus : int32_t
{
    Loading = 0,
    Ready = 1,
    Failed = 2,
};

/// The assets a page loads by URL. A load fetches the file with the browser's fetch, writes it
/// under the asset root at a path named by the URL's hash, and loads it through the asset
/// manager. Its status moves only inside Advance, which the frame runs: a status read changes
/// nothing, so a load never finishes between two reads of the same frame. A URL is fetched and
/// loaded once: this table owns the URL's asset, and starting it again returns the load it
/// already has (a failed one is retried). Each instance of a model is an instantiation of that
/// one asset.
class UrlAssetLoads
{
public:
    UrlAssetLoads() = default;
    ~UrlAssetLoads();
    UrlAssetLoads(const UrlAssetLoads&) = delete;
    UrlAssetLoads& operator=(const UrlAssetLoads&) = delete;

    /// Starts fetching `url` (absolute) and returns the load's handle, never 0; returns the
    /// handle it already has for a URL that is loading or loaded, and 0 with the reason in
    /// `outError` when the load cannot start.
    uint32_t Start(const std::string& url, std::string& outError);

    /// The load's status; false when no load has `handle`.
    bool TryGetStatus(uint32_t handle, UrlAssetStatus& outStatus, std::string& outFailure) const;

    /// The loaded asset of a Ready load, with its identity; null otherwise.
    SharedPtr<Asset> GetAsset(uint32_t handle, GUID& outGuid) const;

    /// Moves every load forward: a finished fetch starts its asset load, a finished asset
    /// load becomes Ready or Failed.
    void Advance();

    /// Forgets every load; fetches still in flight are dropped when they land.
    void Clear();

private:
    struct LoadResult;
    struct Load
    {
        std::string Url;
        UrlAssetStatus Status = UrlAssetStatus::Loading;
        // The browser fetch while the file downloads; 0 once it has landed.
        int32_t FetchId = 0;
        // Filled by the asset manager's completion callback, on whichever thread completes it.
        std::shared_ptr<LoadResult> Result;
        GUID AssetGuid;
        SharedPtr<Asset> LoadedAsset;
        std::string Failure;
    };

    void AdvanceFetch(Load& load);
    void AdvanceAssetLoad(Load& load);

    std::unordered_map<uint32_t, Load> m_Loads;
    // The load each URL started, so a URL is fetched and loaded once.
    std::unordered_map<std::string, uint32_t> m_HandleByUrl;
    uint32_t m_NextHandle = 1;
};

} // namespace GameEngine::WebLibrary
