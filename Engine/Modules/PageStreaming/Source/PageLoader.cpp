#include "PageStreaming/PageLoader.h"

#include "PageStreaming/HeightPageCodec.h"
#include "PageStreaming/PageStoreReader.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetIOService.h"

namespace GameEngine::PageStreaming
{
namespace
{

// One load in flight, shared by the read's decode continuation and its failure callback. The
// first of Load and Fail resolves it; a load destroyed unresolved (its decode cancelled without
// running, as a pool shutdown does to queued work) fails as cancelled when the last holder lets
// go, on that holder's thread. So exactly one of OnLoaded and OnFailed runs, whatever happens.
class PendingPageLoad
{
public:
    explicit PendingPageLoad(PageLoadRequest request)
        : m_Request(std::move(request))
    {
    }

    ~PendingPageLoad() { Fail("the page load was cancelled"); }

    PendingPageLoad(const PendingPageLoad&) = delete;
    PendingPageLoad& operator=(const PendingPageLoad&) = delete;

    const PageLoadRequest& Request() const { return m_Request; }

    bool IsCancelled() const { return m_Request.Cancel && m_Request.Cancel->load(std::memory_order_acquire); }

    void Load(std::vector<float32> samples)
    {
        if (!m_Resolved.exchange(true))
            m_Request.OnLoaded(m_Request.Address, std::move(samples));
    }

    void Fail(const std::string& reason)
    {
        if (!m_Resolved.exchange(true))
            m_Request.OnFailed(m_Request.Address, reason);
    }

private:
    PageLoadRequest m_Request;
    std::atomic<bool> m_Resolved{false};
};

// The decode continuation: the page's bytes to samples. A page is not an asset, so the
// continuation's own result is always null.
SharedPtr<Asset> DecodeLoadedPage(PendingPageLoad& load, Vector<uint8> bytes)
{
    if (load.IsCancelled())
    {
        load.Fail("the page load was cancelled");
        return nullptr;
    }
    std::vector<float32> samples(kPageSampleCount);
    DecodeHeightPage(load.Request().Store->Layout().Header, bytes, samples);
    load.Load(std::move(samples));
    return nullptr;
}

} // namespace

void SubmitHeightPageLoad(AssetIOService& io, PageLoadRequest request)
{
    const PageIndexEntry* entry = request.Store ? request.Store->Layout().FindPresent(request.Address) : nullptr;
    if (!entry)
    {
        request.OnFailed(request.Address, "the page is not in the store");
        return;
    }

    auto load = std::make_shared<PendingPageLoad>(std::move(request));
    const PageLoadRequest& pending = load->Request();
    AssetIOService::ReadRequest read;
    read.AssetGuid = pending.StoreAsset;
    read.Metadata.Path = pending.Store->File();
    read.Priority = pending.Priority;
    read.RangeOffset = pending.Store->BaseOffset() + entry->Offset;
    read.RangeBytes = entry->Size;
    read.CancelRequested = pending.Cancel;
    read.ProcessData = [load](Vector<uint8> bytes) { return DecodeLoadedPage(*load, std::move(bytes)); };
    read.OnFailure = [load](const String& error) { load->Fail(error); };
    io.SubmitRead(std::move(read));
}

} // namespace GameEngine::PageStreaming
