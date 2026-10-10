#pragma once

#include "ECS/ECS.h"

#include <cstdint>

namespace GameEngine
{
class TreeView;
class UIElement;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// The Hierarchy row's preview icon: the row's child carrying `hierarchy-preview-icon`, or nullptr
/// before the row's first bind has created it.
UIElement* FindHierarchyPreviewIcon(const UIElement& row);

/// Shows `entity`'s Light color on its row's preview icon while the UI root carries
/// `hierarchy-icons-colored`, at the brightness of the uncolored icon tint; an entity with no Light
/// gets that neutral tint. Without the class the inline tint is cleared, so the style sheet applies.
void ApplyHierarchyLightIconTint(ECS::World& world, ECS::EntityHandle entity, UIElement& icon);

/// Retints, in place, the row `tree` holds for `treeId` (the row showing `entity`), and does nothing
/// when the tree holds none. It rebinds no row: a Light edit notifies from inside the Inspector's
/// pointer event, where a provider refresh would rebind every row the tree holds.
void RetintHeldHierarchyLightRow(ECS::World& world, ECS::EntityHandle entity, const TreeView& tree,
                                 std::uint64_t treeId);

} // namespace GameEngine::Editor
