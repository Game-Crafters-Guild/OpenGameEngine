#pragma once

#include "PageStreaming/PageAddress.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetIOService;
}

namespace GameEngine::PageStreaming
{

class PageStoreReader;

/// One asynchronous page load.
struct PageLoadRequest
{
    /// The open store to read from; kept alive until the load resolves.
    std::shared_ptr<const PageStoreReader> Store;
    /// The store's identity for the reader queue (cancellation and logs): the asset it was cooked from.
    GUID StoreAsset;
    PageAddress Address;
    AssetLoadPriority Priority = AssetLoadPriority::Normal;
    /// Set to drop the load: a queued read is skipped, a finished read is not decoded.
    std::shared_ptr<std::atomic<bool>> Cancel;
    /// The decoded page's kPageSampleCount samples, on a Background job.
    std::function<void(const PageAddress&, std::vector<float32>)> OnLoaded;
    /// Why the page did not load (absent, unreadable, cancelled, the service or the job pool
    /// stopped), on whichever thread found out. Exactly one of OnLoaded and OnFailed runs, also when
    /// the decode is dropped without running (a pool shutdown cancels queued work).
    std::function<void(const PageAddress&, const std::string&)> OnFailed;
};

/// Loads one height page: a byte-range read of the page on `io`'s reader threads at the request's
/// priority, then the decode on a Background job. An absent page fails inline.
void SubmitHeightPageLoad(AssetIOService& io, PageLoadRequest request);

} // namespace GameEngine::PageStreaming
