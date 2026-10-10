#include "VersionControl/SceneDiffLiveOverlay.h"

#include "Components/Hierarchy.h"
#include "Components/SceneEntityTag.h"
#include "ECS/World.h"
#include "Scene/SceneSchemaRegistry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine::Editor
{
namespace
{
std::string_view TrimSceneLine(std::string_view value)
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

// Serializes the entity through the same schemas the scene writer uses, so the
// keys line up with the "Component.field" keys BuildSceneDiff parses.
std::unordered_map<std::string, std::string> SerializeLiveProperties(
    const ECS::World& world, ECS::EntityHandle entity)
{
    Scene::SceneSaveContext saveContext{};
    std::vector<std::string> lines;
    for (const Scene::ISceneComponentSchema* schema :
         Scene::SceneSchemaRegistry::GetAllSorted())
        if (schema)
            schema->Serialize(world, entity, saveContext, lines);

    if (ECS::Archetype* archetype = world.GetEntityArchetype(entity))
        for (const ECS::ComponentTypeId typeId :
             archetype->GetSignature().GetComponents())
            if (const Scene::ISceneComponentSchema* schema =
                    Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(typeId))
                schema->Serialize(world, entity, saveContext, lines);

    std::unordered_map<std::string, std::string> properties;
    for (const std::string& line : lines)
    {
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        properties[std::string(
            TrimSceneLine(std::string_view(line).substr(0, equals)))] =
            std::string(
                TrimSceneLine(std::string_view(line).substr(equals + 1)));
    }
    return properties;
}
// The scene id of the entity's current parent, empty at the root. Parenting
// never appears in the serialized property set, so it has to be read from the
// world directly.
std::string LiveParentSceneId(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* parent = world.GetComponent<Components::Parent>(entity);
    if (!parent || !parent->parent.IsValid() || !world.IsValid(parent->parent))
        return {};
    const auto* tag =
        world.GetComponent<Components::SceneEntityTag>(parent->parent);
    return tag && tag->value[0] != '\0' ? std::string(tag->View()) : std::string{};
}
} // namespace

SceneObjectDiff OverlaySceneDiffWithLiveValues(SceneObjectDiff object,
                                               const ECS::World& world,
                                               ECS::EntityHandle entity)
{
    const auto live = SerializeLiveProperties(world, entity);
    object.CurrentParent = LiveParentSceneId(world, entity);
    std::unordered_set<std::string> seen;
    bool anyChanged = object.WasReparented();
    for (ScenePropertyDiff& property : object.Properties)
    {
        const auto liveIt = live.find(property.Key);
        if (liveIt == live.end())
            continue;
        seen.insert(property.Key);
        const bool authoredRemoved =
            property.State == SceneDiffState::Removed &&
            property.CurrentValue.empty();
        if (authoredRemoved)
        {
            anyChanged = true;
            continue;
        }
        property.CurrentValue = liveIt->second;
        if (property.State != SceneDiffState::Added ||
            !property.OriginalValue.empty())
            property.State =
                property.OriginalValue == property.CurrentValue
                    ? SceneDiffState::Unchanged
                    : SceneDiffState::Modified;
        anyChanged |= property.State != SceneDiffState::Unchanged;
    }
    for (const auto& [key, value] : live)
    {
        if (seen.contains(key))
            continue;
        const size_t dot = key.find('.');
        const std::string_view component =
            dot == std::string::npos ? std::string_view(key)
                                     : std::string_view(key).substr(0, dot);
        const bool componentAlreadyAuthored = std::any_of(
            object.Properties.begin(), object.Properties.end(),
            [component](const ScenePropertyDiff& property)
            {
                const size_t propertyDot = property.Key.find('.');
                return propertyDot != std::string::npos &&
                       std::string_view(property.Key).substr(0, propertyDot) ==
                           component &&
                       (!property.OriginalValue.empty() ||
                        !property.CurrentValue.empty());
            });
        if (componentAlreadyAuthored)
            continue;
        ScenePropertyDiff property;
        property.Key = key;
        property.CurrentValue = value;
        property.State = SceneDiffState::Added;
        object.Properties.push_back(std::move(property));
        anyChanged = true;
    }
    object.State =
        anyChanged
            ? (object.State == SceneDiffState::Added ? SceneDiffState::Added
                                                     : SceneDiffState::Modified)
            : SceneDiffState::Unchanged;
    return object;
}

} // namespace GameEngine::Editor
