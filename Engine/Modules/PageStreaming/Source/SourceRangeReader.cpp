#include "SourceRangeReader.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetIOService.h"

#include <memory>

namespace GameEngine::PageStreaming
{
namespace
{

using RangeResult = std::optional<std::vector<uint8>>;

// The read's two outcomes resolve one promise once: the bytes from the decode continuation (a
// Background job, which only hands them over), or none from the failure callback.
struct RangePromise
{
    std::promise<RangeResult> Promise;
    std::atomic<bool> Resolved{false};

    void Resolve(RangeResult result)
    {
        if (!Resolved.exchange(true))
            Promise.set_value(std::move(result));
    }

    ~RangePromise() { Resolve(std::nullopt); }
};

SharedPtr<Asset> HandOverBytes(RangePromise& promise, Vector<uint8> bytes)
{
    promise.Resolve(std::vector<uint8>(std::move(bytes)));
    return nullptr;
}

} // namespace

SourceRangeReader::SourceRangeReader(const std::filesystem::path& file, AssetIOService* io)
    : m_File(file)
    , m_Io(io)
{
    if (!m_Io)
        m_Reader.Open(file);
}

bool SourceRangeReader::IsOpen() const
{
    return m_Io || m_Reader.IsOpen();
}

void SourceRangeReader::Request(uint64 offset, uint64 bytes)
{
    if (!m_Io)
    {
        std::promise<RangeResult> ready;
        m_Pending = ready.get_future();
        std::vector<uint8> data(static_cast<std::size_t>(bytes));
        const bool read = m_Reader.SeekTo(offset) && m_Reader.Read(data.data(), bytes) == static_cast<int64>(bytes);
        ready.set_value(read ? RangeResult(std::move(data)) : std::nullopt);
        return;
    }

    auto promise = std::make_shared<RangePromise>();
    m_Pending = promise->Promise.get_future();
    AssetIOService::ReadRequest read;
    read.Metadata.Path = m_File;
    read.Priority = AssetLoadPriority::Low;
    read.RangeOffset = offset;
    read.RangeBytes = bytes;
    read.ProcessData = [promise](Vector<uint8> data) { return HandOverBytes(*promise, std::move(data)); };
    read.OnFailure = [promise](const String&) { promise->Resolve(std::nullopt); };
    m_Io->SubmitRead(std::move(read));
}

bool SourceRangeReader::Take(std::vector<uint8>& out)
{
    if (!m_Pending)
        return false;
    RangeResult result = m_Pending->get();
    m_Pending.reset();
    if (!result)
        return false;
    out = std::move(*result);
    return true;
}

} // namespace GameEngine::PageStreaming
