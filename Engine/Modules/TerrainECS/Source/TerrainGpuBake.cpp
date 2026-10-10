#include "TerrainECS/TerrainGpuBake.h"

#include <cstdlib>

namespace GameEngine::TerrainECS
{

namespace
{
// Test override for the GPU height-bake gate: -1 = follow the env var (default), 0 = force off,
// 1 = force on. TU-local so it is a single instance behind the exported accessors, never a
// cross-dll DATA symbol (which the auto-export does not carry reliably). Stays -1 in shipped runs.
int g_GpuHeightBakeTestOverride = -1;
} // namespace

bool IsGpuHeightBakeEnabled()
{
    if (g_GpuHeightBakeTestOverride >= 0)
        return g_GpuHeightBakeTestOverride != 0;
    // Default ON (kill switch GE_TERRAIN_GPU_BAKE=0), mirroring the DeferSplatResplatEnabled /
    // IncrementalQuadtreeEnabled convention. This is inert unless the terrain engaged the atlas
    // (past the unified-source ceiling) AND is whole-resident — AtlasGpuBakeEligible is only set
    // inside the atlas dispatch block — so a terrain on the unified path is unaffected; where the
    // atlas is active it moves the drag bake onto the GPU (parity-proven at the RG16F/RGBA8
    // floors, editor-verified radius-100 drag).
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_GPU_BAKE");
        return !(v != nullptr && v[0] == '0');
    }();
    return enabled;
}

void SetGpuHeightBakeEnabledForTests(int state) { g_GpuHeightBakeTestOverride = state; }

} // namespace GameEngine::TerrainECS
