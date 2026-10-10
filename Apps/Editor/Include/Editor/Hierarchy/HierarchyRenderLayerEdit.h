#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// Writes `mask` into whichever component carries the render layer on each entity and
/// records the whole list as ONE undo step, so a multi-row edit undoes in one press.
///
/// The component search order is RenderLayer, then MeshRenderer, then
/// SkinnedMeshRenderer — the same order the hierarchy row reads to populate its
/// dropdown, so the row and the edit never disagree about which component owns the
/// mask. An entity that already holds `mask`, and an entity with none of the three
/// components, contributes nothing and leaves the undo step no larger.
void ApplyRenderLayerMask(
    ECS::World& world,
    const std::vector<ECS::EntityHandle>& entities,
    uint32 mask,
    UndoRedoService* undo,
    EditorChangeNotifications* notifications);

}  // namespace GameEngine::Editor
