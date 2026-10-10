#pragma once

#include "ECS/Entity.h"

#include <functional>

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

// Appends the read-only notice an entity carrying Components::RuntimeOnlyEntity
// gets at the top of the Inspector, and does nothing for any other entity.
//
// These entities are generator output — spline placement tiles, fence posts and
// spans, HLOD proxies, terrain collider tiles. They are rebuilt from their
// source and are excluded from the saved scene, so a hand edit to one survives
// neither a rebuild nor a reload. The notice states both halves rather than
// letting the fields look authoritative; blocking every field widget would cost
// a read-only path through the whole inspector for no extra truth.
//
// When the entity has an owner, the notice's action selects it (`selectEntity`, the inspector's
// InspectorContext::SelectEntity), since the owner is where the edit belongs.
void AddGeneratedEntityNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                              const std::function<void(ECS::EntityHandle)>& selectEntity);

} // namespace GameEngine::Editor
