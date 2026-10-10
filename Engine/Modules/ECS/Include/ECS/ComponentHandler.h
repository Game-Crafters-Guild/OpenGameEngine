#pragma once

#include "ECS/IComponentHandler.h"

#include <optional>

namespace GameEngine::ECS
{
struct FieldInfo;

/// Handler for a registered C++ component. Operations live in ECS; registration
/// supplies its layout and optional reflection table. The table belongs to the
/// registering module and stays valid until its registry entry is replaced or removed.
class ComponentHandler final : public IComponentHandler
{
  public:
    ComponentHandler(ComponentTypeId typeId, std::string typeName, std::size_t componentSize,
                     std::string opaqueTypeName, std::optional<std::span<const FieldInfo>> fields);
    ~ComponentHandler() override;

    void CopyComponent(Archetype* from, uint16_t fromChunk, uint16_t fromIndex,
                       Archetype* to, uint16_t toChunk, uint16_t toIndex) override;
    void AddComponentFromData(World* world, EntityHandle entity, std::span<const uint8_t> data) override;
    void RemoveComponent(World* world, EntityHandle entity) override;
    std::vector<uint8_t> SerializeComponent(Archetype* archetype, uint16_t chunkIndex,
                                          uint16_t indexInChunk) override;
    std::string GetComponentAsJson(Archetype* archetype, uint16_t chunkIndex,
                                   uint16_t indexInChunk) override;
    ComponentTypeId GetTypeId() const override;
    const char* GetTypeName() const override;
    size_t GetComponentSize() const override;

  private:
    ComponentTypeId m_TypeId;
    std::string m_TypeName;
    std::size_t m_ComponentSize;
    std::string m_OpaqueTypeName;
    std::optional<std::span<const FieldInfo>> m_ReflectedFields;
};
} // namespace GameEngine::ECS
