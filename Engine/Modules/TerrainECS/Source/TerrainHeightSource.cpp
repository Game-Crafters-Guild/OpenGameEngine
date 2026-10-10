#include "TerrainECS/TerrainHeightSource.h"

namespace GameEngine::TerrainECS
{

bool TerrainHeightIsPaged(const TerrainHeightSourceInputs& inputs)
{
    if (inputs.ModifiersPending || !inputs.PageTableFits || !inputs.GpuReadsPages)
        return false;
    return inputs.GeneratedBase || inputs.CookedStoreOpen;
}

} // namespace GameEngine::TerrainECS
