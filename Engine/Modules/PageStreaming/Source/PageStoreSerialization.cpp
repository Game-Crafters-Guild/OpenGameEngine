#include "PageStoreSerialization.h"

#include "AssetCore/SharedFileRead.h"

#include <bit>
#include <cstring>

namespace GameEngine::PageStreaming
{
namespace
{

static_assert(std::endian::native == std::endian::little,
              "the .gepage layout is little-endian and written with raw copies");

template <typename T>
void Put(std::vector<uint8>& bytes, const T& value)
{
    const auto* raw = reinterpret_cast<const uint8*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

template <typename T>
T Take(const uint8* data, std::size_t& cursor)
{
    T value{};
    std::memcpy(&value, data + cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

bool ReadExact(SharedFileReader& file, uint64 offset, void* destination, uint64 bytes)
{
    return file.SeekTo(offset) && file.Read(destination, bytes) == static_cast<int64>(bytes);
}

std::string ValidateHeader(const PageStoreHeader& header, uint32 levelCount)
{
    if (header.Format != PageFieldFormat::HeightR16 && header.Format != PageFieldFormat::HeightR32F)
        return "unknown page format";
    if (header.Filter != PageFieldFilter::HeightTent)
        return "unknown pyramid filter";
    if (header.Units != PageHeightUnits::Normalized && header.Units != PageHeightUnits::Absolute)
        return "unknown height units";
    if (header.FaceCount == 0 || header.FaceCount > kPageStoreMaxFaces)
        return "face count out of range";
    if (header.SamplesX == 0 || header.SamplesZ == 0 || header.SamplesX > kPageStoreMaxSamplesPerAxis ||
        header.SamplesZ > kPageStoreMaxSamplesPerAxis)
        return "level-0 size out of range";
    if (header.Format == PageFieldFormat::HeightR16 && !(header.Quantum.Step > 0.0f))
        return "16-bit store without a positive height step";
    if (levelCount != BuildPageStoreLevels(header.SamplesX, header.SamplesZ).size())
        return "level count disagrees with the level-0 size";
    return {};
}

} // namespace

std::vector<uint8> SerializePageStoreHead(const PageStoreLayout& layout)
{
    std::vector<uint8> bytes;
    bytes.reserve(static_cast<std::size_t>(layout.DataOffset()));
    const PageStoreHeader& header = layout.Header;
    Put(bytes, kPageStoreMagic);
    Put(bytes, kPageStoreFormatVersion);
    Put(bytes, static_cast<uint8>(header.Format));
    Put(bytes, static_cast<uint8>(header.Filter));
    Put(bytes, static_cast<uint8>(header.Units));
    Put(bytes, header.FaceCount);
    Put(bytes, header.SamplesX);
    Put(bytes, header.SamplesZ);
    Put(bytes, static_cast<uint32>(layout.Levels.size()));
    Put(bytes, header.Quantum.Offset);
    Put(bytes, header.Quantum.Step);
    Put(bytes, header.Key);
    for (const PageStoreLevel& level : layout.Levels)
    {
        Put(bytes, level.SamplesX);
        Put(bytes, level.SamplesZ);
        Put(bytes, level.PagesX);
        Put(bytes, level.PagesZ);
        Put(bytes, level.FirstEntry);
    }
    for (const PageIndexEntry& entry : layout.Entries)
    {
        Put(bytes, entry.Offset);
        Put(bytes, entry.Hash);
        Put(bytes, entry.Size);
        Put(bytes, entry.MinWord);
        Put(bytes, entry.MaxWord);
    }
    return bytes;
}

std::string ReadPageStoreHead(SharedFileReader& file, uint64 baseOffset, uint64 availableBytes,
                              PageStoreLayout& out)
{
    uint8 fixed[kPageStoreHeaderBytes];
    if (availableBytes < kPageStoreHeaderBytes || !ReadExact(file, baseOffset, fixed, sizeof(fixed)))
        return "the file is shorter than a page store header";

    std::size_t cursor = 0;
    if (Take<uint32>(fixed, cursor) != kPageStoreMagic)
        return "the file is not a page store";
    if (Take<uint32>(fixed, cursor) != kPageStoreFormatVersion)
        return "the page store was written by another version of the engine; cook it again";

    PageStoreLayout layout;
    PageStoreHeader& header = layout.Header;
    header.Format = static_cast<PageFieldFormat>(Take<uint8>(fixed, cursor));
    header.Filter = static_cast<PageFieldFilter>(Take<uint8>(fixed, cursor));
    header.Units = static_cast<PageHeightUnits>(Take<uint8>(fixed, cursor));
    header.FaceCount = Take<uint8>(fixed, cursor);
    header.SamplesX = Take<uint32>(fixed, cursor);
    header.SamplesZ = Take<uint32>(fixed, cursor);
    const uint32 levelCount = Take<uint32>(fixed, cursor);
    header.Quantum.Offset = Take<float32>(fixed, cursor);
    header.Quantum.Step = Take<float32>(fixed, cursor);
    header.Key = Take<uint64>(fixed, cursor);
    if (std::string reason = ValidateHeader(header, levelCount); !reason.empty())
        return "the page store header is corrupt: " + reason;

    // The expected levels follow from the validated header; the stored directory must equal them.
    const std::vector<PageStoreLevel> expected = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    layout.Levels = expected;
    const uint64 entryCount = static_cast<uint64>(layout.EntriesPerFace()) * header.FaceCount;
    const uint64 headBytes =
        kPageStoreHeaderBytes + levelCount * kPageStoreLevelBytes + entryCount * kPageIndexEntryBytes;
    if (headBytes > availableBytes)
        return "the page store is truncated in its index";

    std::vector<uint8> rest(static_cast<std::size_t>(headBytes - kPageStoreHeaderBytes));
    if (!ReadExact(file, baseOffset + kPageStoreHeaderBytes, rest.data(), rest.size()))
        return "the page store's index could not be read";
    cursor = 0;
    for (const PageStoreLevel& level : expected)
    {
        const PageStoreLevel stored{Take<uint32>(rest.data(), cursor), Take<uint32>(rest.data(), cursor),
                                    Take<uint32>(rest.data(), cursor), Take<uint32>(rest.data(), cursor),
                                    Take<uint32>(rest.data(), cursor)};
        if (stored.SamplesX != level.SamplesX || stored.SamplesZ != level.SamplesZ ||
            stored.PagesX != level.PagesX || stored.PagesZ != level.PagesZ || stored.FirstEntry != level.FirstEntry)
            return "the page store's level directory is corrupt";
    }

    const uint32 pageBytes = PageStoredBytes(header.Format);
    layout.Entries.resize(static_cast<std::size_t>(entryCount));
    for (PageIndexEntry& entry : layout.Entries)
    {
        entry.Offset = Take<uint64>(rest.data(), cursor);
        entry.Hash = Take<uint64>(rest.data(), cursor);
        entry.Size = Take<uint32>(rest.data(), cursor);
        entry.MinWord = Take<uint32>(rest.data(), cursor);
        entry.MaxWord = Take<uint32>(rest.data(), cursor);
        if (!entry.IsPresent())
            continue;
        if (entry.Size != pageBytes || entry.Offset < headBytes || entry.Offset > availableBytes ||
            entry.Size > availableBytes - entry.Offset)
            return "the page store's index points outside the store";
    }
    out = std::move(layout);
    return {};
}

} // namespace GameEngine::PageStreaming
