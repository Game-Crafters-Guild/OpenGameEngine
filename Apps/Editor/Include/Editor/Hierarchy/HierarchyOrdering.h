#pragma once

#include "Components/Hierarchy.h"
#include "ECS/ECS.h"

#include <span>

namespace GameEngine {
namespace Editor {

// The HierarchyOrder a new root entity needs to appear at the bottom of the
// hierarchy: one past the largest order currently in the world. Siblings sort
// by HierarchyOrder and a missing one reads as 0 (the top), so every place that
// creates a root for the user (create menu, asset drops, download placeholders)
// assigns this instead of leaving the new entity at the top.
Components::HierarchyOrder NextHierarchyOrderAtBottom(ECS::World* world);

// Append every entity in `roots` that has no HierarchyOrder yet, in the given
// order, after the current maximum. Entities that already carry an order (an
// existing entity a drop targeted, a placeholder the factory ordered) keep it.
// Scans the world once for the whole batch.
void AppendUnorderedRootsToHierarchyEnd(ECS::World& world, std::span<const ECS::EntityHandle> roots);

} // namespace Editor
} // namespace GameEngine
