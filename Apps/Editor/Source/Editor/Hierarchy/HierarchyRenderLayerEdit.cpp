#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "ECS/World.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
const std::string kEditName = "Change Render Layer";

// Commits `mask` to the one component that owns the layer on this entity. `current`
// is taken by value because the commit reinserts the component and invalidates every
// pointer into it. An entity already standing at `mask` commits nothing, so a no-op
// row never enlarges the undo step.
template <typename TComponent, typename ApplyFn>
void CommitMaskTo(ECS::World& world,
                  ECS::EntityHandle entity,
                  uint32 current,
                  uint32 mask,
                  UndoRedoService* undo,
                  EditorChangeNotifications* notifications,
                  ApplyFn&& apply)
{
    if (current == mask)
        return;

    InspectorDrag::CommitComponentWithUndo<TComponent>(
        &world, entity, notifications, undo, kEditName, std::forward<ApplyFn>(apply));
}
}  // namespace

void ApplyRenderLayerMask(ECS::World& world,
                          const std::vector<ECS::EntityHandle>& entities,
                          uint32 mask,
                          UndoRedoService* undo,
                          EditorChangeNotifications* notifications)
{
    if (entities.empty())
        return;

    // One compound for the whole list. UndoRedoService::CommitInternal routes every
    // nested commit into the open compound, so N rows still cost the author one undo.
    if (undo)
        undo->BeginCompound(kEditName);

    for (const ECS::EntityHandle entity : entities)
    {
        if (!entity.IsValid() || !world.IsValid(entity))
            continue;

        if (const auto* layer = world.GetComponent<Components::RenderLayer>(entity))
        {
            CommitMaskTo<Components::RenderLayer>(
                world, entity, layer->mask, mask, undo, notifications,
                [mask](Components::RenderLayer& target) { target.mask = mask; });
        }
        else if (const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity))
        {
            CommitMaskTo<Components::MeshRenderer>(
                world, entity, renderer->renderLayerMask, mask, undo, notifications,
                [mask](Components::MeshRenderer& target) { target.renderLayerMask = mask; });
        }
        else if (const auto* skinned = world.GetComponent<Components::SkinnedMeshRenderer>(entity))
        {
            CommitMaskTo<Components::SkinnedMeshRenderer>(
                world, entity, skinned->renderLayerMask, mask, undo, notifications,
                [mask](Components::SkinnedMeshRenderer& target) { target.renderLayerMask = mask; });
        }
    }

    if (undo)
        undo->EndCompound();
}

}  // namespace GameEngine::Editor
