// DirtyFeedTest.cpp — lock tests for the change-signaling P2 per-entity dirty
// feed: World-member ComponentDirtyFeed + unified-Set-body emission
// + mutable-span-grant emission + swap/Clear lifecycle.
//
// Component-agnostic coverage lives here (the feed doesn't know about
// WorldTransform); the WorldTransform helper + SceneTlas consumer lock tests
// live in the Scene module's SceneTlasTests.

#include <gtest/gtest.h>

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/CachedQuery.h"
#include "ECS/ComponentDirtyFeed.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"

#include <algorithm>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{

std::vector<EntityHandle> Snapshot(World& world)
{
    std::vector<EntityHandle> out;
    world.GetComponentDirtyFeed().Snapshot(out);
    return out;
}

bool Contains(const std::vector<EntityHandle>& entries, EntityHandle e)
{
    return std::any_of(entries.begin(), entries.end(),
                       [&](EntityHandle h) { return h.id == e.id; });
}

} // namespace

// A world that never subscribed emits nothing for any Set shape.
TEST(DirtyFeed, UnsubscribedWorldEmitsNothing)
{
    World world;
    auto e = world.CreateHandle(Position{1, 2, 3});
    world.AddComponentImmediate<Position>(e, Position{4, 5, 6});
    const Position bytes{7, 8, 9};
    world.SetComponentBytesImmediate(e, GetComponentTypeId<Position>(), &bytes, sizeof(bytes));
    EXPECT_TRUE(Snapshot(world).empty());
    EXPECT_FALSE(world.IsComponentDirtyFeedEnabledFor(GetComponentTypeId<Position>()));
}

// Producer coverage across the Set-shaped writer class (seam-review rows
// 8-15): every funnel into the unified body emits exactly for the subscribed
// type.
TEST(DirtyFeed, TypedImmediateSetOnExistingComponentEmits)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    const auto baseline = Snapshot(world).size();

    world.AddComponentImmediate<Position>(e, Position{2, 0, 0}); // data-only SET branch
    auto entries = Snapshot(world);
    EXPECT_EQ(entries.size(), baseline + 1);
    EXPECT_TRUE(Contains(entries, e));
}

TEST(DirtyFeed, StructuralAddEmits)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Position>(e, Position{1, 0, 0}); // structural ADD branch
    EXPECT_TRUE(Contains(Snapshot(world), e));
}

TEST(DirtyFeed, SetComponentBytesImmediateEmits)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    const Position bytes{9, 9, 9};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, GetComponentTypeId<Position>(),
                                                 &bytes, sizeof(bytes)));
    EXPECT_TRUE(Contains(Snapshot(world), e));
}

TEST(DirtyFeed, DeferredSetPlaybackEmits)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    world.GetComponentDirtyFeed().Reset(); // isolate the deferred write

    world.AddComponent<Position>(e, Position{5, 0, 0}); // deferred; SET on playback
    EXPECT_TRUE(Snapshot(world).empty());               // not yet played back

    world.ProcessCommands(); // playback → handler → unified body → emit
    EXPECT_TRUE(Contains(Snapshot(world), e));
}

// Undo-style byte restore: consumers key on Version-changed (!=, any
// direction), so a restore of OLDER bytes must still produce an entry.
TEST(DirtyFeed, UndoByteRestoreEmits)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    std::vector<uint8_t> captured;
    ASSERT_TRUE(world.CaptureComponentBytes(e, GetComponentTypeId<Position>(), captured));

    world.AddComponentImmediate<Position>(e, Position{2, 0, 0});
    world.GetComponentDirtyFeed().Reset(); // isolate the restore

    ASSERT_TRUE(world.ApplyComponentBytesImmediate(e, GetComponentTypeId<Position>(), captured));
    EXPECT_TRUE(Contains(Snapshot(world), e));
}

// Unsubscribed component types never emit even on a subscribed world.
TEST(DirtyFeed, OtherComponentTypesDoNotEmit)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0}, Velocity{0, 0, 0});
    world.GetComponentDirtyFeed().Reset();

    world.AddComponentImmediate<Velocity>(e, Velocity{1, 1, 1});
    EXPECT_TRUE(Snapshot(world).empty());
}

// Amendment A4: a MUTABLE chunk-span grant (the managed GetChunkSpan<T> /
// v1.6 slice surface) is invisible to both the unified body and the bump
// helper — the grant itself must emit the chunk's entities (coarse
// over-report, symmetric with its coarse stamp-at-grant). The read-only
// overload must emit nothing.
TEST(DirtyFeed, MutableSpanGrantEmitsChunkEntities)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    std::vector<EntityHandle> handles;
    for (int i = 0; i < 5; ++i)
        handles.push_back(world.CreateHandle(Position{static_cast<float32>(i), 0, 0}));
    world.GetComponentDirtyFeed().Reset();

    CachedQuery q(&world, {GetComponentTypeId<Position>()}, {});
    q.Refresh();
    ASSERT_GE(q.GetArchetypeCount(), 1u);
    Archetype* archetype = q.GetArchetype(0);
    ASSERT_NE(archetype, nullptr);

    // Read-only grant: no stamp, no feed entries.
    {
        const Archetype* constArchetype = archetype;
        auto [ptr, count] =
            constArchetype->GetChunkDataRaw(GetComponentTypeId<Position>(), 0);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(count, handles.size());
        EXPECT_TRUE(Snapshot(world).empty());
    }

    // Mutable grant: every entity in the granted chunk lands in the feed.
    {
        auto [ptr, count] = archetype->GetChunkDataRaw(GetComponentTypeId<Position>(), 0);
        ASSERT_NE(ptr, nullptr);
        auto entries = Snapshot(world);
        EXPECT_EQ(entries.size(), handles.size());
        for (EntityHandle h : handles)
            EXPECT_TRUE(Contains(entries, h));
    }

    // A grant for an UNSUBSCRIBED type emits nothing.
    world.GetComponentDirtyFeed().Reset();
    world.AddComponentImmediate<Velocity>(handles[0], Velocity{1, 0, 0});
    world.GetComponentDirtyFeed().Reset();
    CachedQuery qv(&world, {GetComponentTypeId<Velocity>()}, {});
    qv.Refresh();
    ASSERT_GE(qv.GetArchetypeCount(), 1u);
    auto [vptr, vcount] =
        qv.GetArchetype(0)->GetChunkDataRaw(GetComponentTypeId<Velocity>(), 0);
    ASSERT_NE(vptr, nullptr);
    EXPECT_TRUE(Snapshot(world).empty());
}

// Frame-boundary swap semantics: an entry is readable via Snapshot both
// before and after ONE swap (pending → current), and discarded by the second
// swap — so it survives at least one full frame wherever it was appended.
TEST(DirtyFeed, SwapLifecycle)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    world.GetComponentDirtyFeed().Reset();
    world.AddComponentImmediate<Position>(e, Position{2, 0, 0});

    EXPECT_TRUE(Contains(Snapshot(world), e)); // pending
    world.SwapComponentDirtyFeed();
    EXPECT_TRUE(Contains(Snapshot(world), e)); // current
    world.SwapComponentDirtyFeed();
    EXPECT_TRUE(Snapshot(world).empty()); // discarded
}

// World::Clear() wipes both buffers — entity ids recycle after Clear, so a
// surviving entry would alias an unrelated entity in the next scene.
TEST(DirtyFeed, ClearWipesFeed)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    world.AddComponentImmediate<Position>(e, Position{2, 0, 0});
    world.SwapComponentDirtyFeed(); // put entries in current
    world.AddComponentImmediate<Position>(e, Position{3, 0, 0}); // and pending
    ASSERT_FALSE(Snapshot(world).empty());

    world.Clear();
    EXPECT_TRUE(Snapshot(world).empty());
    EXPECT_FALSE(world.GetComponentDirtyFeed().Overflowed());
    // Subscription is a bootstrap-time property and survives Clear.
    EXPECT_TRUE(world.IsComponentDirtyFeedEnabledFor(GetComponentTypeId<Position>()));
}

// Overflow: appends beyond the cap set the flag (consumers must take their
// poll fallback) and the flag follows the buffer through the swap.
TEST(DirtyFeed, OverflowSetsFlagAndClearsOnDiscard)
{
    ComponentDirtyFeed feed;
    EntityHandle h;
    h.id = 1;
    for (std::size_t i = 0; i < ComponentDirtyFeed::kOverflowCap + 1; ++i)
        feed.Append(h);
    EXPECT_TRUE(feed.Overflowed());

    feed.Swap(); // overflowed buffer becomes current — still incomplete
    EXPECT_TRUE(feed.Overflowed());

    feed.Swap(); // overflowed window discarded
    EXPECT_FALSE(feed.Overflowed());
}

// FeedMatchesVersionScan (lite, §5.3): across a mixed writer set, the feed
// snapshot equals exactly the set of written entities — no misses, no
// spurious extras.
TEST(DirtyFeed, FeedMatchesWrittenSetAcrossMixedWriters)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    std::vector<EntityHandle> handles;
    for (int i = 0; i < 64; ++i)
        handles.push_back(world.CreateHandle(Position{static_cast<float32>(i), 0, 0}));
    world.GetComponentDirtyFeed().Reset();

    std::vector<EntityHandle> written;
    // Typed immediate SET.
    world.AddComponentImmediate<Position>(handles[3], Position{100, 0, 0});
    written.push_back(handles[3]);
    // ABI byte SET.
    const Position bytes{200, 0, 0};
    world.SetComponentBytesImmediate(handles[17], GetComponentTypeId<Position>(),
                                     &bytes, sizeof(bytes));
    written.push_back(handles[17]);
    // Deferred SET playback.
    world.AddComponent<Position>(handles[42], Position{300, 0, 0});
    world.ProcessCommands();
    written.push_back(handles[42]);
    // Undo-style restore.
    std::vector<uint8_t> captured;
    ASSERT_TRUE(world.CaptureComponentBytes(handles[55], GetComponentTypeId<Position>(), captured));
    world.ApplyComponentBytesImmediate(handles[55], GetComponentTypeId<Position>(), captured);
    written.push_back(handles[55]);

    auto entries = Snapshot(world);
    EXPECT_EQ(entries.size(), written.size());
    for (EntityHandle w : written)
        EXPECT_TRUE(Contains(entries, w));
}

// Dead entries are legal feed content: a destroyed entity's entry survives in
// the feed (consumers IsValid-gate) and destruction itself needs no emission.
TEST(DirtyFeed, DeadEntriesAreTolerated)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());

    auto e = world.CreateHandle(Position{1, 0, 0});
    world.GetComponentDirtyFeed().Reset();
    world.AddComponentImmediate<Position>(e, Position{2, 0, 0});
    world.DestroyEntityImmediate(e);

    auto entries = Snapshot(world);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_FALSE(world.IsValid(entries[0]));
}
