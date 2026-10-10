#pragma once

// AbiHandles.h - Decodes the opaque values the GE_* C ABI passes for engine objects.

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Scripting/ScriptingABI.h"

#include <cstdint>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::ScriptingAbi
{

// A world handle is the World's address.
inline ECS::World* WorldFromHandle(GE_Handle worldHandle)
{
    return reinterpret_cast<ECS::World*>(static_cast<uintptr_t>(worldHandle));
}

// An entity id carries the packed EntityHandle::id (index and version) in its low 32 bits.
inline ECS::EntityHandle EntityFromId(uint64_t entityId)
{
    return ECS::EntityHandle(static_cast<uint32_t>(entityId));
}

// ABI GUID parameters are 16-byte arrays that decay to pointers at the C boundary.
inline GUID GuidFromAbiBytes(const uint8_t bytes[GUID::kSize])
{
    return GUID::FromBytes(*reinterpret_cast<const uint8 (*)[GUID::kSize]>(bytes));
}

} // namespace GameEngine::ScriptingAbi
