#include "ECS/UnresolvedComponentStore.h"

#include "ECS/Entity.h"

namespace GameEngine::ECS
{

bool CapturePreservedFieldBytes(const World& world, EntityHandle entity, const PreservedField& field,
                                std::vector<std::uint8_t>& out)
{
    out.clear();
    if (field.TypeId == 0 || field.FieldSize == 0)
        return false;
    std::vector<std::uint8_t> bytes;
    if (!world.CaptureComponentBytes(entity, field.TypeId, bytes))
        return false;
    if (static_cast<std::size_t>(field.FieldOffset) + field.FieldSize > bytes.size())
        return false;
    out.assign(bytes.begin() + field.FieldOffset, bytes.begin() + field.FieldOffset + field.FieldSize);
    return true;
}

bool IsPreservedFieldSuperseded(const World& world, EntityHandle entity, const PreservedField& field)
{
    if (field.FieldSize == 0)
        return false;
    std::vector<std::uint8_t> live;
    return !CapturePreservedFieldBytes(world, entity, field, live) || live != field.FallbackBytes;
}

} // namespace GameEngine::ECS
