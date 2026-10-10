#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Matrix4x4.h"
#include "Placement/FenceEmission.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// One piece entity a fence placed, with what it is and the mesh it renders.
struct FencePieceEntity
{
    ECS::EntityHandle Entity{};
    FencePieceIdentity Identity{};
    uint64 Signature = 0;
};

// Brings a fence's piece entities in line with a new emission list, keeping
// every entity whose identity survives (FencePieceIdentity): a kept piece has
// its transform rewritten, takes the new renderer and bounds only when its mesh
// signature changed, its label only when the label changed, and the span
// address a picked piece reports (Components::SplineFenceSpanPiece) whenever it
// changed. Pieces whose identity is gone are destroyed; new identities are
// spawned as runtime-only children of `parent`. `pieces` comes back in emission
// order. Returns whether any entity was created or destroyed, which is when the
// caller must flush the world's deferred commands.
bool ReconcileFencePieces(ECS::World& world, ECS::EntityHandle parent,
                          const Mathematics::Matrix4x4& invPlacerWorld,
                          std::span<const FenceEmission> emissions,
                          std::vector<FencePieceEntity>& pieces);

} // namespace GameEngine::Editor
