#pragma once

#include "ECS/ComponentRegistry.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::ECS
{

// Runtime-defined fixed-size blob component handler.
// All data is stored in the colocated ArchetypeTable columns (type-erased).
class BlobComponentHandler final : public IComponentHandler
{
  public:
    BlobComponentHandler(ComponentTypeId typeId, std::string name, std::size_t sizeBytes);

    void CopyComponent(Archetype* from, uint16_t fromChunk, uint16_t fromIdx,
                      Archetype* to, uint16_t toChunk, uint16_t toIdx) override;
    void AddComponentFromData(World* world, EntityHandle entity, std::span<const uint8_t> data) override;
    void RemoveComponent(World* world, EntityHandle entity) override;
    std::vector<uint8_t> SerializeComponent(Archetype* archetype, uint16_t chunkIdx, uint16_t idxInChunk) override;
    std::string GetComponentAsJson(Archetype* archetype, uint16_t chunkIdx, uint16_t idxInChunk) override;
    ComponentTypeId GetTypeId() const override;
    const char* GetTypeName() const override;
    size_t GetComponentSize() const override;

    // Update the stored byte size after a hot-reload changed the component's layout.
    // The caller (component migration) must also resize the archetype columns that
    // already hold this component so the recorded size and the storage stride agree.
    void SetSize(std::size_t sizeBytes) { m_SizeBytes = sizeBytes; }

  private:
    ComponentTypeId m_TypeId = 0;
    std::string m_Name;
    std::size_t m_SizeBytes = 0;
};
} // namespace GameEngine::ECS
