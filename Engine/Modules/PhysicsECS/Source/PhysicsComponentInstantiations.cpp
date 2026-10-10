// Explicit cross-DLL instantiation for PhysicsECS's public components.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
//
// Without this, macOS / Linux linkers fail to resolve `World::AddComponent<T>`
// symbols when Editor.exe inspectors
// (PhysicsBodyInspector, PhysicsColliderInspector, ...) and engine scene
// loaders (BuiltInSceneSchemas, TerrainPhysicsSystem) attach physics
// components.
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::BoxColliderShape);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::CapsuleColliderShape);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::CharacterController);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::HeightFieldColliderShape);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::PhysicsBody);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::PhysicsCollider);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::PhysicsColliderOwner);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::PhysicsWorldSettingsComponent);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::PlaneColliderShape);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::SphereColliderShape);
} // namespace GameEngine::ECS
