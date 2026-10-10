#include "PageStreaming/TerrainPageContainer.h"

#include "PageStreaming/PageStoreReader.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "NonInheritedFile.h"

#include <cstring>
#include <system_error>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

// "GETR" read as a little-endian uint32.
constexpr uint32 kContainerMagic = 0x52544547u;
// magic, version, field count.
constexpr uint64 kContainerHeaderBytes = 12u;
// kind (as uint32), offset, size.
constexpr uint64 kContainerFieldBytes = 20u;
// One field per kind at most; the directory is bounded by the kinds there are.
constexpr uint32 kMaxContainerFields = 16u;
constexpr std::size_t kCopyChunkBytes = 64u * 1024u * 1024u;

template <typename T>
bool Write(std::FILE* out, const T& value)
{
    return std::fwrite(&value, sizeof(T), 1, out) == 1;
}

bool AppendFile(std::FILE* out, const std::filesystem::path& file, uint64 bytes)
{
    SharedFileReader reader;
    if (!reader.Open(file))
        return false;
    std::vector<char> chunk(kCopyChunkBytes);
    while (bytes > 0)
    {
        const uint64 want = std::min<uint64>(bytes, chunk.size());
        if (reader.Read(chunk.data(), want) != static_cast<int64>(want))
            return false;
        if (std::fwrite(chunk.data(), 1, static_cast<std::size_t>(want), out) != want)
            return false;
        bytes -= want;
    }
    return true;
}

} // namespace

std::string WriteTerrainContainer(const std::filesystem::path& target, std::span<const TerrainContainerField> fields)
{
    if (fields.empty() || fields.size() > kMaxContainerFields)
        return "a terrain container holds one to " + std::to_string(kMaxContainerFields) + " fields";
    std::vector<uint64> sizes;
    for (const TerrainContainerField& field : fields)
    {
        for (const TerrainContainerField& other : fields)
            if (&other != &field && other.Kind == field.Kind)
                return "a terrain container holds one store per field";
        std::error_code ec;
        sizes.push_back(std::filesystem::file_size(field.Store, ec));
        if (ec)
            return "the page store " + field.Store.string() + " could not be read";
    }

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const std::filesystem::path temp = FileSystem::MakeTemporarySiblingPath(target);
    bool written = false;
    if (std::FILE* out = OpenNonInheritedFile(temp, NonInheritedFileMode::CreateWrite))
    {
        written = Write(out, kContainerMagic) && Write(out, kPageStoreFormatVersion) &&
                  Write(out, static_cast<uint32>(fields.size()));
        uint64 offset = kContainerHeaderBytes + fields.size() * kContainerFieldBytes;
        for (std::size_t i = 0; written && i < fields.size(); ++i)
        {
            written = Write(out, static_cast<uint32>(fields[i].Kind)) && Write(out, offset) && Write(out, sizes[i]);
            offset += sizes[i];
        }
        for (std::size_t i = 0; written && i < fields.size(); ++i)
            written = AppendFile(out, fields[i].Store, sizes[i]);
        written = std::fclose(out) == 0 && written;
    }
    if (!written)
    {
        std::filesystem::remove(temp, ec);
        return "the terrain container could not be written to " + temp.string() + " (is the disk full?)";
    }
    if (!FileSystem::PublishFile(temp, target))
        return "the terrain container could not be put in place at " + target.string();
    return {};
}

std::string OpenTerrainContainerField(const std::filesystem::path& container, PageFieldKind kind,
                                      PageStoreReader& out)
{
    SharedFileReader reader;
    if (!reader.Open(container))
        return "the terrain container " + container.string() + " could not be opened";
    const int64 fileBytes = reader.Size();
    uint32 header[3] = {};
    if (fileBytes < static_cast<int64>(kContainerHeaderBytes) ||
        reader.Read(header, sizeof(header)) != static_cast<int64>(sizeof(header)))
        return "the terrain container " + container.string() + " is truncated";
    if (header[0] != kContainerMagic)
        return container.string() + " is not a terrain container";
    if (header[1] != kPageStoreFormatVersion)
        return "the terrain container " + container.string() + " was written by another version of the engine; "
               "export the game again";
    const uint32 fieldCount = header[2];
    if (fieldCount == 0 || fieldCount > kMaxContainerFields)
        return "the terrain container " + container.string() + " has a corrupt field directory";

    for (uint32 i = 0; i < fieldCount; ++i)
    {
        uint8 entry[kContainerFieldBytes];
        if (reader.Read(entry, sizeof(entry)) != static_cast<int64>(sizeof(entry)))
            return "the terrain container " + container.string() + " is truncated in its field directory";
        uint32 storedKind = 0;
        uint64 offset = 0;
        uint64 size = 0;
        std::memcpy(&storedKind, entry, sizeof(storedKind));
        std::memcpy(&offset, entry + 4, sizeof(offset));
        std::memcpy(&size, entry + 12, sizeof(size));
        if (storedKind != static_cast<uint32>(kind))
            continue;
        if (size == 0 || offset > static_cast<uint64>(fileBytes) || size > static_cast<uint64>(fileBytes) - offset)
            return "the terrain container " + container.string() + " has a field outside the file";
        reader.Close();
        return out.Open(container, offset, size);
    }
    return "the terrain container " + container.string() + " has no height field: export the game again";
}

} // namespace GameEngine::PageStreaming
