// The scripting ABI's enable switch: GE_ECSABI_GetComponentDisabledTypeId resolves the tag
// that switches a component off, the managed EntityCommands.SetEnabled<T> queues that tag
// through GE_ECSABI_DeferCommand, and EntityCommands.SetEnabled(entity) goes through
// GE_ECSABI_SetEntityEnabled. These drive the same exports the managed binding calls, with
// no CLR, so the ids and the query exclusion are checked against the typed C++ API. A
// component declared NotToggleable has no tag: the managed IsEnabled<T> reads it as on and
// SetEnabled<T> refuses it.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "Components/Name.h"
#include "ECS/CachedQuery.h"
#include "ECS/Components.h"
#include "ECS/DisabledInHierarchySystem.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scripting/ECSABI.h"

namespace EnableStateAbiTestTypes
{
// A typed component with an on/off state, as a game module declares one.
struct Switchable
{
    float Value = 0.0f;
};
} // namespace EnableStateAbiTestTypes

namespace
{
using GameEngine::ECS::World;

constexpr uint32_t kDeferAddComponent = 2;
constexpr uint32_t kDeferRemoveComponent = 3;

GE_Handle HandleOf(World* world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
}

const GE_ECS_Interface_v2* Interface()
{
    const void* table = nullptr;
    uint32_t sizeBytes = 0;
    if (GE_ECS_GetInterface(GE_ECS_ABI_VERSION_CURRENT, &table, &sizeBytes) != GE_Result_Ok)
        return nullptr;
    if (sizeBytes < sizeof(GE_ECS_Interface_v2))
        return nullptr;
    return static_cast<const GE_ECS_Interface_v2*>(table);
}

std::size_t CountMatching(World& world, GameEngine::ECS::ComponentTypeId required)
{
    GameEngine::ECS::CachedQuery query(&world, {required}, {});
    query.Refresh();
    std::size_t count = 0;
    for (std::size_t i = 0; i < query.GetArchetypeCount(); ++i)
        count += query.GetArchetype(i)->GetEntityCount();
    return count;
}

// The managed side queues tags as EntityCommands.SetEnabled does: deferred, one byte of data.
void QueueTag(World& world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId tag, bool present)
{
    const GE_Handle handle = HandleOf(&world);
    ASSERT_EQ(GE_ECSABI_SetDeferStructuralChanges(handle, 1), GE_Result_Ok);
    const uint8_t tagByte = 0;
    ASSERT_EQ(GE_ECSABI_DeferCommand(handle, present ? kDeferAddComponent : kDeferRemoveComponent, entity, tag,
                                     present ? &tagByte : nullptr, present ? 1u : 0u),
              GE_Result_Ok);
    ASSERT_EQ(GE_ECSABI_FlushDeferredCommands(handle), GE_Result_Ok);
}

// The managed entity switch: EntityCommands.SetEnabled(entity, on) while systems run, so the
// switch is queued and applied at the flush.
void SwitchEntity(World& world, GE_ECS_Entity entity, bool on)
{
    const GE_Handle handle = HandleOf(&world);
    ASSERT_EQ(GE_ECSABI_SetDeferStructuralChanges(handle, 1), GE_Result_Ok);
    ASSERT_EQ(GE_ECSABI_SetEntityEnabled(handle, entity, on ? 1 : 0), GE_Result_Ok);
    ASSERT_EQ(GE_ECSABI_FlushDeferredCommands(handle), GE_Result_Ok);
}
} // namespace

TEST(EnableStateAbi, TheInterfaceResolvesTheSameTagAsTheTypedApi)
{
    const GE_ECS_Interface_v2* iface = Interface();
    ASSERT_NE(iface, nullptr);
    ASSERT_NE(iface->GetComponentDisabledTypeId, nullptr);
    ASSERT_NE(iface->SetEntityEnabled, nullptr);

    using EnableStateAbiTestTypes::Switchable;
    GameEngine::ECS::AutoComponentRegistrar<Switchable>::EnsureRegistered();
    GE_ECS_ComponentTypeId tag = 0;
    ASSERT_EQ(iface->GetComponentDisabledTypeId(GameEngine::ECS::GetComponentTypeId<Switchable>(), &tag),
              GE_Result_Ok);
    EXPECT_EQ(tag, GameEngine::ECS::GetComponentTypeId<GameEngine::ECS::ComponentDisabled<Switchable>>());

    const char* entityTag = "GameEngine::ECS::Disabled";
    GE_ECS_ComponentTypeId disabledId = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentTypeIdByName(entityTag, static_cast<uint32_t>(std::strlen(entityTag)),
                                                 &disabledId),
              GE_Result_Ok);
    EXPECT_EQ(disabledId, GameEngine::ECS::GetComponentTypeId<GameEngine::ECS::Disabled>())
        << "the managed entity switch resolves the tag by this name";
}

TEST(EnableStateAbi, ANotToggleableComponentHasNoTag)
{
    using GameEngine::Components::Name;
    GameEngine::ECS::AutoComponentRegistrar<Name>::EnsureRegistered();
    GE_ECS_ComponentTypeId tag = 1;
    ASSERT_EQ(GE_ECSABI_GetComponentDisabledTypeId(GameEngine::ECS::GetComponentTypeId<Name>(), &tag), GE_Result_Ok);
    EXPECT_EQ(tag, 0u);
}

TEST(EnableStateAbi, RefusesAnUnknownComponentAndAnEnableStateTag)
{
    GE_ECS_ComponentTypeId tag = 1;
    EXPECT_EQ(GE_ECSABI_GetComponentDisabledTypeId(0, &tag), GE_Result_InvalidArg);
    EXPECT_EQ(tag, 0u);
    EXPECT_EQ(GE_ECSABI_GetComponentDisabledTypeId(0x5EEDF00Dull, &tag), GE_Result_NotFound);
    EXPECT_EQ(GE_ECSABI_GetComponentDisabledTypeId(
                  GameEngine::ECS::GetComponentTypeId<GameEngine::ECS::Disabled>(), &tag),
              GE_Result_NotFound);
}

TEST(EnableStateAbi, AQueuedTagSwitchesABlobComponentOutOfQueriesAndBack)
{
    const char* name = "EnableStateAbiProbe";
    GE_ECS_ComponentTypeId probeId = 0;
    ASSERT_EQ(GE_ECSABI_RegisterBlobComponent(name, static_cast<uint32_t>(std::strlen(name)), sizeof(float), &probeId),
              GE_Result_Ok);
    GE_ECS_ComponentTypeId tag = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentDisabledTypeId(probeId, &tag), GE_Result_Ok);
    ASSERT_NE(tag, 0u);

    World world(nullptr);
    const GE_Handle handle = HandleOf(&world);
    GE_ECS_Entity entity = 0;
    ASSERT_EQ(GE_ECSABI_CreateEntityRaw(handle, &entity), GE_Result_Ok);
    const float value = 1.5f;
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(handle, entity, probeId, reinterpret_cast<const uint8_t*>(&value),
                                          sizeof(value)),
              GE_Result_Ok);
    const GameEngine::ECS::EntityHandle entityHandle(static_cast<uint32_t>(entity));
    ASSERT_EQ(CountMatching(world, probeId), 1u);

    QueueTag(world, entity, tag, true);
    EXPECT_FALSE(world.IsComponentEnabled(entityHandle, probeId));
    EXPECT_EQ(CountMatching(world, probeId), 0u) << "a switched-off component leaves the query";
    float stored = 0.0f;
    uint32_t storedLen = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentBytes(handle, entity, probeId, reinterpret_cast<uint8_t*>(&stored),
                                          sizeof(stored), &storedLen),
              GE_Result_Ok);
    EXPECT_EQ(stored, value) << "switching off keeps the data";

    QueueTag(world, entity, tag, false);
    EXPECT_TRUE(world.IsComponentEnabled(entityHandle, probeId));
    EXPECT_EQ(CountMatching(world, probeId), 1u);
}

// The entity switch moves both activity tags when no parent is off: an entity switched off,
// left through one Early pass (which derives DisabledInHierarchy) and switched back on returns
// to queries at once, not a pass later. A removal of ECS::Disabled alone would leave the derived
// tag behind.
TEST(EnableStateAbi, AnEntitySwitchedBackOnAfterAnEarlyPassReturnsToQueries)
{
    const char* name = "EnableStateAbiEntityProbe";
    GE_ECS_ComponentTypeId probeId = 0;
    ASSERT_EQ(GE_ECSABI_RegisterBlobComponent(name, static_cast<uint32_t>(std::strlen(name)), sizeof(float), &probeId),
              GE_Result_Ok);

    World world(nullptr);
    const GE_Handle handle = HandleOf(&world);
    GE_ECS_Entity entity = 0;
    ASSERT_EQ(GE_ECSABI_CreateEntityRaw(handle, &entity), GE_Result_Ok);
    const float value = 1.0f;
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(handle, entity, probeId, reinterpret_cast<const uint8_t*>(&value),
                                          sizeof(value)),
              GE_Result_Ok);
    const GameEngine::ECS::EntityHandle entityHandle(static_cast<uint32_t>(entity));
    GameEngine::ECS::DisabledInHierarchySystem earlyPass;

    SwitchEntity(world, entity, false);
    EXPECT_EQ(CountMatching(world, probeId), 0u);
    earlyPass.Update(world, 0.016f);
    EXPECT_TRUE(world.HasComponent<GameEngine::ECS::DisabledInHierarchy>(entityHandle)) << "the pass derives the tag";

    SwitchEntity(world, entity, true);
    EXPECT_EQ(CountMatching(world, probeId), 1u) << "back in queries before the next pass";
    EXPECT_FALSE(world.HasComponent<GameEngine::ECS::DisabledInHierarchy>(entityHandle)) << "no stale derived tag";
    earlyPass.Update(world, 0.016f);
    EXPECT_EQ(CountMatching(world, probeId), 1u);
}
