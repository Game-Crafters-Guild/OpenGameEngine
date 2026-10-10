#pragma once

#include <cstdint>
#include <string_view>

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
} // namespace GameEngine::ECS

namespace GameEngine::WebLibrary
{

/// The primary world a call acts on; null, with the last error naming `call`, when the call
/// may not run (during ge_create, after ge_shutdown) or there is no world yet.
ECS::World* WorldForCall(std::string_view call);

/// The live entity `id` names in `world`; false, with the last error naming `call`, when it
/// names none.
bool ResolveEntity(const ECS::World& world, uint32_t id, std::string_view call, ECS::EntityHandle& outEntity);

/// Makes `parent` the parent of `child`, or makes `child` a root for an invalid `parent`. The
/// child keeps its local transform. False, with the last error naming `call`, when `parent`
/// is `child` or one of its descendants.
bool SetEntityParent(ECS::World& world, ECS::EntityHandle child, ECS::EntityHandle parent, std::string_view call);

} // namespace GameEngine::WebLibrary
