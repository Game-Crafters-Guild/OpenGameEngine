#pragma once

#include "ECS/Entity.h"

namespace GameEngine {
namespace Components {

// Parent component: attach to a child to reference its logical parent entity.
// The editor Hierarchy will derive children sets by scanning Parent on all entities.
// [DoNotSerialize] — SceneIO persists parent links via the entity `parent=` header attribute, not as a reflected field; reflection would double-write it.
struct Parent {
    // Structure: the link is there or it is not, so it has no off state.
    static constexpr bool NotToggleable = true;

    ECS::EntityHandle parent{}; // default (invalid handle) = no parent / root
};

// Optional sibling ordering hint for editor hierarchy (and scene serialization).
// Entities with lower order appear earlier under the same parent.
struct HierarchyOrder
{
    // Structure: a sibling position has no off state.
    static constexpr bool NotToggleable = true;

    std::int32_t order = 0;
};

} // namespace Components
} // namespace GameEngine

