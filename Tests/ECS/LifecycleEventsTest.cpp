// LifecycleEventsTest.cpp — lock tests for the change-signaling P3
// Added<T>/Removed<T> lifecycle event buffers (design v0.2 §6.3):
// recording in the P-1 unified bodies + the enumerated creation/clone
// family, the dedicated once-per-frame swap (C6), the WorldReset signal on
// Clear (Q4), CreateBatchWithInit rollback purity (C15), and hook/event
// parity with the Clear carve-out.

#include <gtest/gtest.h>

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{

std::size_t CountOf(std::span<const EntityHandle> events, EntityHandle e)
{
    return static_cast<std::size_t>(std::count_if(
        events.begin(), events.end(), [&](EntityHandle h) { return h.id == e.id; }));
}

} // namespace

// ---------------------------------------------------------------------------
// Unsubscribed types record nothing — the zero-cost contract (§6.2).
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, UnsubscribedTypeRecordsNothing)
{
    World world; // no subscriptions at all

    auto e = world.CreateHandle(Position{1, 2, 3});
    world.AddComponentImmediate<Velocity>(e, Velocity{1, 0, 0});
    world.RemoveComponentImmediate<Velocity>(e);
    world.DestroyEntityImmediate(e);
    world.SwapLifecycleEvents();

    EXPECT_TRUE(world.GetAdded<Position>().empty());
    EXPECT_TRUE(world.GetRemoved<Position>().empty());
    EXPECT_TRUE(world.GetAdded<Velocity>().empty());
    EXPECT_TRUE(world.GetRemoved<Velocity>().empty());
}

// On a subscribed world, OTHER component types still record nothing.
TEST(LifecycleEvents, OtherComponentTypesDoNotRecord)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    auto e = world.CreateHandle(Velocity{1, 0, 0});
    world.AddComponentImmediate<Health>(e, Health{50});
    world.RemoveComponentImmediate<Health>(e);
    world.SwapLifecycleEvents();

    EXPECT_TRUE(world.GetAdded<Position>().empty());
    EXPECT_TRUE(world.GetRemoved<Position>().empty());
    EXPECT_TRUE(world.GetAdded<Velocity>().empty()); // unsubscribed
    EXPECT_TRUE(world.GetRemoved<Health>().empty()); // unsubscribed
}

// ---------------------------------------------------------------------------
// DeferredAndImmediateBothRecorded (§6.3, EXTENDED C3/C11): every add-shaped
// path records exactly once — deferred playback, immediate, typed create,
// batch create, batch-with-init, archetype create, bundle, batch-bundle,
// clone (Ctrl+D shape), signature-create, blob-typed, ABI byte-set.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, DeferredAndImmediateBothRecorded)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    // Deferred command → playback.
    auto eDeferred = world.CreateEntity();
    world.AddComponent<Position>(eDeferred, Position{1, 0, 0});
    world.ProcessCommands();

    // Immediate typed add.
    auto eImmediate = world.CreateEntity();
    world.AddComponentImmediate<Position>(eImmediate, Position{2, 0, 0});

    // Typed creation family.
    auto eCreate = world.CreateHandle(Position{3, 0, 0});
    auto batch = world.CreateBatchHandle<Position>(2, Position{4, 0, 0});
    auto batchInit = world.CreateBatchWithInit<Position>(
        2, [](std::size_t i, Position& p) { p.x = static_cast<float32>(i); });
    auto eInArchetype = world.CreateInArchetypeHandle<Position>();

    // Bundle family.
    ComponentBundle bundle;
    bundle.Add(Position{5, 0, 0});
    auto eBundle = world.CreateFromBundleHandle(bundle);
    auto batchBundle = world.CreateBatchFromBundle(2, bundle);

    // Clone (editor Ctrl+D shape, C3).
    auto eClone = world.CloneEntity(eCreate);

    // Signature-create: query-visible entity, no data written (C3).
    ComponentSignature sig;
    sig.Add(GetComponentTypeId<Position>());
    auto eSignature = world.CreateFromSignatureHandle(sig);

    // ABI-driven type-erased byte-set (C11).
    auto eAbi = world.CreateEntity();
    const Position abiBytes{6, 0, 0};
    ASSERT_TRUE(world.SetComponentBytesImmediate(eAbi, GetComponentTypeId<Position>(),
                                                 &abiBytes, sizeof(abiBytes)));

    world.SwapLifecycleEvents();
    auto added = world.GetAdded<Position>();

    EXPECT_EQ(CountOf(added, eDeferred), 1u);
    EXPECT_EQ(CountOf(added, eImmediate), 1u);
    EXPECT_EQ(CountOf(added, eCreate), 1u);
    for (EntityHandle h : batch)
        EXPECT_EQ(CountOf(added, h), 1u);
    for (EntityHandle h : batchInit)
        EXPECT_EQ(CountOf(added, h), 1u);
    EXPECT_EQ(CountOf(added, eInArchetype), 1u);
    EXPECT_EQ(CountOf(added, eBundle), 1u);
    for (EntityHandle h : batchBundle)
        EXPECT_EQ(CountOf(added, h), 1u);
    EXPECT_EQ(CountOf(added, eClone), 1u);
    EXPECT_EQ(CountOf(added, eSignature), 1u);
    EXPECT_EQ(CountOf(added, eAbi), 1u);

    // Exactly the 14 adds above — nothing double-recorded.
    EXPECT_EQ(added.size(), 14u);
}

// Blob-typed component (C11): registered by name at runtime, added via the
// type-erased path — the unified body records it like any typed component.
TEST(LifecycleEvents, BlobTypedComponentRecords)
{
    World world;
    const ComponentTypeId blobId =
        ComponentRegistry::RegisterBlobComponent("P3LifecycleTestBlob", 16);
    ASSERT_NE(blobId, 0u);
    world.EnableLifecycleEvents(blobId);

    auto e = world.CreateEntity();
    const uint8_t bytes[16] = {};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, blobId, bytes, sizeof(bytes)));

    world.SwapLifecycleEvents();
    EXPECT_EQ(CountOf(world.GetAdded(blobId), e), 1u);

    // Blob removal funnels through the unified remove body the same way.
    ASSERT_TRUE(world.RemoveComponentByTypeIdImmediate(e, blobId));
    world.SwapLifecycleEvents();
    EXPECT_EQ(CountOf(world.GetRemoved(blobId), e), 1u);
}

// ---------------------------------------------------------------------------
// SetIsNotAdd (§6.3): data-only updates (typed fast path, type-erased
// data-only path, undo byte-restore onto an existing component) record no
// Added event.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, SetIsNotAdd)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    auto e = world.CreateHandle(Position{1, 0, 0});
    std::vector<uint8_t> captured;
    ASSERT_TRUE(world.CaptureComponentBytes(e, GetComponentTypeId<Position>(), captured));

    // Drain the creation event out of both windows.
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents();

    world.AddComponentImmediate<Position>(e, Position{2, 0, 0}); // typed data-only SET
    const Position bytes{3, 0, 0};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, GetComponentTypeId<Position>(),
                                                 &bytes, sizeof(bytes))); // ABI data-only SET
    ASSERT_TRUE(world.ApplyComponentBytesImmediate(e, GetComponentTypeId<Position>(),
                                                   captured)); // undo restore onto existing
    world.AddComponent<Position>(e, Position{4, 0, 0});
    world.ProcessCommands(); // deferred SET playback onto existing

    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetAdded<Position>().empty());
    EXPECT_TRUE(world.GetRemoved<Position>().empty());
}

// ---------------------------------------------------------------------------
// Removal paths: typed immediate, type-erased, deferred playback each record
// exactly once; removing an absent component records nothing.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, RemovalPathsRecordOnce)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    auto e1 = world.CreateHandle(Position{1, 0, 0});
    auto e2 = world.CreateHandle(Position{2, 0, 0});
    auto e3 = world.CreateHandle(Position{3, 0, 0});
    auto eAbsent = world.CreateHandle(Velocity{0, 0, 0});
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents(); // drain creation events

    world.RemoveComponentImmediate<Position>(e1);                              // typed
    world.RemoveComponentByTypeIdImmediate(e2, GetComponentTypeId<Position>()); // type-erased
    world.RemoveComponent<Position>(e3);                                       // deferred
    world.ProcessCommands();
    world.RemoveComponentImmediate<Position>(eAbsent); // absent — no event

    world.SwapLifecycleEvents();
    auto removed = world.GetRemoved<Position>();
    EXPECT_EQ(CountOf(removed, e1), 1u);
    EXPECT_EQ(CountOf(removed, e2), 1u);
    EXPECT_EQ(CountOf(removed, e3), 1u);
    EXPECT_EQ(CountOf(removed, eAbsent), 0u);
    EXPECT_EQ(removed.size(), 3u);
}

// ---------------------------------------------------------------------------
// DestroyEmitsRemovedPerSubscribedType (§6.3, EXTENDED C4): destroying an
// entity with three subscribed types emits exactly three Removed entries;
// unsubscribed types absent; identically across all per-entity destroy paths
// (immediate, deferred playback, preserve-handle) — and a preserve-handle
// revive + byte-restore re-emits Added via the unified add body.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, DestroyEmitsRemovedPerSubscribedType)
{
    World world;
    world.EnableLifecycleEvents<Position>();
    world.EnableLifecycleEvents<Velocity>();
    world.EnableLifecycleEvents<Health>();

    auto makeEntity = [&] {
        // Rotation stays unsubscribed on purpose.
        return world.CreateHandle(Position{1, 0, 0}, Velocity{2, 0, 0},
                                  Health{100}, Rotation{0, 0, 0, 1});
    };
    auto drain = [&] {
        world.SwapLifecycleEvents();
        world.SwapLifecycleEvents();
    };
    auto expectRemovedOnce = [&](EntityHandle e) {
        EXPECT_EQ(CountOf(world.GetRemoved<Position>(), e), 1u);
        EXPECT_EQ(CountOf(world.GetRemoved<Velocity>(), e), 1u);
        EXPECT_EQ(CountOf(world.GetRemoved<Health>(), e), 1u);
        EXPECT_TRUE(world.GetRemoved<Rotation>().empty()); // unsubscribed
    };

    // Path 1: immediate destroy.
    auto e1 = makeEntity();
    drain();
    world.DestroyEntityImmediate(e1);
    world.SwapLifecycleEvents();
    expectRemovedOnce(e1);

    // Path 2: deferred destroy → playback.
    auto e2 = makeEntity();
    drain();
    world.DestroyEntity(e2);
    world.ProcessCommands();
    world.SwapLifecycleEvents();
    expectRemovedOnce(e2);

    // Path 3: preserve-handle destroy (editor undo/redo, C4).
    auto e3 = makeEntity();
    std::vector<uint8_t> capturedPos;
    ASSERT_TRUE(world.CaptureComponentBytes(e3, GetComponentTypeId<Position>(), capturedPos));
    drain();
    world.DestroyEntityImmediatePreserveHandle(e3);
    world.SwapLifecycleEvents();
    expectRemovedOnce(e3);

    // Revive + byte-restore re-emits Added through the unified add body.
    world.SwapLifecycleEvents(); // drain the Removed window
    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(e3));
    ASSERT_TRUE(world.ApplyComponentBytesImmediate(e3, GetComponentTypeId<Position>(), capturedPos));
    world.SwapLifecycleEvents();
    EXPECT_EQ(CountOf(world.GetAdded<Position>(), e3), 1u);
}

// ---------------------------------------------------------------------------
// OneFrameVisibility (§6.3): an event surfaces in the frame after its swap
// and is discarded by the next one.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, OneFrameVisibility)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    auto e = world.CreateHandle(Position{1, 0, 0});
    EXPECT_TRUE(world.GetAdded<Position>().empty()); // pending, not yet visible

    world.SwapLifecycleEvents();
    EXPECT_EQ(CountOf(world.GetAdded<Position>(), e), 1u); // frame N

    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetAdded<Position>().empty()); // gone in N+1
}

// Add-then-remove within one frame delivers BOTH events (§6.2 semantics).
TEST(LifecycleEvents, AddThenRemoveSameFrameDeliversBoth)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Position>(e, Position{1, 0, 0});
    world.RemoveComponentImmediate<Position>(e);

    world.SwapLifecycleEvents();
    EXPECT_EQ(CountOf(world.GetAdded<Position>(), e), 1u);
    EXPECT_EQ(CountOf(world.GetRemoved<Position>(), e), 1u);
}

// ---------------------------------------------------------------------------
// MidFrameFlushDoesNotSwap (§6.3, NEW C6): ProcessCommands must never touch
// the event windows — the current window is stable across any number of
// mid-frame flushes until the explicit engine-tick swap.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, MidFrameFlushDoesNotSwap)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    // Put event A into the CURRENT window.
    auto eA = world.CreateHandle(Position{1, 0, 0});
    world.SwapLifecycleEvents();
    ASSERT_EQ(CountOf(world.GetAdded<Position>(), eA), 1u);

    // Record event B deferred, then flush three times mid-frame.
    auto eB = world.CreateEntity();
    world.AddComponent<Position>(eB, Position{2, 0, 0});
    world.ProcessCommands();
    world.ProcessCommands();
    world.ProcessCommands();

    // Current window unchanged: still exactly {A}; B stays pending.
    auto added = world.GetAdded<Position>();
    EXPECT_EQ(added.size(), 1u);
    EXPECT_EQ(CountOf(added, eA), 1u);
    EXPECT_EQ(CountOf(added, eB), 0u);

    // Only the explicit swap promotes B (and discards A).
    world.SwapLifecycleEvents();
    added = world.GetAdded<Position>();
    EXPECT_EQ(added.size(), 1u);
    EXPECT_EQ(CountOf(added, eB), 1u);
}

// ---------------------------------------------------------------------------
// RollbackEmitsNothing (§6.3, NEW C15): a CreateBatchWithInit whose initFn
// throws rolls back every created entity — zero Added (recording happens
// after initFn completes) and zero Removed (rollback bypasses the destroy
// body: the entities were never observable).
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, RollbackEmitsNothing)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    EXPECT_THROW(
        world.CreateBatchWithInit<Position>(
            4,
            [](std::size_t i, Position& p) {
                if (i == 2)
                    throw std::runtime_error("init failure");
                p.x = static_cast<float32>(i);
            }),
        std::runtime_error);

    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetAdded<Position>().empty());
    EXPECT_TRUE(world.GetRemoved<Position>().empty());
}

// ---------------------------------------------------------------------------
// WorldResetSignal (§6.3, NEW Q4): Clear() bumps the reset generation, wipes
// pending AND current windows, and emits no per-entity Removed events. The
// subscription survives (bootstrap-time property).
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, WorldResetSignal)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    // Populate CURRENT (create + swap) and PENDING (create after swap).
    auto e1 = world.CreateHandle(Position{1, 0, 0});
    world.SwapLifecycleEvents();
    auto e2 = world.CreateHandle(Position{2, 0, 0});
    ASSERT_EQ(CountOf(world.GetAdded<Position>(), e1), 1u);
    (void)e2;

    const uint64 generationBefore = world.GetLifecycleResetGeneration();
    world.Clear();

    EXPECT_EQ(world.GetLifecycleResetGeneration(), generationBefore + 1);
    EXPECT_TRUE(world.GetAdded<Position>().empty()); // current wiped immediately

    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetAdded<Position>().empty());   // pending was wiped too
    EXPECT_TRUE(world.GetRemoved<Position>().empty()); // no per-entity burst

    // Subscription survives Clear: new-scene entities record normally.
    auto e3 = world.CreateHandle(Position{3, 0, 0});
    world.SwapLifecycleEvents();
    EXPECT_EQ(CountOf(world.GetAdded<Position>(), e3), 1u);
}

// ---------------------------------------------------------------------------
// HookParity (§6.3, CARVE-OUT Q4): OnRemove hook and Removed event fire for
// the same per-entity operations (the hook receives the live T&) — except
// Clear, where hooks fire per-entity but events are replaced by the
// WorldReset signal.
// ---------------------------------------------------------------------------
namespace
{
int g_HookParityRemoveCount = 0;
float g_HookParityLastX = 0.0f;
} // namespace

TEST(LifecycleEvents, HookParityWithClearCarveOut)
{
    World world;
    world.EnableLifecycleEvents<Position>();
    g_HookParityRemoveCount = 0;
    g_HookParityLastX = 0.0f;
    world.RegisterOnRemove<Position>(+[](Position& p) {
        ++g_HookParityRemoveCount;
        g_HookParityLastX = p.x; // live T& — removal-with-data stays on hooks
    });

    // Component removal: hook and event both fire.
    auto e1 = world.CreateHandle(Position{7, 0, 0});
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents();
    world.RemoveComponentImmediate<Position>(e1);
    world.SwapLifecycleEvents();
    EXPECT_EQ(g_HookParityRemoveCount, 1);
    EXPECT_EQ(g_HookParityLastX, 7.0f);
    EXPECT_EQ(CountOf(world.GetRemoved<Position>(), e1), 1u);

    // Entity destroy: hook and event both fire.
    auto e2 = world.CreateHandle(Position{9, 0, 0});
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents();
    world.DestroyEntityImmediate(e2);
    world.SwapLifecycleEvents();
    EXPECT_EQ(g_HookParityRemoveCount, 2);
    EXPECT_EQ(g_HookParityLastX, 9.0f);
    EXPECT_EQ(CountOf(world.GetRemoved<Position>(), e2), 1u);

    // Clear: hooks fire per-entity, events do NOT (WorldReset signal instead).
    auto e3 = world.CreateHandle(Position{11, 0, 0});
    (void)e3;
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents();
    const uint64 generationBefore = world.GetLifecycleResetGeneration();
    world.Clear();
    world.SwapLifecycleEvents();
    EXPECT_EQ(g_HookParityRemoveCount, 3); // hook fired for e3
    EXPECT_EQ(g_HookParityLastX, 11.0f);
    EXPECT_TRUE(world.GetRemoved<Position>().empty());
    EXPECT_EQ(world.GetLifecycleResetGeneration(), generationBefore + 1);
}

// ---------------------------------------------------------------------------
// SwapGenerationDetectsMissedWindows (the F1 cadence guard): the swap
// generation increments once per swap; a consumer that was idle across a
// whole window observes a gap > 1, its events are gone (exact count), and
// the prescribed one-shot bookkeeping re-scan recovers the orphaned state.
// Models the editor's play-mode pause (SetRenderingSystemEnabled disables
// the consumer while the engine tick keeps swapping).
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, SwapGenerationDetectsMissedWindows)
{
    World world;
    world.EnableLifecycleEvents<Position>();

    // Simulated consumer state: handle-keyed bookkeeping + cached generation.
    std::vector<EntityHandle> bookkeeping;
    uint64 lastConsumedGeneration = world.GetLifecycleSwapGeneration();

    // Normal cadence: one swap per frame => gap of exactly 1, event present.
    auto e = world.CreateHandle(Position{1, 0, 0});
    world.SwapLifecycleEvents();
    {
        const uint64 generation = world.GetLifecycleSwapGeneration();
        EXPECT_EQ(generation - lastConsumedGeneration, 1u); // no sweep needed
        EXPECT_EQ(CountOf(world.GetAdded<Position>(), e), 1u);
        bookkeeping.push_back(e); // consumer seeds its record
        lastConsumedGeneration = generation;
    }

    // Consumer goes idle (pause). The entity dies; the swap keeps ticking.
    world.DestroyEntityImmediate(e);
    world.SwapLifecycleEvents(); // Removed promoted to current — unseen
    world.SwapLifecycleEvents(); // Removed discarded

    // Consumer resumes: the Removed event is gone (exact count 0) and the
    // generation gap says so.
    const uint64 generation = world.GetLifecycleSwapGeneration();
    EXPECT_EQ(world.GetRemoved<Position>().size(), 0u); // event lost, provably
    EXPECT_EQ(generation - lastConsumedGeneration, 2u); // gap > 1 => sweep

    // Prescribed recovery: one-shot re-scan of the bookkeeping. Exactly the
    // one orphaned record is detected dead and dropped.
    std::size_t swept = 0;
    std::erase_if(bookkeeping, [&](EntityHandle h) {
        const bool stale = !world.IsValid(h);
        swept += stale ? 1u : 0u;
        return stale;
    });
    EXPECT_EQ(swept, 1u);
    EXPECT_TRUE(bookkeeping.empty());
    lastConsumedGeneration = generation;

    // Back to normal cadence: the next window delivers again, gap == 1.
    auto e2 = world.CreateHandle(Position{2, 0, 0});
    world.SwapLifecycleEvents();
    EXPECT_EQ(world.GetLifecycleSwapGeneration() - lastConsumedGeneration, 1u);
    EXPECT_EQ(CountOf(world.GetAdded<Position>(), e2), 1u);
}

// ---------------------------------------------------------------------------
// CloneEmitsPerSubscribedType: the clone records one Added per SUBSCRIBED
// type in the archetype signature — not per component.
// ---------------------------------------------------------------------------
TEST(LifecycleEvents, CloneEmitsPerSubscribedType)
{
    World world;
    world.EnableLifecycleEvents<Position>();
    world.EnableLifecycleEvents<Velocity>();

    auto source = world.CreateHandle(Position{1, 0, 0}, Velocity{2, 0, 0},
                                     Rotation{0, 0, 0, 1}); // Rotation unsubscribed
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents();

    auto clone = world.CloneEntity(source);
    world.SwapLifecycleEvents();

    EXPECT_EQ(CountOf(world.GetAdded<Position>(), clone), 1u);
    EXPECT_EQ(CountOf(world.GetAdded<Velocity>(), clone), 1u);
    EXPECT_TRUE(world.GetAdded<Rotation>().empty());
    EXPECT_EQ(CountOf(world.GetAdded<Position>(), source), 0u); // source unaffected
}
