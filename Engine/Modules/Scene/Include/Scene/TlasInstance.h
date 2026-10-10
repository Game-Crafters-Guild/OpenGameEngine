#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Geometry.h"
#include "Types/Types.h"

namespace GameEngine::Scene
{

// Per-entity record passed to TLAS traversal callbacks. POD; no backend
// implementation bits leak through. Callers needing per-mesh data (BLAS,
// material) resolve via Entity through the existing registries.
struct TlasInstance
{
    GameEngine::ECS::EntityHandle Entity{};
    Mathematics::AABB             WorldBounds{};
    uint32                        LayerMask    = 0xFFFFFFFFu;
    uint8                         InstanceMask = 0xFFu;  // 8-bit projection of LayerMask for future HW RT
    uint8                         _pad[3]      = {};
};

}
