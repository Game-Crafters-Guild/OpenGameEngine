#pragma once

#include "ECS/SwapGenerationGuard.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Creates physics bodies/shapes for entities that have PhysicsBody + PhysicsCollider.
class PhysicsInitSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsInitSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // How long a heightfield's provider version must stay stable before a
    // tier-2 full rebuild is paid (in-place region updates are not throttled).
    // Instance state: tests construct their own system and shrink it without
    // cross-test contamination through process-global state.
    static constexpr float32 kDefaultRebuildQuiescenceSeconds = 0.25f;
    float32 GetRebuildQuiescenceSeconds() const { return m_RebuildQuiescenceSeconds; }
    void SetRebuildQuiescenceSeconds(float32 seconds) { m_RebuildQuiescenceSeconds = seconds; }

private:
    // Destroys the backend body of every PhysicsBody that went off since the
    // last swap (GetDisabled<PhysicsBody>), or of every body that is off when
    // a window was missed.
    void ReleaseDisabledBodies(ECS::World& world);

    float32 m_RebuildQuiescenceSeconds = kDefaultRebuildQuiescenceSeconds;
    ECS::SwapGenerationGuard m_SwapGuard;
};

} // namespace GameEngine::PhysicsECS

