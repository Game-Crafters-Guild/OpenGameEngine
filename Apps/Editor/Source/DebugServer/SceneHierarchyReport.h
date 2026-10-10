#pragma once

#include <nlohmann/json.hpp>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// The `get_scene_hierarchy` debug response: every root entity with its children, each node
// {id, name, enabled, enabledInHierarchy, children}. `enabled` is the entity's own state;
// `enabledInHierarchy` is false when the entity or any ancestor is off, the derived state the
// hierarchy pass writes, so automation reads a child under a switched-off parent as inactive
// rather than as enabled.
nlohmann::json DescribeSceneHierarchy(ECS::World& world);

} // namespace GameEngine::Editor
