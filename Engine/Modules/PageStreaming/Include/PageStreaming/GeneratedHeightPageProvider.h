#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// What a generated height field holds.
enum class GeneratedHeightSource : uint8
{
    Noise = 0, ///< the terrain's base noise (Terrain/Heightfield.h, SampleWorldSpaceNoise)
    Flat = 1,  ///< zero everywhere
};

/// The base noise's parameters, as the terrain's tile fill uses them.
struct GeneratedHeightNoise
{
    float32 Frequency = 0.0f;
    float32 Amplitude = 0.0f;
    uint32 Octaves = 0;
    uint32 Seed = 0;
};

/// The lattice of a tiled terrain whose base is generated rather than cooked: its tile grid, whose
/// tiles share their edge samples, and the source. Level 0 is the tiles' own samples.
struct GeneratedHeightLattice
{
    GeneratedHeightSource Source = GeneratedHeightSource::Flat;
    float32 TerrainOriginX = 0.0f; ///< world position of tile (0, 0)'s corner
    float32 TerrainOriginZ = 0.0f;
    float32 TileWorldSize = 0.0f;  ///< meters per tile side
    uint32 TileSamples = 0;        ///< samples per tile side, shared edge included
    uint32 TilesX = 0;
    uint32 TilesZ = 0;
    GeneratedHeightNoise Noise;
};

/// The page provider of a terrain whose base is noise or flat: a page is evaluated on demand, with
/// no store and no cook, the same in the editor and the Player.
///
/// Level 0 is byte-identical to the terrain's tile fill (TerrainECS FillTiledBaseRegion): a sample
/// is evaluated at the world position its owning tile computes for it (a tile owns its samples
/// but its last row and column, which the next tile owns; the grid's last tile owns its own). A
/// coarser level is evaluated at its own spacing with the octaves finer than twice that spacing
/// band-limited away, never filtered from level 0. Samples past the lattice's edge repeat the
/// edge position. Thread-safe: FillPage is a pure function of the lattice and the address.
class GeneratedHeightPageProvider
{
public:
    explicit GeneratedHeightPageProvider(const GeneratedHeightLattice& lattice);

    const GeneratedHeightLattice& Lattice() const { return m_Lattice; }
    /// The pyramid of the lattice, as a store of its level-0 size would have it.
    const std::vector<PageStoreLevel>& Levels() const { return m_Levels; }

    /// Fills a page's kPageSampleCount samples (row-major, apron included), normalized heights.
    /// False when the address is outside the pyramid or the lattice is empty.
    bool FillPage(const PageAddress& address, std::span<float32> outSamples) const;

private:
    float32 WorldX(uint32 level0Index) const;
    float32 WorldZ(uint32 level0Index) const;

    GeneratedHeightLattice m_Lattice;
    std::vector<PageStoreLevel> m_Levels;
    float32 m_Level0Spacing = 0.0f;
};

} // namespace GameEngine::PageStreaming
