#include "PageStreaming/PageStoreFormat.h"

#include "PageStoreSerialization.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::PageStreaming
{
namespace
{

uint32 PagesFor(uint32 samples)
{
    return (samples + kPageOwnedSamples - 1u) / kPageOwnedSamples;
}

// One present page of a level with its Morton code, for the level's file order.
struct MortonPage
{
    uint64 Code = 0;
    std::size_t Entry = 0;
};

} // namespace

uint16 HeightQuantum::Encode(float32 value) const
{
    const float32 word = std::round((value - Offset) / Step);
    return static_cast<uint16>(std::clamp(word, 0.0f, 65535.0f));
}

uint32 PageStoreLayout::EntriesPerFace() const
{
    if (Levels.empty())
        return 0;
    const PageStoreLevel& last = Levels.back();
    return last.FirstEntry + last.PagesX * last.PagesZ;
}

std::optional<std::size_t> PageStoreLayout::EntryIndex(const PageAddress& address) const
{
    if (address.Face >= Header.FaceCount || address.Level >= Levels.size())
        return std::nullopt;
    const PageStoreLevel& level = Levels[address.Level];
    if (address.X >= level.PagesX || address.Z >= level.PagesZ)
        return std::nullopt;
    return static_cast<std::size_t>(address.Face) * EntriesPerFace() + level.FirstEntry +
           static_cast<std::size_t>(address.Z) * level.PagesX + address.X;
}

const PageIndexEntry* PageStoreLayout::FindPresent(const PageAddress& address) const
{
    const std::optional<std::size_t> index = EntryIndex(address);
    if (!index || *index >= Entries.size() || !Entries[*index].IsPresent())
        return nullptr;
    return &Entries[*index];
}

std::optional<PageAddress> PageStoreLayout::ResolvePresent(PageAddress address) const
{
    while (EntryIndex(address))
    {
        if (FindPresent(address))
            return address;
        address = address.Parent();
    }
    return std::nullopt;
}

uint64 PageStoreLayout::DataOffset() const
{
    return kPageStoreHeaderBytes + Levels.size() * kPageStoreLevelBytes + Entries.size() * kPageIndexEntryBytes;
}

uint64 PageStoreLayout::TotalBytes() const
{
    uint64 end = DataOffset();
    for (const PageIndexEntry& entry : Entries)
        if (entry.IsPresent())
            end = std::max(end, entry.Offset + entry.Size);
    return end;
}

std::vector<PageStoreLevel> BuildPageStoreLevels(uint32 samplesX, uint32 samplesZ)
{
    std::vector<PageStoreLevel> levels;
    if (samplesX == 0 || samplesZ == 0)
        return levels;
    uint32 firstEntry = 0;
    while (levels.size() < kPageStoreMaxLevels)
    {
        PageStoreLevel level;
        level.SamplesX = samplesX;
        level.SamplesZ = samplesZ;
        level.PagesX = PagesFor(samplesX);
        level.PagesZ = PagesFor(samplesZ);
        level.FirstEntry = firstEntry;
        firstEntry += level.PagesX * level.PagesZ;
        levels.push_back(level);
        if (level.PagesX == 1 && level.PagesZ == 1)
            break;
        samplesX = samplesX / 2u + 1u;
        samplesZ = samplesZ / 2u + 1u;
    }
    return levels;
}

uint32 PageStoredBytes(PageFieldFormat format)
{
    return kPageSampleCount * (format == PageFieldFormat::HeightR16 ? 2u : 4u);
}

PageStoreLayout MakePageStoreLayout(const PageStoreHeader& header, std::span<const uint8> present)
{
    PageStoreLayout layout;
    layout.Header = header;
    layout.Levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    layout.Entries.resize(static_cast<std::size_t>(layout.EntriesPerFace()) * header.FaceCount);
    if (!present.empty() && present.size() != layout.Entries.size())
        return layout; // every page absent: the caller's presence does not fit the header

    const uint32 pageBytes = PageStoredBytes(header.Format);
    uint64 offset = layout.DataOffset();
    std::vector<MortonPage> order;
    for (uint32 face = 0; face < header.FaceCount; ++face)
    {
        for (std::size_t levelIndex = 0; levelIndex < layout.Levels.size(); ++levelIndex)
        {
            const PageStoreLevel& level = layout.Levels[levelIndex];
            const std::size_t base = static_cast<std::size_t>(face) * layout.EntriesPerFace() + level.FirstEntry;
            order.clear();
            for (uint32 z = 0; z < level.PagesZ; ++z)
            {
                for (uint32 x = 0; x < level.PagesX; ++x)
                {
                    const std::size_t entry = base + static_cast<std::size_t>(z) * level.PagesX + x;
                    if (present.empty() || present[entry] != 0)
                        order.push_back(MortonPage{PageMortonCode(x, z), entry});
                }
            }
            std::sort(order.begin(), order.end(),
                      [](const MortonPage& a, const MortonPage& b) { return a.Code < b.Code; });
            for (const MortonPage& page : order)
            {
                layout.Entries[page.Entry].Offset = offset;
                layout.Entries[page.Entry].Size = pageBytes;
                offset += pageBytes;
            }
        }
    }
    return layout;
}

} // namespace GameEngine::PageStreaming
