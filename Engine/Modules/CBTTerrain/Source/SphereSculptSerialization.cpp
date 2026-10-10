#include "CBTTerrain/SphereSculptSerialization.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::CBTTerrain
{
namespace
{

// 'T','S','C','P' little-endian. Version 2 = adaptive page levels (S4); the encoder still emits
// version 1 for an all-level-0 store (byte-identical to pre-S4, readable by older builds), and
// the decoder reads both.
constexpr uint32_t kSculptBlobMagic = 0x50435354u;
constexpr uint32_t kSculptBlobVersionV1 = 1u;
constexpr uint32_t kSculptBlobVersionV2 = 2u;

// Header: magic, version, virtualDim, poolPageCount, pageCount (5 x uint32).
constexpr std::size_t kHeaderBytes = 5 * sizeof(uint32_t);
// Per-page record prefix: face, pageX, pageY, layerFlags (4 x uint32).
constexpr std::size_t kPagePrefixBytes = 4 * sizeof(uint32_t);

std::size_t LayerBytes(uint32_t level)
{
    const std::size_t fdim = SculptFineDim(level);
    return fdim * fdim * sizeof(float);
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value)
{
    const std::size_t offset = out.size();
    out.resize(offset + sizeof(uint32_t));
    std::memcpy(out.data() + offset, &value, sizeof(uint32_t));
}

uint32_t ReadU32(const uint8_t* data, std::size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(uint32_t));
    return value;
}

bool AllZero(const std::vector<float>& layer)
{
    return std::all_of(layer.begin(), layer.end(), [](float v) { return v == 0.0f; });
}

} // namespace

std::vector<uint8_t> EncodeSphereSculpt(const SphereSculptGeometry& geom,
                                        const std::vector<SphereSculptPageContent>& pages)
{
    // v1 unless an escalated page forces the level field into existence — an unescalated store's
    // blob stays byte-identical to pre-S4 (dark-ship) and readable by older builds.
    const bool anyLevel = std::any_of(pages.begin(), pages.end(),
                                      [](const SphereSculptPageContent& p) { return p.Level > 0u; });

    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + pages.size() * (kPagePrefixBytes + 2 * LayerBytes(0u)));
    AppendU32(out, kSculptBlobMagic);
    AppendU32(out, anyLevel ? kSculptBlobVersionV2 : kSculptBlobVersionV1);
    AppendU32(out, geom.VirtualDim);
    AppendU32(out, geom.PoolPageCount);
    const std::size_t pageCountOffset = out.size();
    AppendU32(out, 0u); // patched below with the pages actually written

    uint32_t written = 0;
    for (const SphereSculptPageContent& page : pages)
    {
        uint32_t flags = 0u;
        if (!AllZero(page.Dab))
            flags |= kSculptBlobHasDab;
        if (!AllZero(page.Modifier))
            flags |= kSculptBlobHasModifier;
        if (flags == 0u)
            continue; // zero-content page — never encoded
        if (anyLevel)
            flags |= page.Level << kSculptBlobLevelShift;

        const std::size_t layerBytes = LayerBytes(page.Level);
        AppendU32(out, page.Face);
        AppendU32(out, page.PageX);
        AppendU32(out, page.PageY);
        AppendU32(out, flags);
        if (flags & kSculptBlobHasDab)
        {
            const std::size_t offset = out.size();
            out.resize(offset + layerBytes);
            std::memcpy(out.data() + offset, page.Dab.data(), layerBytes);
        }
        if (flags & kSculptBlobHasModifier)
        {
            const std::size_t offset = out.size();
            out.resize(offset + layerBytes);
            std::memcpy(out.data() + offset, page.Modifier.data(), layerBytes);
        }
        ++written;
    }
    std::memcpy(out.data() + pageCountOffset, &written, sizeof(uint32_t));
    return out;
}

bool DecodeSphereSculpt(const uint8_t* data, std::size_t size, SphereSculptGeometry& outGeom,
                        std::vector<SphereSculptPageContent>& outPages)
{
    if (data == nullptr || size < kHeaderBytes)
        return false;
    if (ReadU32(data, 0) != kSculptBlobMagic)
        return false;
    const uint32_t version = ReadU32(data, sizeof(uint32_t));
    if (version != kSculptBlobVersionV1 && version != kSculptBlobVersionV2)
        return false;

    const uint32_t virtualDim = ReadU32(data, 2 * sizeof(uint32_t));
    const uint32_t poolPageCount = ReadU32(data, 3 * sizeof(uint32_t));
    const uint32_t pageCount = ReadU32(data, 4 * sizeof(uint32_t));

    // The saved grid must be a whole page multiple inside the fixed page-table cap — the same
    // invariant DeriveSculptVirtualDim guarantees for every live grid.
    if (virtualDim < kSculptPageDim || virtualDim % kSculptPageDim != 0u)
        return false;
    const uint32_t pagesPerAxis = virtualDim / kSculptPageDim;
    if (pagesPerAxis > kSculptMaxPagesPerFaceAxis)
        return false;
    // Cap the claimed page count against BOTH the grid capacity and the bytes actually present
    // (each record is at least prefix + one level-0 layer; escalated records are only bigger),
    // so a crafted count cannot force a huge reserve before the truncation check below would
    // catch it.
    const uint32_t gridCapacity = kCubeFaceCount * pagesPerAxis * pagesPerAxis;
    const std::size_t maxRecordsBySize = (size - kHeaderBytes) / (kPagePrefixBytes + LayerBytes(0u));
    if (pageCount > gridCapacity || pageCount > maxRecordsBySize)
        return false;

    // v1 rejects any bit beyond the two layer flags (unchanged); v2 additionally knows the
    // level field. Unknown bits in either version are rejected rather than guessed.
    const uint32_t knownFlags = kSculptBlobHasDab | kSculptBlobHasModifier |
                                (version == kSculptBlobVersionV2 ? kSculptBlobLevelMask : 0u);
    std::vector<SphereSculptPageContent> pages;
    pages.reserve(pageCount);
    std::size_t offset = kHeaderBytes;
    for (uint32_t i = 0; i < pageCount; ++i)
    {
        if (size - offset < kPagePrefixBytes)
            return false;
        SphereSculptPageContent page;
        page.Face = ReadU32(data, offset);
        page.PageX = ReadU32(data, offset + sizeof(uint32_t));
        page.PageY = ReadU32(data, offset + 2 * sizeof(uint32_t));
        const uint32_t flags = ReadU32(data, offset + 3 * sizeof(uint32_t));
        offset += kPagePrefixBytes;

        if (page.Face >= kCubeFaceCount || page.PageX >= pagesPerAxis || page.PageY >= pagesPerAxis)
            return false;
        if ((flags & (kSculptBlobHasDab | kSculptBlobHasModifier)) == 0u ||
            (flags & ~knownFlags) != 0u)
            return false; // empty record / unknown bits

        page.Level = version == kSculptBlobVersionV2
                         ? (flags & kSculptBlobLevelMask) >> kSculptBlobLevelShift
                         : 0u;
        if (page.Level > kSculptMaxPageLevel)
            return false;
        const std::size_t layerBytes = LayerBytes(page.Level);
        const std::size_t layerFloats = layerBytes / sizeof(float);
        page.Dab.assign(layerFloats, 0.0f);
        page.Modifier.assign(layerFloats, 0.0f);
        if (flags & kSculptBlobHasDab)
        {
            if (size - offset < layerBytes)
                return false;
            std::memcpy(page.Dab.data(), data + offset, layerBytes);
            offset += layerBytes;
        }
        if (flags & kSculptBlobHasModifier)
        {
            if (size - offset < layerBytes)
                return false;
            std::memcpy(page.Modifier.data(), data + offset, layerBytes);
            offset += layerBytes;
        }
        pages.push_back(std::move(page));
    }

    SphereSculptGeometry geom{};
    geom.VirtualDim = virtualDim;
    geom.PagesPerAxis = pagesPerAxis;
    geom.Cap = kSculptMaxPagesPerFaceAxis;
    geom.PoolPageCount = poolPageCount;
    outGeom = geom;
    outPages = std::move(pages);
    return true;
}

} // namespace GameEngine::CBTTerrain
