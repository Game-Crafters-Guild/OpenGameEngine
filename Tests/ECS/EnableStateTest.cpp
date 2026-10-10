// The enable model at the query engine: what a disabled row is invisible to
// and what still sees it.
//
// The interesting cases are the ones a reader would get wrong: an Optional
// parameter is not a required one and reads as null rather than excluding the
// row; a per-type opt-in is not a blanket one and still skips inactive
// entities; Get/Has are direct access and never filter; and naming a tag in the
// type list means the query wants the rows that carry it.
//
// The derived tag's producer (DisabledInHierarchySystem) is an engine system
// and is covered where the engine's systems are; these tests write
// DisabledInHierarchy directly, which is what that system does.

#include <gtest/gtest.h>

#include "Components/Transform.h"
#include "ECS/AutoRegistration.h"
#include "ECS/CachedQuery.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "DeathTestChild.h"
#include "TestComponents.h"

#include <algorithm>
#include <cstddef>
#include <vector>

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{
// A parent link whose handle is not its first member, so the world has to
// read the handle at the offset the relation names.
struct TestParentLink
{
    uint32 Depth = 0;
    EntityHandle Parent{};
};

// Never registered: SetParentRelation refuses it.
struct UnregisteredParentLink
{
    EntityHandle Parent{};
};

void SetTestParentRelation(World& world)
{
    AutoComponentRegistrar<TestParentLink>::EnsureRegistered();
    world.SetParentRelation(GetComponentTypeId<TestParentLink>(),
                            static_cast<uint32>(offsetof(TestParentLink, Parent)));
}

EntityHandle MakeMover(World& world, float32 x)
{
    Entity entity = world.Create();
    entity.Set(Position{x, 0.0f, 0.0f});
    entity.Set(Velocity{1.0f, 0.0f, 0.0f});
    world.ProcessCommands(); // Set is deferred; the enable API is immediate
    return entity.GetHandle();
}

std::size_t CountPositions(World& world)
{
    return world.Query<Read<Position>>().Count();
}
} // namespace

TEST(EnableStateTest, DisablingAnEntityTakesItOutOfEveryQuery)
{
    World world(nullptr);
    const EntityHandle kept = MakeMover(world, 1.0f);
    const EntityHandle turnedOff = MakeMover(world, 2.0f);

    ASSERT_EQ(CountPositions(world), 2u);

    Entity(&world, turnedOff).SetEnabled(false);
    EXPECT_EQ(CountPositions(world), 1u);

    std::vector<float32> visited;
    world.Query<Read<Position>>().Each(
        [&](EntityHandle, const Position& position) { visited.push_back(position.x); });
    ASSERT_EQ(visited.size(), 1u);
    EXPECT_FLOAT_EQ(visited[0], 1.0f);

    Entity(&world, turnedOff).SetEnabled(true);
    EXPECT_EQ(CountPositions(world), 2u);
    EXPECT_TRUE(Entity(&world, kept).IsEnabled());
}

TEST(EnableStateTest, DirectAccessStillSeesADisabledEntity)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 3.0f);
    Entity(&world, entity).SetEnabled(false);

    Entity handle(&world, entity);
    EXPECT_FALSE(handle.IsEnabled());
    EXPECT_FALSE(handle.IsEnabledInHierarchy());
    ASSERT_TRUE(handle.Has<Position>());
    ASSERT_NE(handle.Get<Position>(), nullptr);
    EXPECT_FLOAT_EQ(handle.Get<Position>()->x, 3.0f);
}

TEST(EnableStateTest, DisablingAnEntityIsOneArchetypeMoveAndCarriesBothTags)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 4.0f);

    const std::size_t before = world.GetStructuralChangeVersion();
    Entity(&world, entity).SetEnabled(false);
    EXPECT_EQ(world.GetStructuralChangeVersion(), before + 1)
        << "SetEnabled(false) writes both activity tags, and it has to do it in one move";

    Entity handle(&world, entity);
    EXPECT_TRUE(handle.Has<Disabled>());
    EXPECT_TRUE(handle.Has<DisabledInHierarchy>());

    const std::size_t afterDisable = world.GetStructuralChangeVersion();
    Entity(&world, entity).SetEnabled(false);
    EXPECT_EQ(world.GetStructuralChangeVersion(), afterDisable)
        << "a redundant SetEnabled must not invalidate every query cache in the world";

    Entity(&world, entity).SetEnabled(true);
    EXPECT_EQ(world.GetStructuralChangeVersion(), afterDisable + 1);
    EXPECT_FALSE(handle.Has<Disabled>());
    EXPECT_FALSE(handle.Has<DisabledInHierarchy>());
}

// With a parent relation set, switching an entity back on while its parent is
// still off keeps the derived tag, so the entity stays out of every query until
// the hierarchy pass instead of reappearing in between. Under a parent that is
// on, both tags go, as they do with no relation.
TEST(EnableStateTest, AChildReEnabledAloneUnderADisabledParentStaysInactive)
{
    World world(nullptr);
    SetTestParentRelation(world);

    const EntityHandle offParent = MakeMover(world, 1.0f);
    const EntityHandle onParent = MakeMover(world, 2.0f);
    const EntityHandle underOff = MakeMover(world, 3.0f);
    const EntityHandle underOn = MakeMover(world, 4.0f);
    world.AddComponentImmediate<TestParentLink>(underOff, TestParentLink{1, offParent});
    world.AddComponentImmediate<TestParentLink>(underOn, TestParentLink{1, onParent});

    Entity(&world, underOff).SetEnabled(false);
    Entity(&world, underOn).SetEnabled(false);
    Entity(&world, offParent).SetEnabled(false);

    Entity(&world, underOff).SetEnabled(true);
    Entity(&world, underOn).SetEnabled(true);

    EXPECT_TRUE(Entity(&world, underOff).IsEnabled());
    EXPECT_FALSE(Entity(&world, underOff).IsEnabledInHierarchy()) << "its parent is still off";
    EXPECT_TRUE(Entity(&world, underOn).IsEnabledInHierarchy());
    EXPECT_EQ(CountPositions(world), 2u) << "only the parent that is on and its child are visible";
}

// The shape the editor writes by hand: Disabled alone on the parent, no derived
// tag yet. The lookup reads either tag.
TEST(EnableStateTest, AParentCarryingDisabledAloneKeepsTheChildOut)
{
    World world(nullptr);
    SetTestParentRelation(world);
    const EntityHandle parent = MakeMover(world, 1.0f);
    const EntityHandle child = MakeMover(world, 2.0f);
    world.AddComponentImmediate<TestParentLink>(child, TestParentLink{1, parent});
    Entity(&world, child).SetEnabled(false);
    world.AddComponentImmediate<Disabled>(parent, Disabled{});
    ASSERT_FALSE(world.HasComponent<DisabledInHierarchy>(parent));

    Entity(&world, child).SetEnabled(true);
    EXPECT_FALSE(Entity(&world, child).IsEnabledInHierarchy());
}

#if !defined(NDEBUG)
// A relation the world cannot read safely is refused: an offset whose handle
// runs past the component's end, or a type the registry does not know (its size
// is unknown, so no offset can be checked).
TEST(EnableStateTest, AParentRelationOffsetOutsideTheComponentAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    AutoComponentRegistrar<TestParentLink>::EnsureRegistered();
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            World w(nullptr);
            w.SetParentRelation(GetComponentTypeId<TestParentLink>(), static_cast<uint32>(sizeof(TestParentLink)));
        },
        "handle offset lies outside");
}

TEST(EnableStateTest, AParentRelationOnAnUnregisteredTypeAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ASSERT_EQ(ComponentRegistry::GetComponentInfo(GetComponentTypeId<UnregisteredParentLink>()), nullptr);
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            World w(nullptr);
            w.SetParentRelation(GetComponentTypeId<UnregisteredParentLink>(),
                                static_cast<uint32>(offsetof(UnregisteredParentLink, Parent)));
        },
        "parent component type is not registered");
}
#endif

TEST(EnableStateTest, AnEntityInactiveThroughItsAncestorIsExcludedToo)
{
    World world(nullptr);
    const EntityHandle child = MakeMover(world, 5.0f);

    // What the hierarchy pass writes on a descendant of a disabled parent: the
    // derived tag only, the child's own state untouched.
    world.AddComponentImmediate<DisabledInHierarchy>(child, DisabledInHierarchy{});

    EXPECT_EQ(CountPositions(world), 0u);
    EXPECT_TRUE(Entity(&world, child).IsEnabled());
    EXPECT_FALSE(Entity(&world, child).IsEnabledInHierarchy());

    EXPECT_EQ((world.Query<Read<Position>>().IncludeDisabled().Count()), 1u);
}

TEST(EnableStateTest, ADisabledComponentExcludesOnlyQueriesThatRequireIt)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 6.0f);

    Entity(&world, entity).SetEnabled<Velocity>(false);
    EXPECT_FALSE(Entity(&world, entity).IsEnabled<Velocity>());
    EXPECT_TRUE(Entity(&world, entity).IsEnabled<Position>());
    EXPECT_TRUE(Entity(&world, entity).IsEnabled()) << "the entity itself is untouched";

    EXPECT_EQ((world.Query<Read<Position>, Read<Velocity>>().Count()), 0u);
    EXPECT_EQ((world.Query<Read<Position>>().Count()), 1u);

    Entity(&world, entity).SetEnabled<Velocity>(true);
    EXPECT_EQ((world.Query<Read<Position>, Read<Velocity>>().Count()), 1u);
}

TEST(EnableStateTest, AnOptionalReadsADisabledComponentAsAbsent)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 7.0f);
    Entity(&world, entity).SetEnabled<Velocity>(false);

    std::size_t visited = 0;
    std::size_t sawVelocity = 0;
    world.Query<Read<Position>, Optional<Velocity>>().Each(
        [&](EntityHandle, const Position&, const Velocity* velocity)
        {
            ++visited;
            if (velocity)
                ++sawVelocity;
        });
    EXPECT_EQ(visited, 1u) << "an Optional never excludes the row";
    EXPECT_EQ(sawVelocity, 0u) << "a switched-off component reads as absent";
}

TEST(EnableStateTest, APerTypeOptInSeesTheComponentAndStillSkipsInactiveEntities)
{
    World world(nullptr);
    const EntityHandle componentOff = MakeMover(world, 8.0f);
    const EntityHandle entityOff = MakeMover(world, 9.0f);
    Entity(&world, componentOff).SetEnabled<Velocity>(false);
    Entity(&world, entityOff).SetEnabled(false);

    std::vector<float32> visited;
    std::size_t sawVelocity = 0;
    auto query = world.Query<Read<Position>, Optional<Velocity>>();
    query.IncludeDisabled<Velocity>();
    query.Each(
        [&](EntityHandle, const Position& position, const Velocity* velocity)
        {
            visited.push_back(position.x);
            if (velocity)
                ++sawVelocity;
        });

    ASSERT_EQ(visited.size(), 1u) << "the inactive entity stays excluded";
    EXPECT_FLOAT_EQ(visited[0], 8.0f);
    EXPECT_EQ(sawVelocity, 1u) << "the opted-in type reads through again";

    auto required = world.Query<Read<Position>, Read<Velocity>>();
    required.IncludeDisabled<Velocity>();
    EXPECT_EQ(required.Count(), 1u);
}

TEST(EnableStateTest, TheBlanketOptInSeesEverything)
{
    World world(nullptr);
    MakeMover(world, 10.0f);
    const EntityHandle entityOff = MakeMover(world, 11.0f);
    const EntityHandle componentOff = MakeMover(world, 12.0f);
    Entity(&world, entityOff).SetEnabled(false);
    Entity(&world, componentOff).SetEnabled<Velocity>(false);

    // Only the inactive entity is excluded here: the third row's Velocity is
    // off, and this query does not require Velocity.
    EXPECT_EQ((world.Query<Read<Position>>().Count()), 2u);
    EXPECT_EQ((world.Query<Read<Position>>().IncludeDisabled().Count()), 3u);
    EXPECT_EQ((world.Query<Read<Position>, Read<Velocity>>().IncludeDisabled().Count()), 3u);

    std::size_t sawVelocity = 0;
    auto query = world.Query<Read<Position>, Optional<Velocity>>();
    query.IncludeDisabled();
    query.Each([&](EntityHandle, const Position&, const Velocity* velocity)
               { sawVelocity += velocity != nullptr; });
    EXPECT_EQ(sawVelocity, 3u) << "the blanket opt-in turns off the Optional null rule too";
}

TEST(EnableStateTest, NamingATagInTheTypeListAsksForTheRowsThatCarryIt)
{
    World world(nullptr);
    MakeMover(world, 13.0f);
    const EntityHandle entityOff = MakeMover(world, 14.0f);
    Entity(&world, entityOff).SetEnabled(false);

    // Without this rule the query would exclude exactly the archetypes it asks
    // for and always match nothing — which is how the hierarchy pass finds the
    // entities whose derived tag it owns.
    EXPECT_EQ((world.Query<Read<Disabled>>().Count()), 1u);
    EXPECT_EQ((world.Query<Read<DisabledInHierarchy>>().Count()), 1u);
}

// ---- Enable-state transitions (GetDisabled<T>) ------------------------------

namespace
{
std::vector<EntityHandle> DisabledPositions(const World& world)
{
    const auto span = world.GetDisabled<Position>();
    return {span.begin(), span.end()};
}
} // namespace

TEST(EnableStateTest, GetDisabledReportsASwitchedOffComponentAndAnInactiveEntityOnce)
{
    World world(nullptr);
    world.EnableLifecycleEvents<Position>();
    const EntityHandle componentOff = MakeMover(world, 1.0f);
    const EntityHandle entityOff = MakeMover(world, 2.0f);
    const EntityHandle untouched = MakeMover(world, 3.0f);
    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetDisabled<Position>().empty());

    Entity(&world, componentOff).SetEnabled<Position>(false);
    // One move writes both entity tags: the entity must still be reported once.
    Entity(&world, entityOff).SetEnabled(false);
    EXPECT_TRUE(world.GetDisabled<Position>().empty()) << "the window is the one after the swap";

    world.SwapLifecycleEvents();
    std::vector<EntityHandle> expected{componentOff, entityOff};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(DisabledPositions(world), expected);
    EXPECT_TRUE(Entity(&world, untouched).IsEnabled<Position>());

    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetDisabled<Position>().empty()) << "each transition is delivered for one window";
}

TEST(EnableStateTest, GetDisabledReportsAnEntityInactiveThroughItsAncestor)
{
    World world(nullptr);
    world.EnableLifecycleEvents<Position>();
    const EntityHandle child = MakeMover(world, 1.0f);
    world.SwapLifecycleEvents();

    // What the hierarchy pass writes on a descendant of a disabled parent.
    world.AddComponentImmediate<DisabledInHierarchy>(child, DisabledInHierarchy{});
    world.SwapLifecycleEvents();
    EXPECT_EQ(DisabledPositions(world), std::vector<EntityHandle>{child});
}

TEST(EnableStateTest, GetDisabledLeavesOutWhatIsBackOnOrGoneAtTheSwap)
{
    World world(nullptr);
    world.EnableLifecycleEvents<Position>();
    const EntityHandle toggledBack = MakeMover(world, 1.0f);
    const EntityHandle destroyed = MakeMover(world, 2.0f);
    const EntityHandle removed = MakeMover(world, 3.0f);
    world.SwapLifecycleEvents();

    Entity(&world, toggledBack).SetEnabled<Position>(false);
    Entity(&world, toggledBack).SetEnabled<Position>(true);
    Entity(&world, destroyed).SetEnabled(false);
    world.DestroyEntityImmediate(destroyed);
    Entity(&world, removed).SetEnabled<Position>(false);
    world.RemoveComponentImmediate<Position>(removed);

    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetDisabled<Position>().empty())
        << "a component back on, a destroyed entity and a removed component are not disabled";
    EXPECT_EQ(world.GetRemoved<Position>().size(), 2u) << "removal has its own window";
}

TEST(EnableStateTest, GetDisabledIsEmptyForATypeNobodySubscribed)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 1.0f);
    Entity(&world, entity).SetEnabled<Position>(false);
    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetDisabled<Position>().empty());
}

// ---- Enable state by type id ------------------------------------------------

static_assert(ComponentDisabledTypeId(ComponentTypeName<Position>()) ==
                  GetComponentTypeId<ComponentDisabled<Position>>(),
              "the by-name tag id must be the typed tag's id");

TEST(EnableStateTest, EnablingByTypeIdWritesTheTypedTag)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 1.0f);
    const ComponentTypeId velocityId = GetComponentTypeId<Velocity>();

    EXPECT_TRUE(world.IsComponentEnabled(entity, velocityId));
    EXPECT_TRUE(world.SetComponentEnabledImmediate(entity, velocityId, false));
    EXPECT_FALSE(world.SetComponentEnabledImmediate(entity, velocityId, false)) << "a redundant call changes nothing";
    EXPECT_FALSE(world.IsComponentEnabled(entity, velocityId));
    EXPECT_FALSE(Entity(&world, entity).IsEnabled<Velocity>());
    EXPECT_EQ((world.Query<Read<Position>, Read<Velocity>>().Count()), 0u);

    // The typed API reads and clears the same tag.
    Entity(&world, entity).SetEnabled<Velocity>(true);
    EXPECT_TRUE(world.IsComponentEnabled(entity, velocityId));
    EXPECT_EQ((world.Query<Read<Position>, Read<Velocity>>().Count()), 1u);
}

// Transform is structure, not a feature (ComponentFlags::NotToggleable): switching it by type id is
// refused and writes no tag, so a script or the debug server cannot take an entity's placement out
// of every query.
TEST(EnableStateTest, ANotToggleableComponentRefusesTheSwitch)
{
    using GameEngine::Components::Transform;
    World world(nullptr);
    const EntityHandle entity = world.CreateHandle(Transform{});
    const ComponentTypeId transformId = GetComponentTypeId<Transform>();

    EXPECT_FALSE(ComponentRegistry::SwitchesThroughDisabledTag(transformId));
    const std::size_t before = world.GetStructuralChangeVersion();
    EXPECT_FALSE(world.SetComponentEnabledImmediate(entity, transformId, false));
    EXPECT_TRUE(world.IsComponentEnabled(entity, transformId));
    EXPECT_EQ(world.GetStructuralChangeVersion(), before) << "a refused switch moves nothing";
    EXPECT_EQ(world.Query<Read<Transform>>().Count(), 1u);
}

TEST(EnableStateTest, EnablingByTypeIdRefusesAnUnknownTypeAndTheTags)
{
    World world(nullptr);
    const EntityHandle entity = MakeMover(world, 1.0f);
    constexpr ComponentTypeId kUnregistered = 0x1234567890abcdefull;
    EXPECT_FALSE(world.SetComponentEnabledImmediate(entity, kUnregistered, false));
    AutoComponentRegistrar<Disabled>::EnsureRegistered();
    EXPECT_FALSE(world.SetComponentEnabledImmediate(entity, GetComponentTypeId<Disabled>(), false))
        << "an entity's activity is SetEntityEnabledImmediate's";
    EXPECT_TRUE(Entity(&world, entity).IsEnabled());
}

// ---- The type-erased query the scripting ABI iterates (CachedQuery) ---------

namespace
{
std::size_t CachedQueryEntityCount(CachedQuery& query)
{
    query.Refresh();
    std::size_t count = 0;
    for (std::size_t i = 0; i < query.GetArchetypeCount(); ++i)
        count += query.GetArchetype(i)->GetEntityCount();
    return count;
}
} // namespace

TEST(EnableStateTest, TheCachedQueryExcludesDisabledRowsLikeTheNativeQuery)
{
    World world(nullptr);
    AutoComponentRegistrar<Position>::EnsureRegistered();
    AutoComponentRegistrar<Velocity>::EnsureRegistered();
    MakeMover(world, 1.0f);
    const EntityHandle entityOff = MakeMover(world, 2.0f);
    const EntityHandle velocityOff = MakeMover(world, 3.0f);
    Entity(&world, entityOff).SetEnabled(false);
    Entity(&world, velocityOff).SetEnabled<Velocity>(false);

    CachedQuery positions(&world, {GetComponentTypeId<Position>()}, {});
    EXPECT_EQ(CachedQueryEntityCount(positions), 2u) << "the inactive entity is excluded, the switched-off Velocity is not required";

    CachedQuery movers(&world, {GetComponentTypeId<Position>(), GetComponentTypeId<Velocity>()}, {});
    EXPECT_EQ(CachedQueryEntityCount(movers), 1u);

    CachedQuery inactive(&world, {GetComponentTypeId<Disabled>()}, {});
    EXPECT_EQ(CachedQueryEntityCount(inactive), 1u) << "requiring the tag asks for the rows that carry it";
}

TEST(EnableStateTest, TheCachedQueryReportsASwitchedOffOptionalAsAbsent)
{
    World world(nullptr);
    AutoComponentRegistrar<Position>::EnsureRegistered();
    AutoComponentRegistrar<Velocity>::EnsureRegistered();
    const EntityHandle entity = MakeMover(world, 1.0f);

    CachedQuery positions(&world, {GetComponentTypeId<Position>()}, {});
    positions.Refresh();
    ASSERT_EQ(positions.GetArchetypeCount(), 1u);
    EXPECT_TRUE(positions.ArchetypeHasComponent(0, GetComponentTypeId<Velocity>()));

    Entity(&world, entity).SetEnabled<Velocity>(false);
    positions.Refresh();
    ASSERT_EQ(positions.GetArchetypeCount(), 1u);
    EXPECT_FALSE(positions.ArchetypeHasComponent(0, GetComponentTypeId<Velocity>()));
    EXPECT_TRUE(positions.ArchetypeHasComponent(0, GetComponentTypeId<Position>()));
}
