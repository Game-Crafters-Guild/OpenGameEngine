#include "PageStreaming/GeneratedHeightPageProvider.h"

#include "Terrain/Heightfield.h"

#include <algorithm>

namespace GameEngine::PageStreaming
{
namespace
{

// A tile axis's world coordinate of its sample `index`, as the tile fill computes it: the tile's
// corner (terrain corner + tile * tile size, TileStreamingManager's expression) plus the index
// times the tile's spacing (FillRegionWithNoiseWorldSpace's expression). Each operation is the
// fill's own, in its order, so the result is bit-identical.
float32 TileSampleWorld(float32 terrainOrigin, float32 tileWorldSize, uint32 tileSamples, uint32 tiles,
                        uint32 level0Index)
{
    const uint32 interval = tileSamples - 1u;
    const uint32 tile = std::min(level0Index / interval, tiles - 1u);
    const uint32 local = level0Index - tile * interval;
    const float32 tileOrigin = terrainOrigin + static_cast<float32>(static_cast<int32>(tile)) * tileWorldSize;
    const float32 spacing = tileWorldSize / static_cast<float32>(interval);
    return tileOrigin + static_cast<float32>(local) * spacing;
}

// The level-0 index of level `level`'s sample `index` (which may lie in the apron past either edge),
// clamped to the lattice.
uint32 Level0Index(int64 index, uint32 level, uint32 level0Samples)
{
    const int64 position = std::max<int64>(index, 0) << level;
    return static_cast<uint32>(std::min<int64>(position, static_cast<int64>(level0Samples) - 1));
}

} // namespace

GeneratedHeightPageProvider::GeneratedHeightPageProvider(const GeneratedHeightLattice& lattice)
    : m_Lattice(lattice)
{
    if (lattice.TileSamples < 2 || lattice.TilesX == 0 || lattice.TilesZ == 0 || !(lattice.TileWorldSize > 0.0f))
        return;
    const uint32 interval = lattice.TileSamples - 1u;
    m_Levels = BuildPageStoreLevels(lattice.TilesX * interval + 1u, lattice.TilesZ * interval + 1u);
    m_Level0Spacing = lattice.TileWorldSize / static_cast<float32>(interval);
}

float32 GeneratedHeightPageProvider::WorldX(uint32 level0Index) const
{
    return TileSampleWorld(m_Lattice.TerrainOriginX, m_Lattice.TileWorldSize, m_Lattice.TileSamples, m_Lattice.TilesX,
                           level0Index);
}

float32 GeneratedHeightPageProvider::WorldZ(uint32 level0Index) const
{
    return TileSampleWorld(m_Lattice.TerrainOriginZ, m_Lattice.TileWorldSize, m_Lattice.TileSamples, m_Lattice.TilesZ,
                           level0Index);
}

bool GeneratedHeightPageProvider::FillPage(const PageAddress& address, std::span<float32> outSamples) const
{
    if (m_Levels.empty() || address.Face != 0 || address.Level >= m_Levels.size() ||
        outSamples.size() != kPageSampleCount)
        return false;
    const PageStoreLevel& level = m_Levels[address.Level];
    if (address.X >= level.PagesX || address.Z >= level.PagesZ)
        return false;

    if (m_Lattice.Source == GeneratedHeightSource::Flat)
    {
        std::fill(outSamples.begin(), outSamples.end(), 0.0f);
        return true;
    }

    // Level 0 evaluates every octave, the tile fill's value; a coarser level drops the octaves its
    // spacing cannot carry (a lattice cell under two of its samples).
    const float32 minResolvedCell =
        address.Level == 0 ? 0.0f : 2.0f * m_Level0Spacing * static_cast<float32>(1u << address.Level);
    const uint32 level0SamplesX = m_Levels.front().SamplesX;
    const uint32 level0SamplesZ = m_Levels.front().SamplesZ;
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples;
    const GeneratedHeightNoise& noise = m_Lattice.Noise;

    float32 rowWorldX[kPageStrideSamples];
    for (uint32 i = 0; i < kPageStrideSamples; ++i)
    {
        const int64 sample = std::min<int64>(firstX + i, static_cast<int64>(level.SamplesX) - 1);
        rowWorldX[i] = WorldX(Level0Index(sample, address.Level, level0SamplesX));
    }
    for (uint32 row = 0; row < kPageStrideSamples; ++row)
    {
        const int64 sample = std::min<int64>(firstZ + row, static_cast<int64>(level.SamplesZ) - 1);
        const float32 worldZ = WorldZ(Level0Index(sample, address.Level, level0SamplesZ));
        float32* out = outSamples.data() + static_cast<std::size_t>(row) * kPageStrideSamples;
        for (uint32 i = 0; i < kPageStrideSamples; ++i)
            out[i] = Terrain::SampleWorldSpaceNoise(rowWorldX[i], worldZ, noise.Frequency, noise.Amplitude,
                                                    noise.Octaves, noise.Seed, minResolvedCell);
    }
    return true;
}

} // namespace GameEngine::PageStreaming
