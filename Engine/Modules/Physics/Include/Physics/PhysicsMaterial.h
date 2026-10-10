#pragma once

#include "Types/Types.h"

namespace GameEngine::Physics
{
// Simple physics contact material (backend-agnostic authoring).
// NOTE: Jolt applies friction/restitution at the body level by default.
struct PhysicsMaterial
{
    GameEngine::float32 friction = 0.5f;    // 0=no friction, higher=more friction (typical 0.2..1.0)
    GameEngine::float32 restitution = 0.1f; // 0=no bounce, 1=perfectly elastic
};
} // namespace GameEngine::Physics

