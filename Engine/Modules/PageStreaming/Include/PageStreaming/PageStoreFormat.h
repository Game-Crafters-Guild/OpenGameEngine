#pragma once

#include "PageStreaming/PageAddress.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

// The page store (.gepage): one field of a terrain as a pyramid of fixed-size pages.
//
// The field is a lattice: level-0 sample i sits at field coordinate i / (samples - 1), and level N
// has sample j at the position of level-0 sample j * 2^N. A level's samples continue past the
// field's last sample when (samples - 1) is not a multiple of 2^N; such samples evaluate the
// field's filter over edge-clamped inputs, so every level covers the whole field. A page owns
// kPageOwnedSamples samples per axis (page p owns samples 128p to 128p + 127) and stores one apron
// sample on each side, the neighbor's edge sample, so bilinear filtering and central differences
// inside a page never read another page; past the field's edge the apron repeats the edge sample.
// The apron sample a page stores and the same sample its neighbor owns are the same value, bit for
// bit, because every value is a function of the sample's position and the store-wide encoding.
//
// On disk, little-endian: the fixed header, the level directory, the page index (one entry per
// page of every level of every face, row-major within a level, an absent page with Size 0), then
// the pages, level-major and in Morton order within a level so that pages near each other on the
// terrain are near each other in the file. Offsets are from the start of the store, so a store
// packed into a terrain container (TerrainPageContainer.h) is read at a base offset unchanged.

/// Samples a page owns per axis.
inline constexpr uint32 kPageOwnedSamples = 128u;
/// Apron samples a page stores beyond its owned samples on each side.
inline constexpr uint32 kPageApronSamples = 1u;
/// Samples a page stores per axis: its owned samples and the apron on both sides.
inline constexpr uint32 kPageStrideSamples = kPageOwnedSamples + 2u * kPageApronSamples;
/// Samples a page stores.
inline constexpr uint32 kPageSampleCount = kPageStrideSamples * kPageStrideSamples;
/// The coarsest height step a 16-bit store may use: 1 cm, so 16 bits span 655.35 m. A field whose
/// range needs a coarser step is stored as 32-bit float.
inline constexpr float32 kHeightStepMax = 0.01f;
/// Version of the .gepage layout and of every rule that decides its bytes (the level rule, the
/// filters, the encoding). It seeds every cook key, so a bump turns every store into a miss.
inline constexpr uint32 kPageStoreFormatVersion = 1u;
/// The most faces a store holds (a cube's six).
inline constexpr uint32 kPageStoreMaxFaces = 6u;
/// The most levels a store holds.
inline constexpr uint32 kPageStoreMaxLevels = 32u;
/// The most level-0 samples along either axis of a store: 2^22 (1,048 km at 0.25 m). It keeps every
/// page count of a face, all levels together, inside 32 bits (2^15 x 2^15 level-0 pages, about
/// 1.43 billion in all).
inline constexpr uint32 kPageStoreMaxSamplesPerAxis = 1u << 22u;

/// What a store's field is to the terrain. The terrain container's field directory names its
/// stores by this.
enum class PageFieldKind : uint8
{
    Height = 0,
};

/// How a page's samples are stored.
enum class PageFieldFormat : uint8
{
    HeightR16 = 0,  ///< 16-bit words in the store's HeightQuantum.
    HeightR32F = 1, ///< 32-bit floats as they are.
};

/// How a coarser level is made from the level below it.
enum class PageFieldFilter : uint8
{
    /// The [1 2 1] / 4 tent per axis centered on the coarser sample's own position, so the
    /// lattice keeps its positions at every level.
    HeightTent = 0,
};

/// What a height value is in the world.
enum class PageHeightUnits : uint8
{
    Normalized = 0, ///< [0, 1], scaled by the terrain's Height Scale (.r16 and 16-bit PNG sources).
    Absolute = 1,   ///< The source's own values, scaled by the Height Scale (.r32: meters at Height Scale 1).
};

/// The store-wide 16-bit height encoding: value = Offset + word * Step. One quantum for every page
/// and level of a store, so a sample two pages share is one word and decodes to one float in both.
struct HeightQuantum
{
    float32 Offset = 0.0f;
    float32 Step = 1.0f;

    /// The word nearest `value`, clamped to the 16-bit range.
    uint16 Encode(float32 value) const;
    /// The value of a word. The one decode every reader of a 16-bit store uses.
    float32 Decode(uint16 word) const { return Offset + static_cast<float32>(word) * Step; }
};

/// The fixed header of a store.
struct PageStoreHeader
{
    PageFieldFormat Format = PageFieldFormat::HeightR32F;
    PageFieldFilter Filter = PageFieldFilter::HeightTent;
    PageHeightUnits Units = PageHeightUnits::Absolute;
    uint8 FaceCount = 1;
    uint32 SamplesX = 0; ///< level-0 samples along X
    uint32 SamplesZ = 0; ///< level-0 samples along Z
    HeightQuantum Quantum; ///< used by HeightR16 only
    uint64 Key = 0;        ///< the cook key the store was made under (HeightStoreKey.h)
};

/// One level of a store.
struct PageStoreLevel
{
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    uint32 PagesX = 0;
    uint32 PagesZ = 0;
    uint32 FirstEntry = 0; ///< the level's first entry within a face's run of entries
};

/// One page's entry in the index. Serialized field by field as 28 bytes.
struct PageIndexEntry
{
    uint64 Offset = 0; ///< from the start of the store
    uint64 Hash = 0;   ///< FNV-1a 64 of the page's stored bytes
    uint32 Size = 0;   ///< stored bytes; 0 = the page is absent
    /// The lowest and highest level-0 value under the page's footprint, shared edge included, in
    /// the store's encoding: a quantum word for HeightR16, the float's bits for HeightR32F.
    uint32 MinWord = 0;
    uint32 MaxWord = 0;

    bool IsPresent() const { return Size != 0; }
};

inline constexpr std::size_t kPageIndexEntryBytes = 28u;

/// A store's shape: its header, levels and index.
struct PageStoreLayout
{
    PageStoreHeader Header;
    std::vector<PageStoreLevel> Levels;
    std::vector<PageIndexEntry> Entries; ///< FaceCount runs of EntriesPerFace

    uint32 EntriesPerFace() const;
    /// The index of the address's entry, or none when the address is outside the store.
    std::optional<std::size_t> EntryIndex(const PageAddress& address) const;
    /// The address's entry when the page is present, else null.
    const PageIndexEntry* FindPresent(const PageAddress& address) const;
    /// The address itself or its nearest present ancestor; none when the address is outside the
    /// store or no ancestor is present.
    std::optional<PageAddress> ResolvePresent(PageAddress address) const;
    /// Bytes of everything before the first page.
    uint64 DataOffset() const;
    /// Bytes of the whole store.
    uint64 TotalBytes() const;
};

/// The levels of a field of `samplesX` x `samplesZ` level-0 samples: each coarser level has
/// samples / 2 + 1 per axis (the lattice covering the level below), down to the first level that
/// fits one page.
std::vector<PageStoreLevel> BuildPageStoreLevels(uint32 samplesX, uint32 samplesZ);

/// The stored bytes of one page in `format`.
uint32 PageStoredBytes(PageFieldFormat format);

/// The layout of a store with `header`: every level, and each page present where `present` (one
/// byte per entry, in index order) is non-zero; an empty `present` makes every page present.
/// Present pages get their offsets, level-major and in Morton order within a level.
PageStoreLayout MakePageStoreLayout(const PageStoreHeader& header, std::span<const uint8> present = {});

} // namespace GameEngine::PageStreaming
