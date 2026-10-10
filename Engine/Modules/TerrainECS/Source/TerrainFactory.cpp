#include "TerrainECS/TerrainFactory.h"

namespace GameEngine::TerrainECS
{

TerrainHandle CreateDefaultTerrainWithNoise(TerrainService& service,
                                             const Terrain::TerrainConfig& config,
                                             uint32 seed)
{
    auto handle = service.CreateTerrain(config);
    auto* data = service.GetTerrainData(handle);
    if (!data)
        return handle;

    data->Heightfield.FillWithNoise(4.0f, 1.0f, 5, seed);

    // Center around zero so valleys go below the origin
    float32* samples = data->Heightfield.GetMutableSamples();
    const std::size_t total = data->Heightfield.GetSampleCount();
    for (std::size_t i = 0; i < total; ++i)
        samples[i] -= 0.5f;

    data->MarkFullDirty();
    service.RebuildQuadtree(handle);

    return handle;
}

} // namespace GameEngine::TerrainECS
