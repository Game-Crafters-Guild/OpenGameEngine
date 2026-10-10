// Entities as a page sees them: ge_entity_create, ge_entity_destroy, ge_entity_parent,
// ge_entity_children, ge_entity_bounds and ge_entity_set_mesh (Apps/WebLibrary/ts/src/abi.ts).

#include "AbiEntities.h"

#include "AbiErrors.h"
#include "AbiLifecycle.h"
#include "AbiQuery.h"
#include "WebLibraryApplication.h"

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/PrimitiveGenerator.h"

#include <emscripten/emscripten.h>

#include <array>
#include <string_view>
#include <vector>

namespace GameEngine::WebLibrary
{

ECS::World* WorldForCall(std::string_view call)
{
    if (RefuseAfterShutdown(call))
        return nullptr;
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        SetLastError("{} was called before ge_create succeeded: there is no world yet.", call);
    return world;
}

bool ResolveEntity(const ECS::World& world, uint32_t id, std::string_view call, ECS::EntityHandle& outEntity)
{
    const ECS::EntityHandle entity(id);
    if (!entity.IsValid() || !world.IsValid(entity))
    {
        SetLastError("{}: entity {} does not exist (it was never created, or it was destroyed).", call, id);
        return false;
    }
    outEntity = entity;
    return true;
}

bool SetEntityParent(ECS::World& world, ECS::EntityHandle child, ECS::EntityHandle parent, std::string_view call)
{
    if (!parent.IsValid())
    {
        if (world.HasComponent<Components::Parent>(child))
            world.RemoveComponentImmediate<Components::Parent>(child);
        return true;
    }
    if (parent == child)
    {
        SetLastError("{}: an entity cannot be its own parent.", call);
        return false;
    }
    for (const ECS::EntityHandle descendant : Components::DescendantsOf(world, child))
    {
        if (descendant == parent)
        {
            SetLastError("{}: entity {} is a descendant of entity {}; parenting it there would make a cycle.", call,
                         parent.id, child.id);
            return false;
        }
    }
    world.AddComponentImmediate(child, Components::Parent{parent});
    return true;
}

// The built-in mesh a page names, by PrimitiveGenerator::GuidFromName's spelling; null for any
// other name. A page gets the four solid primitives; the sprite plane is the editor's.
GUID PageMeshGuid(std::string_view name)
{
    using Engine::Renderer::PrimitiveGenerator;
    const GUID guid = PrimitiveGenerator::GuidFromName(name);
    const std::array<GUID, 4> pageMeshes = {PrimitiveGenerator::PlaneGuid(), PrimitiveGenerator::CubeGuid(),
                                            PrimitiveGenerator::SphereGuid(), PrimitiveGenerator::CapsuleGuid()};
    for (const GUID& mesh : pageMeshes)
        if (mesh == guid)
            return guid;
    return GUID::Null();
}

} // namespace GameEngine::WebLibrary

using namespace GameEngine;
using namespace GameEngine::WebLibrary;

extern "C"
{

/// Creates an entity with an identity Transform; kInvalidEntity on failure.
EMSCRIPTEN_KEEPALIVE uint32_t ge_entity_create()
{
    const AbiCallScope scope("ge_entity_create");
    if (scope.Refused())
        return ECS::kInvalidEntity;
    if (RefuseWhileQueryRuns("ge_entity_create"))
        return ECS::kInvalidEntity;
    ECS::World* world = WorldForCall("ge_entity_create");
    if (!world)
        return ECS::kInvalidEntity;
    const ECS::EntityHandle entity = world->CreateEntity();
    if (!entity.IsValid())
    {
        SetLastError("ge_entity_create: the world could not create an entity.");
        return ECS::kInvalidEntity;
    }
    world->AddComponentImmediate(entity, Components::Transform{});
    return entity.id;
}

/// Writes the LocalBounds box (center, then half extents; six float32) to `out`. 1 when
/// written, 0 when the entity has no LocalBounds, kFailed otherwise.
EMSCRIPTEN_KEEPALIVE int32_t ge_entity_bounds(uint32_t entityId, float* out)
{
    const AbiCallScope scope("ge_entity_bounds");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = WorldForCall("ge_entity_bounds");
    ECS::EntityHandle entity;
    if (!world || !ResolveEntity(*world, entityId, "ge_entity_bounds", entity))
        return kFailed;
    if (!out)
    {
        SetLastError("ge_entity_bounds needs an output buffer of six floats.");
        return kFailed;
    }
    const auto* bounds = world->GetComponent<Components::LocalBounds>(entity);
    if (!bounds)
        return 0;
    const auto& center = bounds->Box.center;
    const auto& extents = bounds->Box.halfExtents;
    const float values[6] = {center.x, center.y, center.z, extents.x, extents.y, extents.z};
    for (int i = 0; i < 6; ++i)
        out[i] = values[i];
    return 1;
}

/// Destroys the entity and its descendants.
EMSCRIPTEN_KEEPALIVE int32_t ge_entity_destroy(uint32_t entityId)
{
    const AbiCallScope scope("ge_entity_destroy");
    if (scope.Refused())
        return kFailed;
    if (RefuseWhileQueryRuns("ge_entity_destroy"))
        return kFailed;
    ECS::World* world = WorldForCall("ge_entity_destroy");
    ECS::EntityHandle entity;
    if (!world || !ResolveEntity(*world, entityId, "ge_entity_destroy", entity))
        return kFailed;
    std::vector<ECS::EntityHandle> doomed = Components::DescendantsOf(*world, entity);
    doomed.push_back(entity);
    for (const ECS::EntityHandle e : doomed)
        world->DestroyEntityImmediate(e);
    return kOk;
}

/// Writes the ids of the entity's direct children to `out`, at most `capacity` of them, and returns
/// how many it has (more than `capacity` means a larger buffer is needed); kFailed on failure.
EMSCRIPTEN_KEEPALIVE int32_t ge_entity_children(uint32_t entityId, uint32_t* out, uint32_t capacity)
{
    const AbiCallScope scope("ge_entity_children");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = WorldForCall("ge_entity_children");
    ECS::EntityHandle entity;
    if (!world || !ResolveEntity(*world, entityId, "ge_entity_children", entity))
        return kFailed;
    if (!out && capacity > 0)
    {
        SetLastError("ge_entity_children needs an output buffer of {} entity ids.", capacity);
        return kFailed;
    }
    const std::vector<ECS::EntityHandle> children = Components::ChildrenOf(*world, entity);
    for (std::size_t i = 0; i < children.size() && i < capacity; ++i)
        out[i] = children[i].id;
    return static_cast<int32_t>(children.size());
}

/// Gives the entity a built-in mesh ("plane", "cube", "sphere" or "capsule", the unit-sized
/// primitives the renderer registers at start-up) drawn with the engine's default material: a
/// MeshRenderer bound to the registered GPU mesh and the mesh's LocalBounds, replacing any it had.
EMSCRIPTEN_KEEPALIVE int32_t ge_entity_set_mesh(uint32_t entityId, const char* meshName)
{
    const AbiCallScope scope("ge_entity_set_mesh");
    if (scope.Refused())
        return kFailed;
    constexpr const char* kCall = "ge_entity_set_mesh";
    ECS::World* world = WorldForCall(kCall);
    ECS::EntityHandle entity;
    if (!world || !ResolveEntity(*world, entityId, kCall, entity))
        return kFailed;
    const std::string_view name = meshName ? std::string_view(meshName) : std::string_view();
    const GUID meshGuid = PageMeshGuid(name);
    if (meshGuid.IsNull())
    {
        SetLastError("{}: '{}' is not a built-in mesh; use 'plane', 'cube', 'sphere' or 'capsule'.", kCall, name);
        return kFailed;
    }
    WebLibraryApplication* application = GetRunningApplication();
    Engine::Renderer::RenderServices* renderServices = application ? application->GetRenderServices() : nullptr;
    if (!renderServices)
    {
        SetLastError("{} needs the renderer that ge_create brings up.", kCall);
        return kFailed;
    }
    using Engine::Renderer::PrimitiveGenerator;
    world->AddComponentImmediate(entity, PrimitiveGenerator::MakePrimitiveMeshRenderer(
                                             renderServices, meshGuid, PrimitiveGenerator::DefaultMaterialGuid()));
    world->AddComponentImmediate(entity, PrimitiveGenerator::MakePrimitiveLocalBounds(renderServices, meshGuid));
    return kOk;
}

/// Reparents the entity; kInvalidEntity makes it a root.
EMSCRIPTEN_KEEPALIVE int32_t ge_entity_parent(uint32_t entityId, uint32_t parentId)
{
    const AbiCallScope scope("ge_entity_parent");
    if (scope.Refused())
        return kFailed;
    if (RefuseWhileQueryRuns("ge_entity_parent"))
        return kFailed;
    ECS::World* world = WorldForCall("ge_entity_parent");
    ECS::EntityHandle entity;
    if (!world || !ResolveEntity(*world, entityId, "ge_entity_parent", entity))
        return kFailed;
    ECS::EntityHandle parent;
    if (parentId != ECS::kInvalidEntity && !ResolveEntity(*world, parentId, "ge_entity_parent", parent))
        return kFailed;
    return SetEntityParent(*world, entity, parent, "ge_entity_parent") ? kOk : kFailed;
}

} // extern "C"
