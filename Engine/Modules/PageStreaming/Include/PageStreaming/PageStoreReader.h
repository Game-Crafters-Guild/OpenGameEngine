#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class SharedFileReader;
}

namespace GameEngine::PageStreaming
{

/// Reads one page store: a .gepage file, or one field of a terrain container (.geterrain) at the
/// field's offset. The layout is read and validated once at Open; pages are read synchronously by
/// seek + read. Every page call may run on any thread (reads share one handle under a lock).
class PageStoreReader
{
public:
    PageStoreReader();
    ~PageStoreReader();

    PageStoreReader(const PageStoreReader&) = delete;
    PageStoreReader& operator=(const PageStoreReader&) = delete;

    /// Opens the store at `baseOffset` of `file`, occupying the rest of the file or `storeBytes`
    /// when non-zero. Returns an empty string, else the reason (the reader stays closed).
    std::string Open(const std::filesystem::path& file, uint64 baseOffset = 0, uint64 storeBytes = 0);

    bool IsOpen() const { return m_IsOpen; }
    const PageStoreLayout& Layout() const { return m_Layout; }
    const std::filesystem::path& File() const { return m_File; }
    /// Where the store starts in File(); a page's file position is BaseOffset() + its entry's Offset.
    uint64 BaseOffset() const { return m_BaseOffset; }

    /// Reads a present page's stored bytes. False when the page is absent or the read fails.
    bool ReadPageBytes(const PageAddress& address, std::vector<uint8>& outBytes) const;

    /// Reads and decodes a present height page into kPageSampleCount samples (row-major, apron
    /// included). False when the page is absent or the read fails.
    bool ReadHeightPage(const PageAddress& address, std::span<float32> outSamples) const;

private:
    std::filesystem::path m_File;
    uint64 m_BaseOffset = 0;
    PageStoreLayout m_Layout;
    bool m_IsOpen = false;
    std::unique_ptr<SharedFileReader> m_Reader;
    mutable std::mutex m_ReadMutex;
};

} // namespace GameEngine::PageStreaming
