#pragma once

#include "ECS/ECS.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine::Scene
{
class ISceneComponentSchema;
}

namespace GameEngine::Editor
{

// Apply a `set_component` values object onto a component that has a hand-written scene schema
// (Terrain, ...) rather than GE_REFLECT field metadata. Drives the SAME ISceneComponentSchema the
// .scene loader uses, so the field set and its clamps stay single-sourced: each key is lower-cased
// to the schema's property name and each JSON value rendered as the schema text its parsers read.
// An empty values object adds the component with its defaults. The request is all or nothing for the
// entity: a refused key restores every component on it as the request found it.
// Returns an empty string on success, else the refusal message.
std::string ApplyComponentViaSchema(ECS::World& world, ECS::EntityHandle entity,
                                    const std::string& componentName,
                                    const Scene::ISceneComponentSchema& schema,
                                    const nlohmann::json& values);

} // namespace GameEngine::Editor
