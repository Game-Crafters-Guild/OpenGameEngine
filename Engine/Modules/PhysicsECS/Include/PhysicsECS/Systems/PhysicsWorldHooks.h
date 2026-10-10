#pragma once

namespace GameEngine::Components
{
struct PhysicsBody;
struct CharacterController;
} // namespace GameEngine::Components

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::PhysicsECS
{

/// Destroys the backend body and every shape a PhysicsBody component owns, and
/// clears its runtime handles so a second call is a no-op.
///
/// Self-sufficient from the component alone, and that is a hard constraint, not
/// a style preference. Removal hooks run with the world's mutex held
/// EXCLUSIVELY, and it is a non-recursive shared_mutex that World::GetComponent
/// takes a shared lock on — so resolving a sibling component off the dying
/// entity would self-deadlock on the first call, unconditionally. PhysicsBody
/// carries its body, shape and child shapes, so nothing needs looking up.
void ReleasePhysicsBody(Components::PhysicsBody& body);

/// Destroys the backend character a CharacterController owns. Same mutex
/// constraint as ReleasePhysicsBody: no sibling lookups.
void ReleaseCharacterController(Components::CharacterController& character);

/// Registers the above as World::OnRemove for PhysicsBody and CharacterController,
/// and subscribes both types' lifecycle events: the init and character systems
/// read GetDisabled<T>() to release a body or character whose component went off.
void RegisterPhysicsWorldHooks(ECS::World& world);

} // namespace GameEngine::PhysicsECS
