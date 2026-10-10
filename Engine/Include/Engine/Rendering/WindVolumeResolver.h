#pragma once

#include "Types/Types.h"
#include "Rendering/Core/WindVolumeGPU.h"
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Engine::Renderer
{

struct ResolvedWind
{
    float32 VelocityX = 0.0f;
    float32 VelocityY = 0.0f;
    float32 VelocityZ = 0.0f;
    float32 Turbulence = 0.0f;
    float32 GustFrequency = 0.5f;
    float32 GustScale = 70.0f;
    float32 Weight = 0.0f;
};

class WindVolumeResolver
{
public:
    static ResolvedWind ResolveAt(ECS::World& world,
                                  float32 worldX,
                                  float32 worldY,
                                  float32 worldZ,
                                  const ResolvedWind& baseWind = {},
                                  uint32 layerMask = 0xFFFFFFFFu);

    // Snapshot enabled volumes for spatial GPU consumers into `out` (cleared first),
    // using the CPU resolver's transforms, layer filtering and deterministic priority
    // order. Callers own `out` so a per-frame extraction reuses its capacity.
    static void ExtractGPU(ECS::World& world, std::vector<Rendering::WindVolumeGPU>& out,
                           uint32 layerMask = 0xFFFFFFFFu);

    static uint64 QuantizedHash(const ResolvedWind& wind);
};

} // namespace GameEngine::Engine::Renderer
