#include "WebPackReader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>
#include <type_traits>

namespace GameEngine::WebPlayer
{
namespace
{

constexpr char kMagic[8] = {'G', 'E', 'P', 'A', 'K', '\0', '\0', '\0'};
constexpr std::uint32_t kPackFormatVersion = 1;
constexpr std::size_t kHeaderSize = 24; // magic[8] + version + entryCount + indexBytes

// Little-endian reads against a bounds-checked cursor: the pack arrives over
// the network, so every field is treated as untrusted until it is in range.
class Cursor
{
public:
    explicit Cursor(std::span<const std::uint8_t> data) : m_Data(data) {}

    bool Take(std::size_t count, const std::uint8_t*& out)
    {
        if (count > m_Data.size() - m_Offset)
            return false;
        out = m_Data.data() + m_Offset;
        m_Offset += count;
        return true;
    }

    template <typename T>
    bool Read(T& out)
    {
        static_assert(std::is_unsigned_v<T>);
        const std::uint8_t* bytes = nullptr;
        if (!Take(sizeof(T), bytes))
            return false;
        out = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            out |= static_cast<T>(bytes[i]) << (8 * i);
        return true;
    }

    std::size_t Offset() const { return m_Offset; }

private:
    std::span<const std::uint8_t> m_Data;
    std::size_t m_Offset = 0;
};

bool IsLegalEntryPath(std::string_view path)
{
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == path.size())
            return true;
        start = end + 1;
    }
    return true;
}

PackUnpackResult Fail(std::string error)
{
    PackUnpackResult result;
    result.Error = std::move(error);
    return result;
}

} // namespace

PackUnpackResult UnpackWebPack(std::span<const std::uint8_t> image,
                               const std::filesystem::path& root)
{
    Cursor header(image);
    const std::uint8_t* magic = nullptr;
    std::uint32_t version = 0;
    std::uint32_t entryCount = 0;
    std::uint64_t indexBytes = 0;
    if (!header.Take(sizeof(kMagic), magic) || !header.Read(version) ||
        !header.Read(entryCount) || !header.Read(indexBytes))
        return Fail("pack is shorter than its header");
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return Fail("not a .gepak file (bad magic)");
    if (version != kPackFormatVersion)
        return Fail("pack format version " + std::to_string(version) + " but this player reads " +
                    std::to_string(kPackFormatVersion) + " — re-export with the matching tools");
    if (indexBytes > image.size() - kHeaderSize)
        return Fail("pack index overruns the file");

    PackUnpackResult result;
    Cursor index(image);
    const std::uint8_t* skipped = nullptr;
    index.Take(kHeaderSize, skipped);
    const std::size_t indexEnd = kHeaderSize + static_cast<std::size_t>(indexBytes);

    for (std::uint32_t i = 0; i < entryCount; ++i)
    {
        std::uint32_t pathBytes = 0;
        const std::uint8_t* pathData = nullptr;
        std::uint64_t dataOffset = 0;
        std::uint64_t dataBytes = 0;
        if (!index.Read(pathBytes) || !index.Take(pathBytes, pathData) ||
            !index.Read(dataOffset) || !index.Read(dataBytes) || index.Offset() > indexEnd)
            return Fail("pack index is truncated at entry " + std::to_string(i));

        const std::string path(reinterpret_cast<const char*>(pathData), pathBytes);
        if (!IsLegalEntryPath(path))
            return Fail("pack entry path escapes the unpack root: '" + path + "'");
        // Split the bound so a hostile offset+size cannot wrap past the end.
        if (dataOffset > image.size() || dataBytes > image.size() - dataOffset)
            return Fail("pack entry '" + path + "' extends past the end of the file");

        const std::filesystem::path destination = root / std::filesystem::path(path);
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out)
            return Fail("cannot write '" + destination.string() + "'");
        out.write(reinterpret_cast<const char*>(image.data() + dataOffset),
                  static_cast<std::streamsize>(dataBytes));
        if (!out)
            return Fail("write failed for '" + destination.string() + "'");
        ++result.FileCount;
    }

    result.Success = true;
    return result;
}

} // namespace GameEngine::WebPlayer
