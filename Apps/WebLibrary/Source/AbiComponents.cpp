// Components and their fields as a page sees them: ge_component_add, ge_component_remove,
// ge_component_has, ge_field_get and ge_field_set (Apps/WebLibrary/ts/src/abi.ts). A field
// crosses as its bytes, `size` bytes as ge_reflection_json reports them.

#include "AbiEntities.h"
#include "AbiErrors.h"
#include "AbiQuery.h"
#include "PageReflection.h"

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Entity.h"

#include <emscripten/emscripten.h>

#include <cstring>
#include <vector>

namespace GameEngine::WebLibrary
{

namespace
{

// The world, the entity and the component a component call names; false, with the last
// error set, when any of them is not there.
bool ResolveTarget(std::string_view call, uint32_t entityId, uint64_t typeId, ECS::World*& outWorld,
                   ECS::EntityHandle& outEntity, const PageComponent*& outComponent)
{
    outWorld = WorldForCall(call);
    if (!outWorld || !ResolveEntity(*outWorld, entityId, call, outEntity))
        return false;
    outComponent = FindPageComponent(typeId);
    if (!outComponent)
    {
        SetLastError("{}: {} is not a component this engine reflects; ge_reflection_json lists the ones it does.",
                     call, typeId);
        return false;
    }
    return true;
}

// The component bytes the field calls read and patch: one buffer for the module, grown to the
// largest component a call has touched, so the per-frame field calls allocate nothing.
std::vector<uint8_t>& ComponentScratch()
{
    static std::vector<uint8_t> scratch;
    return scratch;
}

// The field `fieldId` of a component the entity has, with the component's current bytes in
// ComponentScratch() (its first `outSize` bytes).
bool ReadComponentField(std::string_view call, uint32_t entityId, uint64_t typeId, uint32_t fieldId,
                        ECS::World*& outWorld, ECS::EntityHandle& outEntity, const ECS::FieldInfo*& outField,
                        std::size_t& outSize)
{
    const PageComponent* component = nullptr;
    if (!ResolveTarget(call, entityId, typeId, outWorld, outEntity, component))
        return false;
    if (fieldId >= component->Fields.size())
    {
        SetLastError("{}: {} has {} fields; field {} does not exist.", call, component->Name,
                     component->Fields.size(), fieldId);
        return false;
    }
    std::vector<uint8_t>& scratch = ComponentScratch();
    bool read = outWorld->ReadComponentBytes(outEntity, typeId, scratch.data(), scratch.size(), outSize);
    if (read && outSize > scratch.size())
    {
        scratch.resize(outSize);
        read = outWorld->ReadComponentBytes(outEntity, typeId, scratch.data(), scratch.size(), outSize);
    }
    if (!read)
    {
        SetLastError("{}: entity {} has no {}.", call, entityId, component->Name);
        return false;
    }
    outField = component->Fields[fieldId];
    if (static_cast<size_t>(outField->Offset) + outField->Size > outSize)
    {
        SetLastError("{}: {}.{} lies outside the component's bytes.", call, component->Name, outField->Name);
        return false;
    }
    return true;
}

} // namespace

} // namespace GameEngine::WebLibrary

using namespace GameEngine;
using namespace GameEngine::WebLibrary;

extern "C"
{

/// Adds the component with its defaults; fails when the entity already has it.
EMSCRIPTEN_KEEPALIVE int32_t ge_component_add(uint32_t entityId, uint64_t typeId)
{
    const AbiCallScope scope("ge_component_add");
    if (scope.Refused())
        return kFailed;
    if (RefuseWhileQueryRuns("ge_component_add"))
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const PageComponent* component = nullptr;
    if (!ResolveTarget("ge_component_add", entityId, typeId, world, entity, component))
        return kFailed;
    if (world->HasComponent(entity, typeId))
    {
        SetLastError("ge_component_add: entity {} already has a {}.", entityId, component->Name);
        return kFailed;
    }
    // The factory reports success for a creator that adds nothing: a component the ECS has no
    // handler for (none until its first typed use) cannot be added by type id.
    if (!ECS::ComponentFactory::Create(*world, entity, typeId) || !world->HasComponent(entity, typeId))
    {
        SetLastError("ge_component_add: {} cannot be added by its type id in this engine build.", component->Name);
        return kFailed;
    }
    return kOk;
}

/// Removes the component; fails when the entity does not have it.
EMSCRIPTEN_KEEPALIVE int32_t ge_component_remove(uint32_t entityId, uint64_t typeId)
{
    const AbiCallScope scope("ge_component_remove");
    if (scope.Refused())
        return kFailed;
    if (RefuseWhileQueryRuns("ge_component_remove"))
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const PageComponent* component = nullptr;
    if (!ResolveTarget("ge_component_remove", entityId, typeId, world, entity, component))
        return kFailed;
    if (!world->HasComponent(entity, typeId))
    {
        SetLastError("ge_component_remove: entity {} has no {}.", entityId, component->Name);
        return kFailed;
    }
    if (!world->RemoveComponentByTypeIdImmediate(entity, typeId))
    {
        SetLastError("ge_component_remove: {} could not be removed from entity {}.", component->Name, entityId);
        return kFailed;
    }
    return kOk;
}

/// 1 when the entity has the component, 0 when not, kFailed on failure.
EMSCRIPTEN_KEEPALIVE int32_t ge_component_has(uint32_t entityId, uint64_t typeId)
{
    const AbiCallScope scope("ge_component_has");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const PageComponent* component = nullptr;
    if (!ResolveTarget("ge_component_has", entityId, typeId, world, entity, component))
        return kFailed;
    return world->HasComponent(entity, typeId) ? 1 : 0;
}

/// Copies the field's bytes to `out`.
EMSCRIPTEN_KEEPALIVE int32_t ge_field_get(uint32_t entityId, uint64_t typeId, uint32_t fieldId, uint8_t* out)
{
    const AbiCallScope scope("ge_field_get");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const ECS::FieldInfo* field = nullptr;
    std::size_t size = 0;
    if (!ReadComponentField("ge_field_get", entityId, typeId, fieldId, world, entity, field, size))
        return kFailed;
    if (!out)
    {
        SetLastError("ge_field_get needs an output buffer of {} bytes.", field->Size);
        return kFailed;
    }
    std::memcpy(out, ComponentScratch().data() + field->Offset, field->Size);
    return kOk;
}

/// Copies the field's bytes from `value`. A read-only field refuses, and so does a String
/// value with no NUL in its bytes.
EMSCRIPTEN_KEEPALIVE int32_t ge_field_set(uint32_t entityId, uint64_t typeId, uint32_t fieldId, const uint8_t* value)
{
    const AbiCallScope scope("ge_field_set");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const ECS::FieldInfo* field = nullptr;
    std::size_t size = 0;
    if (!ReadComponentField("ge_field_set", entityId, typeId, fieldId, world, entity, field, size))
        return kFailed;
    if (!value)
    {
        SetLastError("ge_field_set needs the field's {} bytes.", field->Size);
        return kFailed;
    }
    if (ECS::HasAnyFlag(field->Flags, ECS::FieldFlags::ReadOnly))
    {
        SetLastError("ge_field_set: {} is read-only.", field->Name);
        return kFailed;
    }
    if (field->Type == ECS::FieldTypeId::String && std::memchr(value, 0, field->Size) == nullptr)
    {
        SetLastError("ge_field_set: {} holds at most {} bytes of UTF-8 including its terminating NUL.", field->Name,
                     field->Size);
        return kFailed;
    }
    std::vector<uint8_t>& bytes = ComponentScratch();
    std::memcpy(bytes.data() + field->Offset, value, field->Size);
    if (!world->SetComponentBytesImmediate(entity, typeId, bytes.data(), size))
    {
        SetLastError("ge_field_set: the engine refused the new value of {}.", field->Name);
        return kFailed;
    }
    return kOk;
}

} // extern "C"
