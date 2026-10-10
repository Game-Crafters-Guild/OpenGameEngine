#pragma once

// Dirty-feed emission helper for DIRECT WorldTransform writers (change
// signaling §5 P2). Lives in the ECSComponents include root so every
// producer module (PhysicsECS, PathfindingECS, Engine, Editor) can reach it
// without new module edges (design M9's surviving half).
//
// The bump contract (see the WorldTransform comment in
// Components/Transform.h) requires `++wt.Version` after in-place matrix
// mutation. This helper fuses the bump with feed emission so the two cannot
// silently fork: a direct writer that bumps without emitting would freeze
// feed consumers (Scene TLAS refit) on that entity. Use it INSIDE your
// change gate — no bump and no emission for unchanged bytes.
//
// It does NOT stamp the write-grant column that Changed<WorldTransform>
// consumers filter on, because every caller here already holds a grant: they
// reached `wt` through a Write<WorldTransform> query visit or
// GetComponentForWrite, both of which stamp. A writer that mutates through a
// pointer cached OUTSIDE such a call owns its own stamping — see
// World::StampComponentWriteBatch and the transform hierarchy's node cache,
// its only user.
//
// Worlds without a feed subscription (thumbnail/preview worlds — anything
// that never called EnableComponentDirtyFeed) pay one compare and skip the
// append, so accumulation is impossible where no consumer drives the swap.
//
// Batch producers (the hierarchy flat parallel path) intentionally do NOT
// call this per entity: they bump inside their memcmp gate, stage changed
// handles chunk-locally, and emit once per chunk via
// World::EmitComponentDirtyBatch — same contract, amortized lock.

#include "Components/Transform.h"
#include "ECS/Entity.h"

namespace GameEngine::Components
{

inline void BumpWorldTransform(ECS::World& world, ECS::EntityHandle entity, WorldTransform& wt)
{
    ++wt.Version;
    world.EmitComponentDirty(ECS::GetComponentTypeId<WorldTransform>(), entity);
}

} // namespace GameEngine::Components
