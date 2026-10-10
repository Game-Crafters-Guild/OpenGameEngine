#pragma once

#include "ECS/Systems.h"
#include "Ocean/OceanCollisionProvider.h"
#include "Types/Types.h"

#include <unordered_map>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Ocean
{

// Applies buoyancy to entities with an OceanBuoyancy + PhysicsBody. Samples the
// provider-backed ocean height at four probe points around each body and applies upward
// force at each submerged probe (yielding buoyancy + self-righting torque) plus
// velocity drag, via the physics force API. Queries are persistent and non-blocking;
// startup and readback misses use the provider's deterministic fallback chain.
class OceanBuoyancySystem : public ECS::ISystem
{
public:
    explicit OceanBuoyancySystem(Engine::Renderer::RenderServices* renderServices);

    const char* GetName() const override { return "OceanBuoyancy"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    struct QueryState
    {
        OceanCollisionQueryHandle Handle = 0u;
        uint64 LastSeenFrame = 0u;
    };
    std::unordered_map<uint32, QueryState> m_Queries;
    uint64 m_Frame = 0u;
};

} // namespace GameEngine::Ocean
