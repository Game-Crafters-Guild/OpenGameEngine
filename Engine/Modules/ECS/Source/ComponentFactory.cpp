#include "ECS/ComponentFactory.h"

#include "ECS/ECSTemplates.h"  // complete World — RegisterDefaultBytes's creator calls World::SetComponentBytesImmediate

#include <cstdint>
#include <unordered_map>

namespace GameEngine {
namespace ECS {

namespace {
std::unordered_map<ComponentTypeId, ComponentFactory::CreateFn>& Creators()
{
    static std::unordered_map<ComponentTypeId, ComponentFactory::CreateFn> creators;
    return creators;
}

// Default-value byte snapshots, keyed by type id. Single source of truth: both the
// bytes-creator closure and GetDefaultBytes read from here, so a reload that re-registers
// with a new size is reflected everywhere.
std::unordered_map<ComponentTypeId, std::vector<std::uint8_t>>& DefaultBytesMap()
{
    static std::unordered_map<ComponentTypeId, std::vector<std::uint8_t>> bytes;
    return bytes;
}

// Kept apart from the creators: a derivation registers from its component's own translation
// unit, in whatever order static initialisation runs relative to the reflection registration
// that installs the creator.
std::unordered_map<ComponentTypeId, ComponentFactory::DeriveDefaultsFn>& Derivations()
{
    static std::unordered_map<ComponentTypeId, ComponentFactory::DeriveDefaultsFn> derivations;
    return derivations;
}
} // namespace

void ComponentFactory::Register(ComponentTypeId typeId, CreateFn create)
{
    if (create)
        Creators()[typeId] = std::move(create);
}

void ComponentFactory::RegisterDefaultBytes(ComponentTypeId typeId, const void* defaultBytes, std::size_t size,
                                            bool addable)
{
    if (!defaultBytes || size == 0)
        return;

    const auto* first = static_cast<const std::uint8_t*>(defaultBytes);
    DefaultBytesMap()[typeId].assign(first, first + size);

    // The Add Component menu only lists components with a creator. `addable` controls menu
    // visibility, NOT whether the default bytes are recorded — the bytes are always stored so
    // hot-reload migration can run on hidden (NO_ADD) components too. The creator is built here,
    // where World is complete, so the registering user TU never needs AddComponentImmediate<T>;
    // it captures the bytes by value (re-registered with fresh bytes on reload).
    // SetComponentBytesImmediate performs the same archetype move + OnAdd/OnSet hooks as a
    // typed add for trivially-copyable components.
    if (addable)
    {
        Creators()[typeId] = [typeId, bytes = DefaultBytesMap()[typeId]](World& world, EntityHandle entity) {
            world.SetComponentBytesImmediate(entity, typeId, bytes.data(), bytes.size());
        };
    }
}

bool ComponentFactory::GetDefaultBytes(ComponentTypeId typeId, std::vector<std::uint8_t>& out)
{
    const auto it = DefaultBytesMap().find(typeId);
    if (it == DefaultBytesMap().end())
        return false;
    out = it->second;
    return true;
}

void ComponentFactory::RegisterDerivedDefaults(ComponentTypeId typeId, DeriveDefaultsFn derive)
{
    if (derive)
        Derivations()[typeId] = std::move(derive);
}

std::vector<ComponentTypeId> ComponentFactory::DefaultByteTypes()
{
    std::vector<ComponentTypeId> types;
    types.reserve(DefaultBytesMap().size());
    for (const auto& [typeId, bytes] : DefaultBytesMap())
        types.push_back(typeId);
    return types;
}

bool ComponentFactory::Create(World& world, EntityHandle entity, ComponentTypeId typeId)
{
    if (const auto derivation = Derivations().find(typeId); derivation != Derivations().end())
    {
        std::vector<std::uint8_t> bytes;
        if (GetDefaultBytes(typeId, bytes))
        {
            derivation->second(world, entity, std::span<std::uint8_t>(bytes));
            return world.SetComponentBytesImmediate(entity, typeId, bytes.data(), bytes.size());
        }
    }

    const auto it = Creators().find(typeId);
    if (it != Creators().end() && it->second)
    {
        it->second(world, entity);
        return true;
    }

    const auto defaultIt = DefaultBytesMap().find(typeId);
    if (defaultIt == DefaultBytesMap().end())
        return false;

    const auto& bytes = defaultIt->second;
    return world.SetComponentBytesImmediate(entity, typeId, bytes.data(), bytes.size());
}

bool ComponentFactory::Has(ComponentTypeId typeId)
{
    return Creators().contains(typeId) || DefaultBytesMap().contains(typeId);
}

std::vector<ComponentTypeId> ComponentFactory::RegisteredTypes()
{
    std::vector<ComponentTypeId> types;
    types.reserve(Creators().size());
    for (const auto& [typeId, fn] : Creators())
        types.push_back(typeId);
    return types;
}

} // namespace ECS
} // namespace GameEngine
