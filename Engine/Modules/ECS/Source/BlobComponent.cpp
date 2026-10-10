#include "ECS/BlobComponent.h"

#include "ECS/Entity.h"
#include "ECS/World.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace GameEngine::ECS
{

BlobComponentHandler::BlobComponentHandler(ComponentTypeId typeId, std::string name, std::size_t sizeBytes)
    : m_TypeId(typeId), m_Name(std::move(name)), m_SizeBytes(sizeBytes)
{
}

void BlobComponentHandler::CopyComponent(Archetype* from, uint16_t fromChunk, uint16_t fromIdx,
                                         Archetype* to, uint16_t toChunk, uint16_t toIdx)
{
    if (!from || !to || m_SizeBytes == 0)
        return;
    const void* src = from->GetComponentRawAt(fromChunk, fromIdx, m_TypeId);
    if (!src)
        return;
    to->SetComponentRawAt(toChunk, toIdx, m_TypeId, src, m_SizeBytes);
}

void BlobComponentHandler::AddComponentFromData(World* world, EntityHandle entity, std::span<const uint8_t> data)
{
    if (!world || data.size() != m_SizeBytes)
        return;
    (void)world->SetComponentBytesImmediate(entity, m_TypeId, data.data(), data.size());
}

void BlobComponentHandler::RemoveComponent(World* world, EntityHandle entity)
{
    if (!world)
        return;
    (void)world->RemoveComponentByTypeIdImmediate(entity, m_TypeId);
}

std::vector<uint8_t> BlobComponentHandler::SerializeComponent(Archetype* archetype, uint16_t chunkIdx, uint16_t idxInChunk)
{
    if (!archetype || m_SizeBytes == 0)
        return {};
    const void* src = archetype->GetComponentRawAt(chunkIdx, idxInChunk, m_TypeId);
    if (!src)
        return {};
    std::vector<uint8_t> out(m_SizeBytes);
    std::memcpy(out.data(), src, m_SizeBytes);
    return out;
}

std::string BlobComponentHandler::GetComponentAsJson(Archetype* archetype, uint16_t chunkIdx, uint16_t idxInChunk)
{
    auto bytes = SerializeComponent(archetype, chunkIdx, idxInChunk);
    std::ostringstream json;
    json << "{";
    json << "\"type\":\"" << m_Name << "\",";
    json << "\"size\":" << m_SizeBytes << ",";
    json << "\"data\":\"";
    for (std::size_t i = 0; i < bytes.size(); ++i)
        json << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(bytes[i]);
    json << "\"}";
    return json.str();
}

ComponentTypeId BlobComponentHandler::GetTypeId() const { return m_TypeId; }
const char* BlobComponentHandler::GetTypeName() const { return m_Name.c_str(); }
size_t BlobComponentHandler::GetComponentSize() const { return m_SizeBytes; }

} // namespace GameEngine::ECS
