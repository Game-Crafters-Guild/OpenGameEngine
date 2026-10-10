#pragma once

#include "Types/Types.h"
#include "Components/AssetRef.h"
#include "ECS/ECS.h"

namespace GameEngine { namespace Components {

enum class SkeletonInstanceOwner : uint8 { Self, Entity };

// Lightweight ECS component referring to a skeleton stored in an Engine-owned repository.
// Use this instead of a heavy Skeleton component inside ECS chunks.
struct SkeletonRef {
    // A handle to the entity's skeleton, not a feature: it has no off state.
    static constexpr bool NotToggleable = true;

    uint32 skeletonId = 0; // [DoNotSerialize] process-local shared hierarchy/bind-pose cache; 0 = invalid
    uint32 runtimeId = 0;  // [DoNotSerialize] per-session SkeletonStore handle (pose, atlas offset), rebuilt per spawn; 0 = invalid
    uint64 runtimeGeneration = 0; // [DoNotSerialize] allocation identity; a recycled numeric slot owns no reference

    // Durable reconstruction source. Empty permits explicit, in-memory procedural
    // skeleton assignment; numeric store IDs are never scene-file identities.
    ModelRef sourceModelGuid{};
    // Entity mode keeps a missing/dangling scene reference unresolved instead
    // of silently becoming an independent instance. Self needs no reference.
    SkeletonInstanceOwner ownerMode = SkeletonInstanceOwner::Self;
    ECS::EntityHandle instanceOwner{};
};

} } // namespace GameEngine::Components
