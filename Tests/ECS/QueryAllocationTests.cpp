// The allocation contract of ECS query construction, measured rather than argued.
//
// Most query sites build the query as a temporary — world.Query<Read<A>, Write<B>>().Each(...)
// — and the per-frame systems among them build a fresh one every frame. Everything a query
// derives from its type list is a compile-time constant, so the only heap traffic such a
// construction may pay is the single buffer that snapshots the matching archetypes, and that
// snapshot is what lets iteration run without holding the world's lock.
//
// No behavioural test can see this. A query that rebuilds its read, write and required
// signatures on every construction returns exactly the same entities as one that reads them
// from a per-instantiation constant; only an allocation count separates them.
//
// Its own target on purpose: the measurement is a process-wide allocation window, and
// ECSCoreTests is one binary of 28 suites. EntityDestroyNotificationTests is split out too:
// it arms allocation faults (Tests/ECS/CMakeLists.txt).

#include <gtest/gtest.h>

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "ArchetypeSpread.h"
#include "TestComponents.h"
#include "Memory/AllocationCountScope.h"

#include <cstddef>
#include <cstdio>

namespace
{
using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

// Enough archetypes that a per-construction match list has to grow if it is not reserved,
// and close to the archetype counts a loaded editor scene reaches.
constexpr unsigned kArchetypeCount = 200;
// The measured window runs many constructions so the per-construction figure is not a
// single sample.
constexpr std::size_t kMeasuredConstructions = 50;
// The archetype snapshot, and nothing else. Signatures are per-instantiation constants,
// so a construction that exceeds this has started building one of them again.
constexpr std::size_t kAllocationsPerConstruction = 1;

std::size_t RunFreshQuery(World& world)
{
    std::size_t visited = 0;
    world.Query<Read<Position>, Write<Velocity>>().Each(
        [&visited](EntityHandle, const Position& position, Velocity& velocity)
        {
            velocity.x += position.x * 0.0f;
            ++visited;
        });
    return visited;
}
} // namespace

// The headline claim: a query built fresh, as the per-frame sites build theirs, allocates
// only its archetype snapshot.
TEST(QueryAllocationTests, FreshConstructionAllocatesOnlyTheArchetypeSnapshot)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    // The world has to have the shape the measurement claims, or the count means nothing.
    ASSERT_EQ(RunFreshQuery(world), static_cast<std::size_t>(kArchetypeCount));
    ASSERT_GE(world.GetArchetypeCount(), static_cast<std::size_t>(kArchetypeCount));

    // Settle: component registration, the per-instantiation signatures and the chunk
    // version arrays are all first-use costs, and none of them is what this pins.
    for (int warmup = 0; warmup < 3; ++warmup)
        RunFreshQuery(world);

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            RunFreshQuery(world);
        allocations = probe.Count();
    }

    std::printf("[Alloc] fresh query construction + Each over %u archetypes: %zu allocations"
                " across %zu runs (%.2f per construction)\n",
                kArchetypeCount, allocations, kMeasuredConstructions,
                static_cast<double>(allocations) / kMeasuredConstructions);

    EXPECT_LE(allocations, kAllocationsPerConstruction * kMeasuredConstructions)
        << "fresh query construction + Each allocated " << allocations << " times across "
        << kMeasuredConstructions << " runs ("
        << (static_cast<double>(allocations) / kMeasuredConstructions)
        << "/construction). Building the read, write or required signature per construction,"
           " or copying the archetype list into a temporary, will do this.";
}

// A retained query — the shape GameUIHost's DocQueryCache uses — allocates nothing while
// the world is structurally unchanged: its snapshot buffer keeps its capacity, so the only
// way this count moves is an allocation on the iteration path itself.
TEST(QueryAllocationTests, RetainedQueryAllocatesNothingWithoutStructuralChange)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Query<Read<Position>, Write<Velocity>> query(&world);
    std::size_t visited = 0;
    const auto visit = [&visited](EntityHandle, const Position&, Velocity&) { ++visited; };

    query.Each(visit);
    ASSERT_EQ(visited, static_cast<std::size_t>(kArchetypeCount));

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            query.Each(visit);
        allocations = probe.Count();
    }

    std::printf("[Alloc] retained query, %zu iterations of an unchanged world: %zu allocations\n",
                kMeasuredConstructions, allocations);

    EXPECT_EQ(allocations, 0u)
        << "a retained query allocated " << allocations << " times over "
        << kMeasuredConstructions
        << " iterations of an unchanged world. Each is allocating on the iteration path, or"
           " the cache rebuild has stopped reusing the snapshot buffer.";
}

// Turning an entity off and on again is an archetype move each way. Both edges are cached
// on the archetype after the first transition through them and the destination chunks already
// exist, so a steady-state toggle has nothing left to allocate — which is the claim that makes
// the enable model cheap enough to be the default everywhere.
TEST(QueryAllocationTests, TogglingEnableStateAllocatesNothingInSteadyState)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    const EntityHandle handle = entity.GetHandle();

    // Warm both edges and the destination chunks; the first transition through an edge
    // creates the archetype it names.
    for (int warmup = 0; warmup < 3; ++warmup)
    {
        Entity(&world, handle).SetEnabled(false);
        Entity(&world, handle).SetEnabled(true);
        Entity(&world, handle).SetEnabled<Velocity>(false);
        Entity(&world, handle).SetEnabled<Velocity>(true);
    }
    ASSERT_TRUE(Entity(&world, handle).IsEnabled());

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
        {
            Entity(&world, handle).SetEnabled(false);
            Entity(&world, handle).SetEnabled(true);
            Entity(&world, handle).SetEnabled<Velocity>(false);
            Entity(&world, handle).SetEnabled<Velocity>(true);
        }
        allocations = probe.Count();
    }

    // The toggles have to have done something, or a no-op would read as zero allocations.
    ASSERT_FALSE(Entity(&world, handle).Has<Disabled>());
    ASSERT_TRUE(Entity(&world, handle).IsEnabled<Velocity>());

    std::printf("[Alloc] %zu enable-state toggles (entity + component, both directions):"
                " %zu allocations\n",
                kMeasuredConstructions * 4, allocations);

    EXPECT_EQ(allocations, 0u)
        << "toggling enable state allocated " << allocations << " times across "
        << (kMeasuredConstructions * 4)
        << " transitions. An uncached archetype edge, a signature rebuilt per call, or a"
           " lifecycle buffer growing every frame will do this.";
}

// One entity switched off and on both ways across three swap windows; returns how many
// entities the disabled windows reported.
static std::size_t ToggleVelocityAcrossThreeWindows(World& world, EntityHandle handle)
{
    std::size_t reported = 0;
    Entity(&world, handle).SetEnabled(false);
    world.SwapLifecycleEvents();
    reported += world.GetDisabled<Velocity>().size();
    Entity(&world, handle).SetEnabled(true);
    Entity(&world, handle).SetEnabled<Velocity>(false);
    world.SwapLifecycleEvents();
    reported += world.GetDisabled<Velocity>().size();
    Entity(&world, handle).SetEnabled<Velocity>(true);
    world.SwapLifecycleEvents();
    return reported;
}

// A world whose Velocity consumers read GetDisabled<Velocity>() records the tag transitions in
// the lifecycle windows and builds the disabled window inside SwapLifecycleEvents. Both reuse
// their vectors frame to frame, so a frame of toggles and its swap allocate nothing once the
// buffers have grown to the frame's size.
TEST(QueryAllocationTests, SwappingDisabledEventsAllocatesNothingInSteadyState)
{
    World world(nullptr);
    world.EnableLifecycleEvents<Velocity>();
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    world.ProcessCommands();
    const EntityHandle handle = entity.GetHandle();

    for (int warmup = 0; warmup < 3; ++warmup)
        ToggleVelocityAcrossThreeWindows(world, handle);

    std::size_t allocations = 0;
    std::size_t reported = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            reported += ToggleVelocityAcrossThreeWindows(world, handle);
        allocations = probe.Count();
    }

    // Both windows have to have reported the entity, or an empty build would read as zero.
    ASSERT_EQ(reported, kMeasuredConstructions * 2);

    std::printf("[Alloc] %zu frames of enable toggles with GetDisabled subscribed: %zu allocations\n",
                kMeasuredConstructions, allocations);
    EXPECT_EQ(allocations, 0u) << "building the disabled window allocated " << allocations << " times";
}

// The shape above is a root entity: its archetype carries neither activity tag, so both
// transitions are the cached activity pair. The engine produces two other shapes — a
// descendant of a disabled parent, which carries DisabledInHierarchy alone (written by the
// hierarchy pass), and an authored Disabled tag no pass has seen yet (scene load, the debug
// server) — and for those only Disabled moves. That single-tag transition resolves through
// the per-type add/remove edges, which cache both directions, so it is as free as the pair.
TEST(QueryAllocationTests, TogglingADescendantOfADisabledParentAllocatesNothingInSteadyState)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    const EntityHandle handle = entity.GetHandle();

    // What the hierarchy pass writes on a descendant of a disabled parent, re-applied after
    // every re-enable the way the pass would on its next update.
    world.AddComponentImmediate<DisabledInHierarchy>(handle, DisabledInHierarchy{});
    for (int warmup = 0; warmup < 3; ++warmup)
    {
        Entity(&world, handle).SetEnabled(false);
        Entity(&world, handle).SetEnabled(true);
        world.AddComponentImmediate<DisabledInHierarchy>(handle, DisabledInHierarchy{});
    }

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
        {
            Entity(&world, handle).SetEnabled(false);
            Entity(&world, handle).SetEnabled(true);
            world.AddComponentImmediate<DisabledInHierarchy>(handle, DisabledInHierarchy{});
        }
        allocations = probe.Count();
    }
    ASSERT_TRUE(Entity(&world, handle).IsEnabled());
    ASSERT_TRUE(Entity(&world, handle).Has<DisabledInHierarchy>());

    std::printf("[Alloc] %zu enable-state toggles on a descendant of a disabled parent:"
                " %zu allocations\n",
                kMeasuredConstructions * 2, allocations);

    EXPECT_EQ(allocations, 0u)
        << "toggling an entity that already carries DisabledInHierarchy allocated " << allocations
        << " times across " << (kMeasuredConstructions * 2)
        << " transitions: the single-tag transition is not going through the cached edges.";
}

TEST(QueryAllocationTests, ReEnablingAnAuthoredDisabledTagAllocatesNothingInSteadyState)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    const EntityHandle handle = entity.GetHandle();

    // What scene load writes: the authored tag alone, before any hierarchy pass ran.
    world.AddComponentImmediate<Disabled>(handle, Disabled{});
    for (int warmup = 0; warmup < 3; ++warmup)
    {
        Entity(&world, handle).SetEnabled(true);
        world.AddComponentImmediate<Disabled>(handle, Disabled{});
    }

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
        {
            Entity(&world, handle).SetEnabled(true);
            world.AddComponentImmediate<Disabled>(handle, Disabled{});
        }
        allocations = probe.Count();
    }
    ASSERT_FALSE(Entity(&world, handle).IsEnabled());

    std::printf("[Alloc] %zu re-enables of an authored Disabled tag: %zu allocations\n",
                kMeasuredConstructions, allocations);

    EXPECT_EQ(allocations, 0u)
        << "re-enabling an entity that carries Disabled alone allocated " << allocations
        << " times across " << kMeasuredConstructions
        << " transitions: the single-tag transition is not going through the cached edges.";
}

// Positive control. A probe that never counts would make the arms above pass whatever the
// query does, so prove the allocation counter sees a path that must allocate.
TEST(QueryAllocationTests, TheProbeSeesTheAllocationsOfEntityCreation)
{
    World world(nullptr);

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        PopulateArchetypeSpread(world, kArchetypeCount);
        allocations = probe.Count();
    }

    EXPECT_GT(allocations, 0u)
        << "the allocation probe counted nothing while an empty world grew "
        << kArchetypeCount
        << " archetypes — it is not wired up, so the arms above prove nothing";
}
