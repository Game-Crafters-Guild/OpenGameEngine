#pragma once

#include "PageStreaming/PageStoreFormat.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::PageStreaming
{
class PageCache;
}

// The CPU mirror of the paged height resolve (cbt_page.glsl, slice 1b-ii): the page table, the
// physical cache texture's slot layout, and the sample at a terrain UV and a continuous level. It is
// the oracle the GLSL resolve is held to, as AtlasHeightSampler is for the resident-window atlas.
//
// The resolve at level lambda: L = floor(lambda) clamped to the pyramid, w = PageParentBlend(lambda).
// Level L's sample comes from L's page when resident, faded in from the parent's sample by its
// arrival fade, and from the first resident ancestor alone when not (the fallback walk). The result
// is lerp(level L's sample, level L+1's sample, w), so it reaches the parent exactly at the ring's
// end and is continuous in distance across every ring; the pinned top ends every walk.
namespace GameEngine::TerrainECS
{

/// The physical cache texture of one field: square-packed slots of kPageStrideSamples texels a side
/// (each page's owned samples and its apron).
struct PageCacheGeometry
{
    uint32 SlotsPerRow = 0;
    uint32 CacheDim = 0; ///< SlotsPerRow * kPageStrideSamples (square)

    /// The cache texel of `slot`'s stride texel (sx, sz).
    uint32 TexelX(uint32 slot, uint32 sx) const { return (slot % SlotsPerRow) * PageStreaming::kPageStrideSamples + sx; }
    uint32 TexelZ(uint32 slot, uint32 sz) const { return (slot / SlotsPerRow) * PageStreaming::kPageStrideSamples + sz; }
};

/// The geometry of a cache of `slotCount` slots.
PageCacheGeometry MakePageCacheGeometry(uint32 slotCount);

/// The page table of one field: one entry per page of every level (PackPageEntry(slot, level) when
/// resident, kNoPage when not) and the arrival fade of each slot. The GPU holds the entries as a
/// mip-mapped R32UI texture, level N of the table the pages of pyramid level N.
struct PageTable
{
    std::vector<PageStreaming::PageStoreLevel> Levels; ///< the field's pyramid (BuildPageStoreLevels)
    std::vector<uint32> Entries;                       ///< every level's pages, level-major, row-major
    std::vector<float32> SlotFade;                     ///< per slot: 0 just assigned, 1 settled

    /// An empty table (every entry kNoPage) for a field of samplesX x samplesZ and slotCount slots.
    void Reset(uint32 samplesX, uint32 samplesZ, uint32 slotCount);
    uint32 Entry(uint32 level, uint32 pageX, uint32 pageZ) const;
    void SetEntry(uint32 level, uint32 pageX, uint32 pageZ, uint32 entry);
};

/// Brings `table`, stream `stream`'s page table, up to date with this frame of its field's `cache`:
/// the entries of the stream's pages that were assigned or released, and the fades of the slots of
/// its pages whose fade changed. Edge-triggered: a quiescent frame writes nothing. Returns the
/// number of entries and fades written.
uint32 UpdatePageTable(const PageStreaming::PageCache& cache, uint32 stream, PageTable& table);

/// The resolve over a page table and the cache texture's texels (CacheDim x CacheDim floats).
struct PagedHeightSampler
{
    const PageTable* Table = nullptr;
    PageCacheGeometry Geometry;
    const float32* Cache = nullptr;

    /// The height at terrain UV (u, v) in [0, 1]^2 at continuous level `level`. Never reads outside
    /// a resident slot; 0 when not even the top level is resident.
    float32 Sample(float32 u, float32 v, float32 level) const;

    /// The height of level `level` alone at (u, v): the level's page when resident, faded in from
    /// its parent, else its first resident ancestor's. The resident level used goes to `outLevel`.
    float32 SampleLevel(float32 u, float32 v, uint32 level, uint32& outLevel) const;

private:
    float32 SampleResidentPage(float32 u, float32 v, uint32 level, uint32 slot) const;
};

} // namespace GameEngine::TerrainECS
