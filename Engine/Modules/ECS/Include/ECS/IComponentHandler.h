#pragma once

#include "ECS/ECS.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
class Archetype;

/**
 * @brief Abstract interface for component operations
 *
 * This interface provides type-erased operations for components,
 * allowing the ECS system to handle any component type without
 * hardcoded logic.
 */
class IComponentHandler {
public:
    virtual ~IComponentHandler() = default;

    // Copy a component between archetypes at given {chunkIndex, indexInChunk} locations.
    virtual void CopyComponent(Archetype* from, uint16_t fromChunk, uint16_t fromIdx,
                              Archetype* to, uint16_t toChunk, uint16_t toIdx) = 0;

    // Add a component to an entity from serialized data (triggers archetype move).
    virtual void AddComponentFromData(class World* world, EntityHandle entity, std::span<const uint8_t> data) = 0;

    // Remove a component from an entity.
    virtual void RemoveComponent(class World* world, EntityHandle entity) = 0;

    // Serialize a component at {chunkIndex, indexInChunk} to binary data.
    virtual std::vector<uint8_t> SerializeComponent(Archetype* archetype, uint16_t chunkIdx, uint16_t idxInChunk) = 0;

    // Get component data as JSON string for debugging.
    virtual std::string GetComponentAsJson(Archetype* archetype, uint16_t chunkIdx, uint16_t idxInChunk) = 0;

    // Get the component type ID.
    virtual ComponentTypeId GetTypeId() const = 0;

    // Get the human-readable component name.
    virtual const char* GetTypeName() const = 0;

    // Get the size of the component in bytes.
    virtual size_t GetComponentSize() const = 0;
};

} // namespace GameEngine::ECS
