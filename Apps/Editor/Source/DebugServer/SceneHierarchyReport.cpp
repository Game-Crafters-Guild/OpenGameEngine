#include "DebugServer/SceneHierarchyReport.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
using json = nlohmann::json;
using ChildrenByParent = std::unordered_map<std::uint32_t, std::vector<ECS::EntityHandle>>;

json DescribeEntity(ECS::World& world, ECS::EntityHandle entity, const ChildrenByParent& childrenByParent)
{
    const ECS::Entity handle(&world, entity);
    json node;
    node["id"] = entity.id;
    const auto* name = world.GetComponent<Components::Name>(entity);
    node["name"] = name ? std::string(name->View()) : std::string();
    node["enabled"] = handle.IsEnabled();
    node["enabledInHierarchy"] = handle.IsEnabledInHierarchy();

    json children = json::array();
    if (const auto it = childrenByParent.find(entity.id); it != childrenByParent.end())
    {
        for (const ECS::EntityHandle child : it->second)
            children.push_back(DescribeEntity(world, child, childrenByParent));
    }
    node["children"] = std::move(children);
    return node;
}
} // namespace

json DescribeSceneHierarchy(ECS::World& world)
{
    ChildrenByParent childrenByParent;
    std::vector<ECS::EntityHandle> roots;
    for (const ECS::EntityHandle entity : world.GetAliveEntitiesSnapshot())
    {
        const auto* parent = world.GetComponent<Components::Parent>(entity);
        if (parent && parent->parent.IsValid() && world.IsValid(parent->parent))
            childrenByParent[parent->parent.id].push_back(entity);
        else
            roots.push_back(entity);
    }

    json entities = json::array();
    for (const ECS::EntityHandle root : roots)
        entities.push_back(DescribeEntity(world, root, childrenByParent));
    json result;
    result["entities"] = std::move(entities);
    return result;
}

} // namespace GameEngine::Editor
