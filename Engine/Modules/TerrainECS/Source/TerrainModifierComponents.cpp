// The terrain modifier component set: its cross-DLL instantiation, and the
// world subscription derived from the canonical lists in the header.
// See ECS/ECSTemplates.h for the instantiation rationale
// (GE_INSTANTIATE_ENGINE_COMPONENT).
#include "TerrainECS/TerrainModifierComponents.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <array>

namespace GameEngine::ECS
{
    // Keep in step with ModifierRootComponents + ModifierEffectComponents. An
    // explicit instantiation is a declaration, not an expression, so it cannot
    // be folded out of the type lists the way the subscription below is.
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainModifierVolume);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainFlattenEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainHeightOffsetEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainNoiseEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainStampEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainPaintLayerEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainSurfaceRulesEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainGroundClaimEffect);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainGrassEffect);

    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainSculptZone);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainPaintZone);
} // namespace GameEngine::ECS

namespace GameEngine::TerrainECS
{
namespace
{
template <typename... Ts>
void EnableLifecycleEventsFor(ECS::World& world, ComponentTypeList<Ts...>)
{
    (world.EnableLifecycleEvents<Ts>(), ...);
}

// Instantiating TerrainEffectChrome<T> for every T in the list is what makes an
// effect without chrome a compile error rather than a blank picker row.
template <typename... Ts>
const std::array<TerrainEffectTypeInfo, sizeof...(Ts)>& EffectTypeInfos(ComponentTypeList<Ts...>)
{
    static const std::array<TerrainEffectTypeInfo, sizeof...(Ts)> kInfos = {
        TerrainEffectTypeInfo{ECS::GetComponentTypeId<Ts>(),
                              TerrainEffectChrome<Ts>::Title,
                              TerrainEffectChrome<Ts>::Description}...};
    return kInfos;
}

// Adds whichever effect type matches `typeId`, default-constructed at
// `stackOrder`. The fold short-circuits on the first type that adds; a matched
// type already present adds nothing and no later type can match, so the whole
// fold is false — "not added", which is what every caller checks.
template <typename... Ts>
bool AddDefaultEffect(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                      int32 stackOrder, ComponentTypeList<Ts...>)
{
    const auto addOne = [&]<typename T>()
    {
        if (typeId != ECS::GetComponentTypeId<T>() || world.GetComponent<T>(entity))
            return false;
        T effect{};
        effect.StackOrder = stackOrder;
        world.AddComponentImmediate<T>(entity, effect);
        return true;
    };
    return (addOne.template operator()<Ts>() || ...);
}

// Stamps the stack order onto whichever effect type matches `typeId`. The fold
// short-circuits on the first type that matches, and a matched type the entity
// does not carry writes nothing — so the whole fold is false, "not stamped".
// The write goes through GetComponentForWrite: renumbering a stack is an edit
// the modifier change gate has to see.
template <typename... Ts>
bool SetEffectStackOrder(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                         int32 stackOrder, ComponentTypeList<Ts...>)
{
    const auto setOne = [&]<typename T>()
    {
        if (typeId != ECS::GetComponentTypeId<T>())
            return false;
        T* effect = world.GetComponentForWrite<T>(entity);
        if (!effect)
            return false;
        effect->StackOrder = stackOrder;
        return true;
    };
    return (setOne.template operator()<Ts>() || ...);
}

template <typename... Ts>
int32 HighestStackOrderPlusOne(const ECS::World& world, ECS::EntityHandle entity,
                               ComponentTypeList<Ts...>)
{
    int32 next = 0;
    const auto consider = [&next](const auto* effect)
    {
        if (effect)
            next = std::max(next, effect->StackOrder + 1);
    };
    (consider(world.GetComponent<Ts>(entity)), ...);
    return next;
}
} // namespace

void EnableTerrainModifierLifecycleEvents(ECS::World& world)
{
    EnableLifecycleEventsFor(world, ModifierRootComponents{});
    EnableLifecycleEventsFor(world, ModifierEffectComponents{});
}

std::span<const TerrainEffectTypeInfo> TerrainEffectTypes()
{
    return EffectTypeInfos(ModifierEffectComponents{});
}

const TerrainEffectTypeInfo* FindTerrainEffectType(ECS::ComponentTypeId typeId)
{
    for (const TerrainEffectTypeInfo& info : TerrainEffectTypes())
        if (info.TypeId == typeId)
            return &info;
    return nullptr;
}

bool IsTerrainEffectComponent(ECS::ComponentTypeId typeId)
{
    return FindTerrainEffectType(typeId) != nullptr;
}

int32 NextEffectStackOrder(const ECS::World& world, ECS::EntityHandle entity)
{
    return HighestStackOrderPlusOne(world, entity, ModifierEffectComponents{});
}

bool AddTerrainEffectDefault(ECS::World& world, ECS::EntityHandle entity,
                             ECS::ComponentTypeId typeId, int32 stackOrder)
{
    return AddDefaultEffect(world, entity, typeId, stackOrder, ModifierEffectComponents{});
}

bool SetTerrainEffectStackOrder(ECS::World& world, ECS::EntityHandle entity,
                                ECS::ComponentTypeId typeId, int32 stackOrder)
{
    return SetEffectStackOrder(world, entity, typeId, stackOrder, ModifierEffectComponents{});
}

} // namespace GameEngine::TerrainECS
