#include "Components/Animation/Animator.h"
#include "Components/GameLogicGraphRef.h"
#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Components/PolyhavenPlaceholder.h"
#include "Components/Rendering/AmbientLight.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Skybox.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h" // typed World operation definitions
#include "ECS/Entity.h"
#include "ECS/World.h"

using GameEngine::Components::AmbientLight;
using GameEngine::Components::Animator;
using GameEngine::Components::GameLogicGraphRef;
using GameEngine::Components::HierarchyOrder;
using GameEngine::Components::Light;
using GameEngine::Components::LocalBounds;
using GameEngine::Components::MeasureComponent;
using GameEngine::Components::MeshRenderer;
using GameEngine::Components::Name;
using GameEngine::Components::Parent;
using GameEngine::Components::ParticleCollisionEventsBuffer;
using GameEngine::Components::ParticleEmitter3D;
using GameEngine::Components::PolyhavenPlaceholder;
using GameEngine::Components::SceneEntityTag;
using GameEngine::Components::Skybox;
using GameEngine::Components::SkyEnvironment;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;

// Explicit template instantiations for components that are used by the editor but
// live in the Engine library, to satisfy the linker and ensure handlers exist.

namespace GameEngine::ECS
{

template const Disabled* World::GetComponent<Disabled>(EntityHandle) const;
template const SceneEntityTag* World::GetComponent<SceneEntityTag>(EntityHandle) const;

template void World::AddComponent<Parent>(EntityHandle, const Parent&);
template void World::AddComponentImmediate<Parent>(EntityHandle, const Parent&);
template Parent* World::GetComponentForWrite<Parent>(EntityHandle);
template const Parent* World::GetComponent<Parent>(EntityHandle) const;
template bool World::HasComponent<Parent>(EntityHandle) const;
template void World::RemoveComponent<Parent>(EntityHandle);

template void World::AddComponent<HierarchyOrder>(EntityHandle, const HierarchyOrder&);
template void World::AddComponentImmediate<HierarchyOrder>(EntityHandle, const HierarchyOrder&);
template HierarchyOrder* World::GetComponentForWrite<HierarchyOrder>(EntityHandle);
template const HierarchyOrder* World::GetComponent<HierarchyOrder>(EntityHandle) const;
template bool World::HasComponent<HierarchyOrder>(EntityHandle) const;
template void World::RemoveComponent<HierarchyOrder>(EntityHandle);

template void World::AddComponent<Name>(EntityHandle, const Name&);
template void World::AddComponentImmediate<Name>(EntityHandle, const Name&);
template Name* World::GetComponentForWrite<Name>(EntityHandle);
template const Name* World::GetComponent<Name>(EntityHandle) const;
template bool World::HasComponent<Name>(EntityHandle) const;
template void World::RemoveComponent<Name>(EntityHandle);

template void World::AddComponent<Transform>(EntityHandle, const Transform&);
template void World::AddComponentImmediate<Transform>(EntityHandle, const Transform&);
template Transform* World::GetComponentForWrite<Transform>(EntityHandle);
template const Transform* World::GetComponent<Transform>(EntityHandle) const;
template bool World::HasComponent<Transform>(EntityHandle) const;
template void World::RemoveComponent<Transform>(EntityHandle);

template void World::AddComponent<WorldTransform>(EntityHandle, const WorldTransform&);
template void World::AddComponentImmediate<WorldTransform>(EntityHandle, const WorldTransform&);
template WorldTransform* World::GetComponentForWrite<WorldTransform>(EntityHandle);
template const WorldTransform* World::GetComponent<WorldTransform>(EntityHandle) const;
template bool World::HasComponent<WorldTransform>(EntityHandle) const;
template void World::RemoveComponent<WorldTransform>(EntityHandle);

template void World::AddComponent<Light>(EntityHandle, const Light&);
template void World::AddComponentImmediate<Light>(EntityHandle, const Light&);
template Light* World::GetComponentForWrite<Light>(EntityHandle);
template const Light* World::GetComponent<Light>(EntityHandle) const;
template bool World::HasComponent<Light>(EntityHandle) const;
template void World::RemoveComponent<Light>(EntityHandle);

template void World::AddComponent<AmbientLight>(EntityHandle, const AmbientLight&);
template void World::AddComponentImmediate<AmbientLight>(EntityHandle, const AmbientLight&);
template AmbientLight* World::GetComponentForWrite<AmbientLight>(EntityHandle);
template const AmbientLight* World::GetComponent<AmbientLight>(EntityHandle) const;
template bool World::HasComponent<AmbientLight>(EntityHandle) const;
template void World::RemoveComponent<AmbientLight>(EntityHandle);

template void World::AddComponent<LocalBounds>(EntityHandle, const LocalBounds&);
template void World::AddComponentImmediate<LocalBounds>(EntityHandle, const LocalBounds&);
template LocalBounds* World::GetComponentForWrite<LocalBounds>(EntityHandle);
template const LocalBounds* World::GetComponent<LocalBounds>(EntityHandle) const;
template bool World::HasComponent<LocalBounds>(EntityHandle) const;
template void World::RemoveComponent<LocalBounds>(EntityHandle);

template void World::AddComponent<MeshRenderer>(EntityHandle, const MeshRenderer&);
template void World::AddComponentImmediate<MeshRenderer>(EntityHandle, const MeshRenderer&);
template MeshRenderer* World::GetComponentForWrite<MeshRenderer>(EntityHandle);
template const MeshRenderer* World::GetComponent<MeshRenderer>(EntityHandle) const;
template bool World::HasComponent<MeshRenderer>(EntityHandle) const;
template void World::RemoveComponent<MeshRenderer>(EntityHandle);

template void World::AddComponent<ParticleEmitter3D>(EntityHandle, const ParticleEmitter3D&);
template void World::AddComponentImmediate<ParticleEmitter3D>(EntityHandle, const ParticleEmitter3D&);
template ParticleEmitter3D* World::GetComponentForWrite<ParticleEmitter3D>(EntityHandle);
template const ParticleEmitter3D* World::GetComponent<ParticleEmitter3D>(EntityHandle) const;
template bool World::HasComponent<ParticleEmitter3D>(EntityHandle) const;
template void World::RemoveComponent<ParticleEmitter3D>(EntityHandle);

template void World::AddComponent<ParticleCollisionEventsBuffer>(EntityHandle, const ParticleCollisionEventsBuffer&);
template void World::AddComponentImmediate<ParticleCollisionEventsBuffer>(EntityHandle, const ParticleCollisionEventsBuffer&);
template ParticleCollisionEventsBuffer* World::GetComponentForWrite<ParticleCollisionEventsBuffer>(EntityHandle);
template const ParticleCollisionEventsBuffer* World::GetComponent<ParticleCollisionEventsBuffer>(EntityHandle) const;
template bool World::HasComponent<ParticleCollisionEventsBuffer>(EntityHandle) const;
template void World::RemoveComponent<ParticleCollisionEventsBuffer>(EntityHandle);
template void World::RemoveComponentImmediate<ParticleCollisionEventsBuffer>(EntityHandle);

template void World::AddComponent<MeasureComponent>(EntityHandle, const MeasureComponent&);
template void World::AddComponentImmediate<MeasureComponent>(EntityHandle, const MeasureComponent&);
template MeasureComponent* World::GetComponentForWrite<MeasureComponent>(EntityHandle);
template const MeasureComponent* World::GetComponent<MeasureComponent>(EntityHandle) const;
template bool World::HasComponent<MeasureComponent>(EntityHandle) const;
template void World::RemoveComponent<MeasureComponent>(EntityHandle);

template void World::AddComponent<SkyEnvironment>(EntityHandle, const SkyEnvironment&);
template void World::AddComponentImmediate<SkyEnvironment>(EntityHandle, const SkyEnvironment&);
template SkyEnvironment* World::GetComponentForWrite<SkyEnvironment>(EntityHandle);
template const SkyEnvironment* World::GetComponent<SkyEnvironment>(EntityHandle) const;
template bool World::HasComponent<SkyEnvironment>(EntityHandle) const;
template void World::RemoveComponent<SkyEnvironment>(EntityHandle);

template void World::AddComponent<Skybox>(EntityHandle, const Skybox&);
template void World::AddComponentImmediate<Skybox>(EntityHandle, const Skybox&);
template Skybox* World::GetComponentForWrite<Skybox>(EntityHandle);
template const Skybox* World::GetComponent<Skybox>(EntityHandle) const;
template bool World::HasComponent<Skybox>(EntityHandle) const;
template void World::RemoveComponent<Skybox>(EntityHandle);

template void World::AddComponent<Animator>(EntityHandle, const Animator&);
template void World::AddComponentImmediate<Animator>(EntityHandle, const Animator&);
template Animator* World::GetComponentForWrite<Animator>(EntityHandle);
template const Animator* World::GetComponent<Animator>(EntityHandle) const;
template bool World::HasComponent<Animator>(EntityHandle) const;
template void World::RemoveComponent<Animator>(EntityHandle);

template void World::AddComponent<GameLogicGraphRef>(EntityHandle, const GameLogicGraphRef&);
template void World::AddComponentImmediate<GameLogicGraphRef>(EntityHandle, const GameLogicGraphRef&);
template GameLogicGraphRef* World::GetComponentForWrite<GameLogicGraphRef>(EntityHandle);
template const GameLogicGraphRef* World::GetComponent<GameLogicGraphRef>(EntityHandle) const;
template bool World::HasComponent<GameLogicGraphRef>(EntityHandle) const;
template void World::RemoveComponent<GameLogicGraphRef>(EntityHandle);

template void World::AddComponent<PolyhavenPlaceholder>(EntityHandle, const PolyhavenPlaceholder&);
template void World::AddComponentImmediate<PolyhavenPlaceholder>(EntityHandle, const PolyhavenPlaceholder&);
template PolyhavenPlaceholder* World::GetComponentForWrite<PolyhavenPlaceholder>(EntityHandle);
template const PolyhavenPlaceholder* World::GetComponent<PolyhavenPlaceholder>(EntityHandle) const;
template bool World::HasComponent<PolyhavenPlaceholder>(EntityHandle) const;
template void World::RemoveComponent<PolyhavenPlaceholder>(EntityHandle);

} // namespace GameEngine::ECS
