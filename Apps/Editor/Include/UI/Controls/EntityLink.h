#pragma once

#include "ECS/ECS.h"

#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::EditorUI
{

// What an entity link does with its entity.
struct EntityLinkActions
{
    // A click on the name selects the entity (frame false); a click on the frame glyph selects
    // and frames it in the Scene View (frame true).
    std::function<void(ECS::EntityHandle entity, bool frame)> Select;
    // The pointer entered the link (the entity) or left it (a null handle): the Scene View
    // highlights the entity meanwhile. Optional.
    std::function<void(ECS::EntityHandle entity)> Hover;
};

// What a link shows for an entity that is not in its world: `Text` muted, with `Tooltip`.
struct EntityLinkMissing
{
    std::string Text;
    std::string Tooltip;
};

// An entity link: a chip with the entity's name (EntityDisplayName) that selects it on a click and
// highlights it while hovered, with a frame glyph at its right end that frames it. An entity that
// is not in `world` is the muted `missing` text, which does nothing. Styled by
// UI/controls/EntityLink/EntityLink.css (classes entity-link, entity-link-name, entity-link-frame,
// entity-link-missing), which the link requests for itself.
std::unique_ptr<UIElement> MakeEntityLink(ECS::EntityHandle entity, const ECS::World& world,
                                          const EntityLinkActions& actions, const EntityLinkMissing& missing);

// The editor's own select, frame and hover for an entity link, for a surface that has no
// selection of its own (a package's panel): select through the Scene View's pick, frame its
// selection, highlight the hovered entity. Set by the editor at startup; empty actions (a link
// that does nothing) where no editor set them.
const EntityLinkActions& EditorEntityLinkActions();
void SetEditorEntityLinkActions(EntityLinkActions actions);

} // namespace GameEngine::EditorUI
