#include "Placement/FencePieceReuse.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Placement/PieceEntity.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace GameEngine::Editor
{
namespace
{

struct IdentityHash
{
    size_t operator()(const FencePieceIdentity& id) const noexcept
    {
        const uint64 packed = (static_cast<uint64>(id.Primary) << 32) ^ id.Secondary;
        return std::hash<uint64>{}(packed ^ (static_cast<uint64>(id.Role) << 62));
    }
};

// Which span of the recipe a span or gate piece is, as a span override
// addresses it; what a picked piece's inspector reads to make it a gate.
Components::SplineFenceSpanPiece SpanAddressOf(const FenceSpan& span)
{
    Components::SplineFenceSpanPiece piece;
    piece.Run = span.Run;
    piece.OrdinalInRun = span.OrdinalInRun;
    piece.PoolSlot = span.PoolSlot;
    piece.IsGate = span.IsGate;
    return piece;
}

void WriteLabelIfChanged(ECS::World& world, ECS::EntityHandle entity, const FenceEmission& emission)
{
    const Components::Name label = MakePieceLabel(emission.RoleLabel, emission.LabelIndex);
    const auto* current = world.GetComponent<Components::Name>(entity);
    if (!current || current->View() != label.View())
        world.AddComponentImmediate(entity, label);
}

void WriteSpanAddressIfChanged(ECS::World& world, ECS::EntityHandle entity, const FenceEmission& emission)
{
    if (!emission.Span)
        return;
    const Components::SplineFenceSpanPiece address = SpanAddressOf(*emission.Span);
    const auto* current = world.GetComponent<Components::SplineFenceSpanPiece>(entity);
    if (!current || !(*current == address))
        world.AddComponentImmediate(entity, address);
}

ECS::EntityHandle SpawnPiece(ECS::World& world, ECS::EntityHandle parent,
                             const Mathematics::Matrix4x4& invPlacerWorld, const FenceEmission& emission)
{
    ECS::Entity piece = world.Create();
    Components::Transform transform{};
    WriteParentLocalPose(transform, invPlacerWorld, *emission.Pose, emission.Axis, emission.LengthScale);
    piece.Set(transform);
    piece.Set(emission.Source->Renderer);
    piece.Set(emission.Source->Bounds);
    piece.Set(Components::RuntimeOnlyEntity{});
    // Parenting is what keeps the hierarchy readable: the pieces collapse
    // under the placer instead of flooding the scene root.
    piece.Set(Components::Parent{parent});
    piece.Set(MakePieceLabel(emission.RoleLabel, emission.LabelIndex));
    if (emission.Span)
        piece.Set(SpanAddressOf(*emission.Span));
    return piece.GetHandle();
}

} // namespace

bool ReconcileFencePieces(ECS::World& world, ECS::EntityHandle parent,
                          const Mathematics::Matrix4x4& invPlacerWorld,
                          std::span<const FenceEmission> emissions,
                          std::vector<FencePieceEntity>& pieces)
{
    // Old pieces by identity, in their previous order, so pieces sharing one
    // identity are matched first to first.
    std::unordered_map<FencePieceIdentity, std::vector<size_t>, IdentityHash> byIdentity;
    byIdentity.reserve(pieces.size());
    for (size_t i = pieces.size(); i-- > 0;)
    {
        if (world.IsValid(pieces[i].Entity))
            byIdentity[pieces[i].Identity].push_back(i);
    }

    std::vector<uint8> claimed(pieces.size(), 0u);
    std::vector<FencePieceEntity> next;
    next.reserve(emissions.size());
    bool spawnedOrDestroyed = false;
    for (const FenceEmission& emission : emissions)
    {
        auto match = byIdentity.find(emission.Identity);
        if (match == byIdentity.end() || match->second.empty())
        {
            next.push_back({SpawnPiece(world, parent, invPlacerWorld, emission), emission.Identity,
                            emission.Signature});
            spawnedOrDestroyed = true;
            continue;
        }
        const size_t index = match->second.back();
        match->second.pop_back();
        claimed[index] = 1u;
        const FencePieceEntity& kept = pieces[index];
        if (auto* t = world.GetComponentForWrite<Components::Transform>(kept.Entity))
            WriteParentLocalPose(*t, invPlacerWorld, *emission.Pose, emission.Axis, emission.LengthScale);
        if (kept.Signature != emission.Signature)
        {
            world.AddComponentImmediate(kept.Entity, emission.Source->Renderer);
            world.AddComponentImmediate(kept.Entity, emission.Source->Bounds);
        }
        WriteLabelIfChanged(world, kept.Entity, emission);
        WriteSpanAddressIfChanged(world, kept.Entity, emission);
        next.push_back({kept.Entity, emission.Identity, emission.Signature});
    }

    for (size_t i = 0; i < pieces.size(); ++i)
    {
        if (claimed[i] != 0u)
            continue;
        if (world.IsValid(pieces[i].Entity))
            world.DestroyEntity(pieces[i].Entity);
        spawnedOrDestroyed = true;
    }
    pieces = std::move(next);
    return spawnedOrDestroyed;
}

} // namespace GameEngine::Editor
