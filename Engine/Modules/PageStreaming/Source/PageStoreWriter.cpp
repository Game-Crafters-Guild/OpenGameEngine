#include "PageStreaming/PageStoreWriter.h"

#include "PageStreaming/HeightPageCodec.h"
#include "NonInheritedFile.h"
#include "PageStoreSerialization.h"

#include "FileSystem/FileSystem.h"

#include <system_error>

namespace GameEngine::PageStreaming
{
namespace
{

bool Seek(std::FILE* file, uint64 offset)
{
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

} // namespace

PageStoreWriter::~PageStoreWriter()
{
    Abandon();
}

std::string PageStoreWriter::Create(const std::filesystem::path& target, PageStoreLayout layout)
{
    Abandon();
    if (layout.Levels.empty() || layout.Entries.empty())
        return "the page store has no levels";
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    m_Target = target;
    m_Temp = FileSystem::MakeTemporarySiblingPath(target);
    m_File = OpenNonInheritedFile(m_Temp, NonInheritedFileMode::CreateReadWrite);
    if (!m_File)
        return "the page store could not be created at " + m_Temp.string();
    m_Layout = std::move(layout);
    m_Failed = false;
    return {};
}

std::string PageStoreWriter::OpenForPatch(const std::filesystem::path& existing, const std::filesystem::path& target,
                                          PageStoreLayout layout)
{
    Abandon();
    m_Target = target;
    m_Temp = FileSystem::MakeTemporarySiblingPath(target);
    std::error_code ec;
    std::filesystem::rename(existing, m_Temp, ec);
    if (ec)
        return "the previous page store could not be moved aside for patching: " + ec.message();
    m_File = OpenNonInheritedFile(m_Temp, NonInheritedFileMode::OpenReadWrite);
    if (!m_File)
        return "the previous page store could not be opened for patching";
    m_Layout = std::move(layout);
    m_Failed = false;
    return {};
}

bool PageStoreWriter::WritePage(const PageAddress& address, const EncodedPage& page)
{
    const std::optional<std::size_t> index = m_Layout.EntryIndex(address);
    if (!index || !m_Layout.Entries[*index].IsPresent() || page.Bytes.size() != m_Layout.Entries[*index].Size)
        return false;
    PageIndexEntry& entry = m_Layout.Entries[*index];
    if (!WriteAt(entry.Offset, page.Bytes.data(), page.Bytes.size()))
        return false;
    // Distinct pages own distinct entries; the entry is read only after the writers are joined.
    entry.Hash = page.Hash;
    entry.MinWord = page.MinWord;
    entry.MaxWord = page.MaxWord;
    return true;
}

bool PageStoreWriter::ReadPage(const PageAddress& address, std::vector<uint8>& outBytes)
{
    const PageIndexEntry* entry = m_Layout.FindPresent(address);
    if (!entry)
        return false;
    outBytes.resize(entry->Size);
    return ReadAt(entry->Offset, outBytes.data(), outBytes.size());
}

std::string PageStoreWriter::Finish()
{
    if (!m_File)
        return "no page store is being written";
    const std::vector<uint8> head = SerializePageStoreHead(m_Layout);
    const bool written = !m_Failed && WriteAt(0, head.data(), head.size()) && std::fflush(m_File) == 0;
    const bool closed = std::fclose(m_File) == 0;
    m_File = nullptr;
    if (!written || !closed)
    {
        Abandon();
        return "the page store could not be written to " + m_Temp.string() + " (is the disk full?)";
    }
    if (!FileSystem::PublishFile(m_Temp, m_Target))
    {
        Abandon();
        return "the page store could not be put in place at " + m_Target.string();
    }
    m_Temp.clear();
    return {};
}

void PageStoreWriter::Abandon()
{
    if (m_File)
    {
        std::fclose(m_File);
        m_File = nullptr;
    }
    if (!m_Temp.empty())
    {
        std::error_code ec;
        std::filesystem::remove(m_Temp, ec);
        m_Temp.clear();
    }
}

bool PageStoreWriter::WriteAt(uint64 offset, const void* data, std::size_t bytes)
{
    std::lock_guard lock(m_FileMutex);
    const bool ok = m_File && Seek(m_File, offset) && std::fwrite(data, 1, bytes, m_File) == bytes;
    m_Failed = m_Failed || !ok;
    return ok;
}

bool PageStoreWriter::ReadAt(uint64 offset, void* data, std::size_t bytes)
{
    std::lock_guard lock(m_FileMutex);
    return m_File && Seek(m_File, offset) && std::fread(data, 1, bytes, m_File) == bytes;
}

} // namespace GameEngine::PageStreaming
