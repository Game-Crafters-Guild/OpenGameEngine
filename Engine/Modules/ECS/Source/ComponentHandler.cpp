#include "ECS/ComponentHandler.h"

#include "ECS/Entity.h"
#include "ECS/ReflectionJson.h"
#include "ECS/World.h"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

namespace GameEngine::ECS
{
ComponentHandler::ComponentHandler(ComponentTypeId typeId, std::string typeName,
                                   std::size_t componentSize, std::string opaqueTypeName,
                                   std::optional<std::span<const FieldInfo>> fields)
    : m_TypeId(typeId), m_TypeName(std::move(typeName)), m_ComponentSize(componentSize),
      m_OpaqueTypeName(std::move(opaqueTypeName)), m_ReflectedFields(fields)
{
}

ComponentHandler::~ComponentHandler() = default;

void ComponentHandler::CopyComponent(Archetype* from, uint16_t fromChunk, uint16_t fromIndex,
                                      Archetype* to, uint16_t toChunk, uint16_t toIndex)
{
    const void* source = from->GetComponentRawAt(fromChunk, fromIndex, m_TypeId);
    if (source)
        to->SetComponentRawAt(toChunk, toIndex, m_TypeId, source, m_ComponentSize);
}

void ComponentHandler::AddComponentFromData(World* world, EntityHandle entity,
                                            std::span<const uint8_t> data)
{
    if (data.size() != m_ComponentSize)
        return;
    world->AddComponentImpl(entity, m_TypeId, data.data(), m_ComponentSize);
}

void ComponentHandler::RemoveComponent(World* world, EntityHandle entity)
{
    world->RemoveComponentImpl(entity, m_TypeId);
}

std::vector<uint8_t> ComponentHandler::SerializeComponent(Archetype* archetype, uint16_t chunkIndex,
                                                         uint16_t indexInChunk)
{
    const void* component = archetype->GetComponentRawAt(chunkIndex, indexInChunk, m_TypeId);
    if (!component)
        return {};
    std::vector<uint8_t> data(m_ComponentSize);
    std::memcpy(data.data(), component, m_ComponentSize);
    return data;
}

std::string ComponentHandler::GetComponentAsJson(Archetype* archetype, uint16_t chunkIndex,
                                                 uint16_t indexInChunk)
{
    const void* component = archetype->GetComponentRawAt(chunkIndex, indexInChunk, m_TypeId);
    if (!component)
        return "{}";
    if (m_ReflectedFields)
    {
        std::string json;
        AppendComponentJson(json, *m_ReflectedFields, static_cast<const std::byte*>(component));
        return json;
    }

    std::ostringstream json;
    json << "{\"type\":\"" << m_OpaqueTypeName << "\",\"size\":" << m_ComponentSize << ",\"data\":\"";
    const auto* bytes = static_cast<const uint8_t*>(component);
    for (size_t index = 0; index < m_ComponentSize; ++index)
        json << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(bytes[index]);
    json << "\"}";
    return json.str();
}

ComponentTypeId ComponentHandler::GetTypeId() const { return m_TypeId; }
const char* ComponentHandler::GetTypeName() const { return m_TypeName.c_str(); }
size_t ComponentHandler::GetComponentSize() const { return m_ComponentSize; }
} // namespace GameEngine::ECS
