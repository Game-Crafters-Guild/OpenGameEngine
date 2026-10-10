#pragma once

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/Entity.h"

#include <functional>
#include <string_view>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// Appends the notice a field carries when this build could not read the value the scene authored
// for it, and does nothing for any other field.
//
// The field shows the value it fell back to, which on its own is indistinguishable from a value the
// user chose — while a save silently writes the authored text back instead. That gap is the whole
// hazard: the widget says one thing and the file gets another. The notice states the authored text
// and that it could not be read, so the row stops looking authoritative.
//
// It also carries the only way to resolve the disagreement in favour of the live value. A save
// keeps the authored text while the field still holds exactly what it fell back to, and re-typing
// that same value is by construction invisible — so "I want the fallback" cannot be expressed by
// editing. Discarding is how it is expressed.
void AddPreservedFieldNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                             ECS::ComponentTypeId typeId, std::string_view fieldName,
                             std::function<void()> onDiscarded);

// Appends one notice per preserved field of `typeId`, naming each field, and does nothing when the
// component has none.
//
// Same three statements as the per-field notice above, hoisted to the component. The per-field form
// is emitted by the reflection-driven inspector, inline over the row it describes; a component with
// a registered CUSTOM inspector never runs that path, because the panel dispatches to the custom
// inspector or to the reflection fallback and never to both. Without this, every such component's
// preserved fields carry no notice, no authored text and no way to discard. The field has to be
// named here: this sits at the top of the section, with no adjacent row to speak for.
void AddPreservedComponentFieldsNotice(UIElement* parent, ECS::World& world,
                                       ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                                       std::function<void()> onDiscarded);

} // namespace GameEngine::Editor
