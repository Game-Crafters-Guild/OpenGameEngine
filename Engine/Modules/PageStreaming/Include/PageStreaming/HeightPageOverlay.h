#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// An inclusive rect of one level's samples.
struct PageSampleRect
{
    uint32 MinX = 0;
    uint32 MinZ = 0;
    uint32 MaxX = 0;
    uint32 MaxZ = 0;

    uint32 Width() const { return MaxX - MinX + 1u; }
    uint32 Height() const { return MaxZ - MinZ + 1u; }
    bool Contains(uint32 x, uint32 z) const { return x >= MinX && x <= MaxX && z >= MinZ && z <= MaxZ; }
};

/// True when page `address` (its apron included) reads a level-0 sample of `level0Rect`.
bool PageReadsLevel0Rect(const PageAddress& address, const PageSampleRect& level0Rect);

/// The height a terrain's modifiers add to its base pages (the baked-page cache of the
/// page-streaming design, 3.8): level 0 holds the baked height minus the base over the rect the
/// modifiers reach, every coarser level the [1 2 1] / 4 tent of the level below with the store's
/// clamped edges (HeightPyramidBuilder's filter). The tent is linear, so a base page plus this
/// overlay's page at any level is the page the cooker cuts from the baked level 0: a modified
/// terrain pages without a second cook, and only the pages over the modifiers change.
///
/// An edit inside the overlay's rect rebakes it in place (Rebake): its one owner, the terrain's
/// modifier system, writes it on the main thread before the height pages read it in the same frame
/// (TerrainModifiers runs before TerrainExtraction), and a page upload applies it synchronously, so
/// no reader sees it half written and an edit never copies the whole overlay.
class HeightPageOverlay
{
public:
    /// The overlay of a pyramid of `levels` (the store's, BuildPageStoreLevels) whose level-0
    /// delta is `level0Delta` (row-major over `level0Rect`, which lies inside level 0). Zero
    /// outside the rect.
    HeightPageOverlay(std::span<const PageStoreLevel> levels, const PageSampleRect& level0Rect,
                      std::vector<float32> level0Delta);

    /// The bytes the overlay of `levels` over `level0Rect` holds: the rect's samples at every level,
    /// 4 B each (about 4/3 x 4 B per level-0 sample of the rect).
    static uint64 Bytes(std::span<const PageStoreLevel> levels, const PageSampleRect& level0Rect);

    /// Replaces level-0 rect `changed` (inside this overlay's level-0 rect) with `changedDelta`
    /// (row-major over `changed`) and refilters the coarser levels only where `changed` reaches them.
    void Rebake(const PageSampleRect& changed, std::span<const float32> changedDelta);

    /// Adds this overlay to page `address`'s kPageSampleCount samples (row-major, apron included).
    /// Returns false, leaving them untouched, when the page reads no sample of the overlay.
    bool ApplyToPage(const PageAddress& address, std::span<float32> samples) const;

    /// The level-0 rect the overlay covers.
    const PageSampleRect& Level0Rect() const { return m_Levels.front().Rect; }
    /// The delta at level `level`'s sample (x, z): zero outside the overlay.
    float32 Delta(uint32 level, uint32 x, uint32 z) const;

private:
    struct Level
    {
        PageStoreLevel Shape;
        PageSampleRect Rect;
        std::vector<float32> Delta; // row-major over Rect
    };

    HeightPageOverlay() = default;
    void FilterLevel(uint32 level, const PageSampleRect& finerChanged);

    std::vector<Level> m_Levels;
};

} // namespace GameEngine::PageStreaming
