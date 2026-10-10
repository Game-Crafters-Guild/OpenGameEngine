#pragma once

#include "ECS/ECS.h"

#include <string>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// The entity's name as editor text shows it: its Name component, or its id label
/// (EntityIdLabel) when it has none or the name is empty.
std::string EntityDisplayName(const ECS::World& world, ECS::EntityHandle entity);

/// The entity's id as editor text shows it: "Entity <id>".
std::string EntityIdLabel(ECS::EntityHandle entity);

} // namespace GameEngine::Editor
