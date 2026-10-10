#include "PageStreaming/PageStoreReader.h"

#include "PageStreaming/HeightPageCodec.h"
#include "PageStoreSerialization.h"

#include "AssetCore/SharedFileRead.h"

namespace GameEngine::PageStreaming
{

PageStoreReader::PageStoreReader() = default;
PageStoreReader::~PageStoreReader() = default;

std::string PageStoreReader::Open(const std::filesystem::path& file, uint64 baseOffset, uint64 storeBytes)
{
    m_IsOpen = false;
    m_Layout = {};
    auto reader = std::make_unique<SharedFileReader>();
    if (!reader->Open(file))
        return "the page store " + file.string() + " could not be opened";
    const int64 fileBytes = reader->Size();
    if (fileBytes < 0 || baseOffset > static_cast<uint64>(fileBytes))
        return "the page store " + file.string() + " is shorter than its offset";
    uint64 available = static_cast<uint64>(fileBytes) - baseOffset;
    if (storeBytes != 0)
    {
        if (storeBytes > available)
            return "the page store in " + file.string() + " is truncated";
        available = storeBytes;
    }
    PageStoreLayout layout;
    if (std::string reason = ReadPageStoreHead(*reader, baseOffset, available, layout); !reason.empty())
        return reason + " (" + file.string() + ")";

    m_File = file;
    m_BaseOffset = baseOffset;
    m_Layout = std::move(layout);
    m_Reader = std::move(reader);
    m_IsOpen = true;
    return {};
}

bool PageStoreReader::ReadPageBytes(const PageAddress& address, std::vector<uint8>& outBytes) const
{
    const PageIndexEntry* entry = m_IsOpen ? m_Layout.FindPresent(address) : nullptr;
    if (!entry)
        return false;
    outBytes.resize(entry->Size);
    std::lock_guard lock(m_ReadMutex);
    return m_Reader->SeekTo(m_BaseOffset + entry->Offset) &&
           m_Reader->Read(outBytes.data(), entry->Size) == static_cast<int64>(entry->Size);
}

bool PageStoreReader::ReadHeightPage(const PageAddress& address, std::span<float32> outSamples) const
{
    std::vector<uint8> bytes;
    if (outSamples.size() != kPageSampleCount || !ReadPageBytes(address, bytes))
        return false;
    DecodeHeightPage(m_Layout.Header, bytes, outSamples);
    return true;
}

} // namespace GameEngine::PageStreaming
