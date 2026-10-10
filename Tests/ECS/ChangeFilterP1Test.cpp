// ChangeFilterP1Test.cpp — lock tests for the change-signaling P1 mechanism:
// per-(chunk x column) uint64 write-grant versions + the Changed<> query
// filter + per-parameter read/write inference.
//
// Unlike the Step-0 prototype test, no environment variable is involved:
// stamping is always on, and the filter engages wherever a consumer passes a
// ChangeGate. GE_ECS_CHANGE_FILTER gates only the production consumers
// (TransformHierarchySystem), not the mechanism under test here.
#include <gtest/gtest.h>
#include <utility> // std::as_const
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"
#include "ECS/ComponentRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <atomic>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace {

// Overloaded functor (namespace scope — MSVC C5046): both call operators are
// viable, so inference classifies the slot as a read and the CONST overload
// must be selected (design §4.3 case table, "Overloaded functors").
struct OverloadedVisitor {
    static inline std::atomic<int> ConstCalls{0};
    static inline std::atomic<int> MutableCalls{0};
    void operator()(GameEngine::ECS::EntityHandle, const GameEngine::ECS::test::Position&) const {
        ConstCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void operator()(GameEngine::ECS::EntityHandle, GameEngine::ECS::test::Position&) const {
        MutableCalls.fetch_add(1, std::memory_order_relaxed);
    }
};

// Count entities visited by a Changed<Position>-gated read-only BatchEach.
// The callback takes const pointers, so the probe classifies every slot as a
// read and the scan itself never stamps.
size_t VisitedSince(World& world, uint64_t gate) {
    ChangeGate g;
    g.LastRunVersion = gate;
    size_t visited = 0;
    auto q = world.Query<Read<Position>>();
    q.Changed<Position>(g);
    q.BatchEach([&](const Position*, std::size_t count) { visited += count; });
    return visited;
}

size_t VisitedHealthSince(World& world, uint64_t gate) {
    ChangeGate g;
    g.LastRunVersion = gate;
    size_t visited = 0;
    auto q = world.Query<Read<Health>>();
    q.Changed<Health>(g);
    q.BatchEach([&](const Health*, std::size_t count) { visited += count; });
    return visited;
}

// Populate `count` Position entities via the immediate creation path.
std::vector<EntityHandle> Populate(World& world, size_t count) {
    std::vector<EntityHandle> handles;
    handles.reserve(count);
    for (size_t i = 0; i < count; ++i)
        handles.push_back(world.CreateHandle(Position{static_cast<float32>(i), 0, 0}));
    return handles;
}

} // namespace

// ---------------------------------------------------------------------------
// CleanFrameVisitsZeroChunks (§4.6): a Changed-filtered query run twice with
// no writes in between visits zero chunks the second time. Includes a fresh
// entity in a freshly-allocated chunk — its visibility must come from the
// Append stamp, never from zero-init (C7).
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, CleanFrameVisitsZeroChunks) {
    World world(nullptr);
    constexpr size_t kEntities = 20000;
    Populate(world, kEntities);

    // A zero gate sees everything once.
    EXPECT_EQ(VisitedSince(world, 0), kEntities);

    // Clean frame: entry-sample, no writes → zero visits.
    const uint64_t gate = world.GetGlobalSystemVersion();
    EXPECT_EQ(VisitedSince(world, gate), 0u);

    // Fresh entity, possibly in a brand-new chunk: allocate more than one
    // chunk's worth so at least one row lands in memory the gate has never
    // seen (C7 — zero-init is "never changed", the Append stamp is what makes
    // these visible).
    const uint64_t gate2 = world.GetGlobalSystemVersion();
    auto* archetype = world.GetEntityArchetype(world.CreateHandle(Position{1, 1, 1}));
    ASSERT_NE(archetype, nullptr);
    const size_t chunkCapacity = std::as_const(*archetype).GetTable().GetLayout().Capacity;
    Populate(world, chunkCapacity + 1);
    EXPECT_GE(VisitedSince(world, gate2), chunkCapacity + 2);

    // And clean again afterwards.
    EXPECT_EQ(VisitedSince(world, world.GetGlobalSystemVersion()), 0u);
}

// ---------------------------------------------------------------------------
// SingleWriteDirtiesExactlyOneChunk (§4.6): one entity written via each path
// class dirties only its owning chunk(s), and every audited path class
// re-dirties (no false negatives).
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, SingleWriteDirtiesExactlyOneChunk) {
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(&jobSystem);
    constexpr size_t kEntities = 4000; // several chunks
    auto handles = Populate(world, kEntities);

    auto* archetype = world.GetEntityArchetype(handles[0]);
    ASSERT_NE(archetype, nullptr);
    const size_t chunkCapacity = std::as_const(*archetype).GetTable().GetLayout().Capacity;
    ASSERT_GT(kEntities, chunkCapacity * 2) << "need multiple chunks for this test";

    auto expectOneChunk = [&](uint64_t gate, const char* path) {
        const size_t visited = VisitedSince(world, gate);
        EXPECT_GT(visited, 0u) << path << ": write was not seen (false negative)";
        EXPECT_LE(visited, chunkCapacity) << path << ": more than one chunk dirtied";
    };

    // 1. Immediate typed set (unified add-or-set body, data-only fast path).
    uint64_t g = world.GetGlobalSystemVersion();
    world.AddComponentImmediate(handles[7], Position{1, 2, 3});
    expectOneChunk(g, "AddComponentImmediate");

    // 2. GetComponentForWrite — the name-split write grant (the editor's
    //    set_component handler writes in place through this pointer).
    g = world.GetGlobalSystemVersion();
    auto* p = world.GetComponentForWrite<Position>(handles[100]);
    ASSERT_NE(p, nullptr);
    p->x = 42.0f;
    expectOneChunk(g, "GetComponentForWrite");

    // 3. Deferred SET via command playback.
    g = world.GetGlobalSystemVersion();
    world.AddComponent(handles[handles.size() - 5], Position{5, 5, 5});
    world.ProcessCommands();
    expectOneChunk(g, "deferred AddComponent playback");

    // 4. Type-erased byte set (ABI / editor undo shape).
    g = world.GetGlobalSystemVersion();
    const Position undo{9, 9, 9};
    EXPECT_TRUE(world.SetComponentBytesImmediate(handles[200], GetComponentTypeId<Position>(),
                                                 &undo, sizeof(undo)));
    expectOneChunk(g, "SetComponentBytesImmediate");

    // 5. Scripting-ABI span grant (mutable GetChunkDataRaw).
    g = world.GetGlobalSystemVersion();
    auto [span, count] = archetype->GetChunkDataRaw(GetComponentTypeId<Position>(), 0);
    ASSERT_NE(span, nullptr);
    ASSERT_GT(count, 0u);
    expectOneChunk(g, "mutable GetChunkDataRaw");

    // 6. Query lambda (write-visit stamping): an unqualified mutable param
    //    stamps every visited chunk whether or not the body writes.
    g = world.GetGlobalSystemVersion();
    world.Query<Position>().Each([](EntityHandle, Position&) {});
    EXPECT_EQ(VisitedSince(world, g), kEntities) << "write-visit must stamp every visited chunk";

    // 7. MoveEntity, both directions: adding Health moves the entity out of
    //    the Position archetype (src chunk swap-rewrite stamps) into the
    //    Position+Health archetype (Append stamps all columns).
    g = world.GetGlobalSystemVersion();
    world.AddComponentImmediate(handles[300], Health{50, 100});
    {
        const size_t visited = VisitedSince(world, g);
        // Source chunk rewrite + destination chunk must both be visible.
        EXPECT_GT(visited, 0u) << "MoveEntity: no chunk dirtied";
        EXPECT_LE(visited, 2 * chunkCapacity) << "MoveEntity: more than src+dst dirtied";
    }
    // ...and back (remove direction).
    g = world.GetGlobalSystemVersion();
    world.RemoveComponentImmediate<Health>(handles[300]);
    EXPECT_GT(VisitedSince(world, g), 0u) << "MoveEntity remove direction: no chunk dirtied";

    // 8. CreateHandle (fresh row).
    g = world.GetGlobalSystemVersion();
    world.CreateHandle(Position{7, 7, 7});
    expectOneChunk(g, "CreateHandle");

    // 9. CreateBatchWithInit — the write happens inside user initFn, into
    //    refs handed out of column storage.
    g = world.GetGlobalSystemVersion();
    world.CreateBatchWithInit<Position>(3, [](std::size_t i, Position& pos) {
        pos = Position{static_cast<float32>(i), 1, 1};
    });
    expectOneChunk(g, "CreateBatchWithInit");

    // 10. CloneEntity (editor Ctrl+D shape — CopyRow into an Append-stamped row).
    g = world.GetGlobalSystemVersion();
    EXPECT_TRUE(world.CloneEntity(handles[400]).IsValid());
    expectOneChunk(g, "CloneEntity");

    // 11. SetBulkComponents data-only cached-offset write.
    g = world.GetGlobalSystemVersion();
    world.SetBulkComponents<Position>({handles[500]}, {Position{3, 3, 3}});
    expectOneChunk(g, "SetBulkComponents");

    // 12. Destroy (swap-remove rewrites the vacated slot).
    g = world.GetGlobalSystemVersion();
    world.DestroyEntityImmediate(handles[3]);
    expectOneChunk(g, "DestroyEntityImmediate");

    // 13. Preserve-handle destroy (editor undo/redo shape).
    g = world.GetGlobalSystemVersion();
    world.DestroyEntityImmediatePreserveHandle(handles[4]);
    expectOneChunk(g, "DestroyEntityImmediatePreserveHandle");
}

// ---------------------------------------------------------------------------
// ConstAccessorsDoNotStamp (§4.6, C1): const Entity::Get, Entity::Has,
// World::HasComponent, const World::GetComponent, and const GetChunkDataRaw
// on a clean world produce zero stamps.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, ConstAccessorsDoNotStamp) {
    World world(nullptr);
    auto handles = Populate(world, 2000);

    const uint64_t gate = world.GetGlobalSystemVersion();

    // const World::GetComponent
    const World& constWorld = world;
    EXPECT_NE(constWorld.GetComponent<Position>(handles[10]), nullptr);

    // const Entity::Get + Entity::Has (both route through the const path).
    const Entity entity(&world, handles[11]);
    EXPECT_NE(entity.Get<Position>(), nullptr);
    EXPECT_TRUE(entity.Has<Position>());

    // World::HasComponent (templated + type-id form).
    EXPECT_TRUE(world.HasComponent<Position>(handles[12]));
    EXPECT_TRUE(world.HasComponent(handles[12], GetComponentTypeId<Position>()));

    // const GetChunkDataRaw (the read-only ABI span path).
    const Archetype* archetype = world.GetEntityArchetype(handles[0]);
    ASSERT_NE(archetype, nullptr);
    auto [ptr, count] = archetype->GetChunkDataRaw(GetComponentTypeId<Position>(), 0);
    EXPECT_NE(ptr, nullptr);
    EXPECT_GT(count, 0u);

    // Read-qualified and inferred-read query visits.
    world.Query<Read<Position>>().Each([](EntityHandle, const Position&) {});
    world.Query<Position>().Each([](EntityHandle, const Position&) {}); // inferred read
    world.Query<Read<Position>>().BatchEach([](const Position*, std::size_t) {});

    EXPECT_EQ(VisitedSince(world, gate), 0u) << "a read stamped a chunk (const laundering)";
}

// ---------------------------------------------------------------------------
// Effective sets (M13): Optional<T> read through a const pointer must NOT
// land in the write set — only actually-written columns stamp. This was hit
// empirically in Step-0 (Optional-in-write-set stamping storms).
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, OptionalConstPointerIsRead) {
    World world(nullptr);
    auto handles = Populate(world, 1000);
    for (size_t i = 0; i < 100; ++i)
        world.AddComponentImmediate(handles[i], Health{100, 100});

    const uint64_t gate = world.GetGlobalSystemVersion();

    // Health via const pointer → inferred read; Position mutable → write.
    world.Query<Position, Optional<Health>>().Each(
        [](EntityHandle, Position&, const Health*) {});

    EXPECT_EQ(VisitedHealthSince(world, gate), 0u)
        << "const-pointer Optional access stamped the Health column (M13 regression)";
    EXPECT_GT(VisitedSince(world, gate), 0u) << "mutable Position param must stamp";

    // Mutable Optional pointer → write (the extraction MeshGPUData* shape).
    const uint64_t gate2 = world.GetGlobalSystemVersion();
    world.Query<Read<Position>, Optional<Health>>().Each(
        [](EntityHandle, const Position&, Health* h) {
            if (h)
                h->current = 99;
        });
    EXPECT_GT(VisitedHealthSince(world, gate2), 0u)
        << "mutable Optional pointer must stamp the Health column";
}

// ---------------------------------------------------------------------------
// StaleAndFreshGatesSurviveCounterChurn (§4.6, C8 — replaces WrapSafety):
// drive the global version past 2^32, then verify (a) a gate that idled
// across the entire churn still sees a subsequent change, and (b) a fresh
// zero gate sees everything. uint64 + plain monotonic compare makes wrap
// unreachable (~292 years at 10^9 grants/s) — this documents that instead of
// pretending to test wrap.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, StaleAndFreshGatesSurviveCounterChurn) {
    World world(nullptr);
    auto handles = Populate(world, 2000);

    // Consume the initial state, then idle the gate across the churn.
    const uint64_t idleGate = world.GetGlobalSystemVersion();
    EXPECT_EQ(VisitedSince(world, idleGate), 0u);

    // Churn: jump far past 2^32 (what a uint32 counter would have wrapped
    // through many times over).
    world.DebugAdvanceGlobalSystemVersion(uint64_t(1) << 33);
    ASSERT_GT(world.GetGlobalSystemVersion(), uint64_t(1) << 33);

    // Still clean — churn alone must not fabricate changes for the idle gate.
    EXPECT_EQ(VisitedSince(world, idleGate), 0u);

    // (a) The idled gate sees a post-churn change.
    world.AddComponentImmediate(handles[42], Position{1, 2, 3});
    EXPECT_GT(VisitedSince(world, idleGate), 0u);

    // (b) A fresh zero gate sees everything.
    EXPECT_EQ(VisitedSince(world, 0), 2000u);
}

// ---------------------------------------------------------------------------
// GateWriteBackIsEntrySample (§4.6, M14): a writer stamping between the
// consumer's entry sample and its write-back must be seen on the NEXT run.
// The consumer contract writes back the entry sample, so a concurrent
// same-wave stamp (necessarily > entry sample) compares greater next run.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, GateWriteBackIsEntrySample) {
    World world(nullptr);
    auto handles = Populate(world, 2000);

    ChangeGate gate;
    // Run 1: consume everything, write back the entry sample.
    uint64_t entry = world.GetGlobalSystemVersion();
    EXPECT_EQ(VisitedSince(world, gate.LastRunVersion), 2000u);
    gate.LastRunVersion = entry;

    // Run 2 begins: entry-sample, then a concurrent writer stamps DURING the
    // run (after the sample). The run itself may or may not see it.
    entry = world.GetGlobalSystemVersion();
    world.AddComponentImmediate(handles[10], Position{5, 5, 5}); // concurrent-writer stand-in
    (void)VisitedSince(world, gate.LastRunVersion);
    gate.LastRunVersion = entry; // entry sample — NEVER an end-of-run resample

    // Run 3: the concurrent write's stamp (> entry) must be visible.
    EXPECT_GT(VisitedSince(world, gate.LastRunVersion), 0u)
        << "entry-sample write-back lost a concurrent same-wave stamp";
}

// ---------------------------------------------------------------------------
// AdaptiveForwardsGate (§4.6, M14): a gated query dispatched through
// Adaptive filters identically to direct dispatch on every branch.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, AdaptiveForwardsGate) {
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(&jobSystem);
    auto handles = Populate(world, 2000); // lands in Adaptive's Parallel branch

    const uint64_t gate = world.GetGlobalSystemVersion();

    ChangeGate g;
    g.LastRunVersion = gate;
    std::atomic<size_t> visited{0};
    {
        auto q = world.Query<Read<Position>>();
        q.Changed<Position>(g);
        q.Adaptive([&](EntityHandle, const Position&) {
            visited.fetch_add(1, std::memory_order_relaxed);
        });
    }
    EXPECT_EQ(visited.load(), 0u) << "Adaptive dropped the Changed<> gate on a clean world";

    world.AddComponentImmediate(handles[123], Position{4, 4, 4});
    {
        auto q = world.Query<Read<Position>>();
        q.Changed<Position>(g);
        q.Adaptive([&](EntityHandle, const Position&) {
            visited.fetch_add(1, std::memory_order_relaxed);
        });
    }
    EXPECT_GT(visited.load(), 0u) << "Adaptive-forwarded gate missed a change";
}

// ---------------------------------------------------------------------------
// ParallelBatchEach honors the gate identically to BatchEach (the flat
// hierarchy consumer's mode), including zero task dispatch on a clean frame.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, ParallelBatchEachHonorsGate) {
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(&jobSystem);
    auto handles = Populate(world, 8000);

    const uint64_t gate = world.GetGlobalSystemVersion();

    ChangeGate g;
    g.LastRunVersion = gate;
    std::atomic<size_t> visited{0};
    {
        auto q = world.Query<Read<Position>>();
        q.Changed<Position>(g);
        q.ParallelBatchEach([&](const Position*, std::size_t count) {
            visited.fetch_add(count, std::memory_order_relaxed);
        });
    }
    EXPECT_EQ(visited.load(), 0u);

    auto* p = world.GetComponentForWrite<Position>(handles[77]);
    ASSERT_NE(p, nullptr);
    p->y = 1.0f;
    {
        auto q = world.Query<Read<Position>>();
        q.Changed<Position>(g);
        q.ParallelBatchEach([&](const Position*, std::size_t count) {
            visited.fetch_add(count, std::memory_order_relaxed);
        });
    }
    EXPECT_GT(visited.load(), 0u);
}

// ---------------------------------------------------------------------------
// CompactRelocationKeepsStamps (§4.6, M14 extended): stamps travel with
// chunks through CompactChunks' swap+pop and trailing-free modes, through
// ArchetypeTable move construction/assignment, and across Reserve growth.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, CompactRelocationKeepsStamps) {
    World world(nullptr);
    auto handles = Populate(world, 1);
    auto* archetype = world.GetEntityArchetype(handles[0]);
    ASSERT_NE(archetype, nullptr);
    const size_t cap = std::as_const(*archetype).GetTable().GetLayout().Capacity;

    // Fill three full chunks (plus the one entity already there).
    auto more = Populate(world, cap * 3 - 1);
    handles.insert(handles.end(), more.begin(), more.end());
    ASSERT_GE(std::as_const(*archetype).GetTable().GetChunkCount(), 3u);

    // Empty chunk 1 (entities [cap, 2cap) by creation order).
    for (size_t i = cap; i < cap * 2; ++i)
        world.DestroyEntityImmediate(handles[i]);

    // Entry-sample, then dirty exactly one entity living in chunk 2.
    const uint64_t gate = world.GetGlobalSystemVersion();
    auto* p = world.GetComponentForWrite<Position>(handles[cap * 2 + 3]);
    ASSERT_NE(p, nullptr);
    p->z = 123.0f;

    // Compact: chunk 2 swaps into slot 1; its stamps must travel (swap+pop
    // mode). The destroy loop above also exercises swap-remove stamping, but
    // those stamps predate `gate`.
    EXPECT_GT(world.CompactAllChunks(), 0u);
    EXPECT_GT(VisitedSince(world, gate), 0u) << "stamp lost in CompactChunks swap";

    // Clean after compact: relocation alone must not fabricate changes for a
    // fresh gate.
    EXPECT_EQ(VisitedSince(world, world.GetGlobalSystemVersion()), 0u);

    // Trailing-free mode: empty the LAST chunk (the relocated former chunk 2),
    // compact, and verify stamps of surviving chunks stay intact.
    for (size_t i = cap * 2; i < cap * 3; ++i) {
        if (world.IsValid(handles[i]))
            world.DestroyEntityImmediate(handles[i]);
    }
    const uint64_t gate2 = world.GetGlobalSystemVersion();
    auto* p0 = world.GetComponentForWrite<Position>(handles[3]); // chunk 0 resident
    ASSERT_NE(p0, nullptr);
    p0->z = 55.0f;
    world.CompactAllChunks();
    EXPECT_GT(VisitedSince(world, gate2), 0u) << "stamp lost in trailing-free compact";

    // Table-level: move ctor/assign and Reserve growth preserve stamps.
    {
        std::vector<ComponentMeta> metas = {
            {1, sizeof(float), 4},
            {2, sizeof(double), 8},
        };
        ArchetypeTable table{std::span<const ComponentMeta>(metas)};
        table.AddEntity(EntityHandle(1, 1));
        table.StampColumnVersion(0, 1, 42);
        table.Reserve(table.GetLayout().Capacity * 2 + 1); // grow: new rows zeroed
        EXPECT_EQ(table.GetColumnVersion(0, 1), 42u);
        EXPECT_EQ(table.GetColumnVersion(1, 1), 0u);

        ArchetypeTable moved{std::move(table)};
        EXPECT_EQ(moved.GetColumnVersion(0, 1), 42u);

        std::vector<ComponentMeta> metas2 = {{1, sizeof(float), 4}};
        ArchetypeTable assigned{std::span<const ComponentMeta>(metas2)};
        assigned = std::move(moved);
        EXPECT_EQ(assigned.GetColumnVersion(0, 1), 42u);
    }
}

// ---------------------------------------------------------------------------
// FilterOffEqualsFilterOn (§4.6): a gated consumer maintaining its ChangeGate
// across frames produces bitwise-identical output to an ungated consumer,
// across a workload that writes through the audited path classes. This is
// the unit form of the hierarchy arc's OFF==ON idiom (the editor-level check
// runs in the PR's runtime gate).
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, FilterOffEqualsFilterOn) {
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(&jobSystem);
    constexpr size_t kEntities = 3000;
    auto handles = Populate(world, kEntities);
    for (auto h : handles)
        world.AddComponentImmediate(h, Velocity{0, 0, 0});

    ChangeGate gate; // the gated consumer's persistent state

    // Consumer: Velocity = Position (the hierarchy's root-resolve shape).
    auto runGated = [&] {
        const uint64_t entry = world.GetGlobalSystemVersion();
        auto q = world.Query<Read<Position>, Write<Velocity>>();
        q.Changed<Position>(gate);
        q.BatchEach([](const Position* in, Velocity* out, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i)
                out[i] = Velocity{in[i].x, in[i].y, in[i].z};
        });
        gate.LastRunVersion = entry; // entry-sample write-back (M14)
    };

    auto verifyParity = [&](const char* frame) {
        size_t mismatches = 0;
        world.Query<Read<Position>, Read<Velocity>>().Each(
            [&](EntityHandle, const Position& pos, const Velocity& vel) {
                if (vel.x != pos.x || vel.y != pos.y || vel.z != pos.z)
                    ++mismatches;
            });
        EXPECT_EQ(mismatches, 0u) << "gated consumer diverged from input state at " << frame;
    };

    runGated();
    verifyParity("frame 1 (initial)");

    runGated(); // clean frame — skips everything, output must stay correct
    verifyParity("frame 2 (clean)");

    // Frame 3: writes through several path classes.
    world.AddComponentImmediate(handles[10], Position{101, 0, 0});
    auto* p = world.GetComponentForWrite<Position>(handles[20]);
    ASSERT_NE(p, nullptr);
    p->x = 202.0f;
    const Position bytes{303, 0, 0};
    world.SetComponentBytesImmediate(handles[30], GetComponentTypeId<Position>(), &bytes,
                                     sizeof(bytes));
    world.AddComponent(handles[40], Position{404, 0, 0});
    world.ProcessCommands();
    runGated();
    verifyParity("frame 3 (mixed writers)");

    // Frame 4: clean again.
    runGated();
    verifyParity("frame 4 (clean)");

    // Frame 5: structural churn — destroy + create + clone, then verify.
    world.DestroyEntityImmediate(handles[50]);
    world.CreateHandle(Position{7, 7, 7}, Velocity{0, 0, 0});
    world.CloneEntity(handles[60]);
    world.ProcessCommands();
    runGated();
    verifyParity("frame 5 (structural)");
}

// ---------------------------------------------------------------------------
// Inference semantics across dispatch shapes: unqualified mutable params
// stamp, unqualified const params do not, Read<T> + deduced param binds
// const. (The reject/contradiction shapes are compile-fail tests —
// Tests/ECS/CompileFail/.)
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, InferenceStampsMatchParameterConstness) {
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(&jobSystem);
    Populate(world, 2000);

    // Unqualified + const param → read, no stamp (all shapes).
    uint64_t g = world.GetGlobalSystemVersion();
    world.Query<Position>().Each([](EntityHandle, const Position&) {});
    world.Query<Position>().Each([](const Position&) {});
    world.Query<Position>().BatchEach([](const Position*, std::size_t) {});
    world.Query<Position>().ForEachChunk([](const Position*, std::size_t) {});
    world.Query<Position>().Parallel([](EntityHandle, const Position&) {});
    world.Query<Position>().ParallelChunks([](const Position*, std::size_t) {});
    EXPECT_EQ(VisitedSince(world, g), 0u) << "an inferred-read shape stamped";

    // Read<T> + deduced param binds const (no stamp), per D-SDK: qualified
    // types keep deduced params legal. (A deduced param that WRITES under
    // Read<T> fails to compile — covered by the compile-fail matrix.)
    g = world.GetGlobalSystemVersion();
    world.Query<Read<Position>>().Each([](EntityHandle, auto&) {});
    EXPECT_EQ(VisitedSince(world, g), 0u);

    // Overloaded functor: both overloads viable → inferred read → const
    // overload selected, no stamp (design §4.3 case table).
    g = world.GetGlobalSystemVersion();
    OverloadedVisitor::ConstCalls = 0;
    OverloadedVisitor::MutableCalls = 0;
    world.Query<Position>().Each(OverloadedVisitor{});
    EXPECT_GT(OverloadedVisitor::ConstCalls.load(), 0);
    EXPECT_EQ(OverloadedVisitor::MutableCalls.load(), 0);
    EXPECT_EQ(VisitedSince(world, g), 0u) << "overloaded functor must infer read";

    // Unqualified + mutable param → write, stamps (each shape).
    g = world.GetGlobalSystemVersion();
    world.Query<Position>().Each([](EntityHandle, Position&) {});
    EXPECT_EQ(VisitedSince(world, g), 2000u) << "Each write-visit did not stamp";

    g = world.GetGlobalSystemVersion();
    world.Query<Position>().BatchEach([](Position*, std::size_t) {});
    EXPECT_EQ(VisitedSince(world, g), 2000u) << "BatchEach write-visit did not stamp";

    g = world.GetGlobalSystemVersion();
    world.Query<Position>().ParallelBatchEach([](Position*, std::size_t) {});
    EXPECT_EQ(VisitedSince(world, g), 2000u) << "ParallelBatchEach write-visit did not stamp";

    g = world.GetGlobalSystemVersion();
    world.Query<Position>().ForEachChunk([](Position*, std::size_t) {});
    EXPECT_EQ(VisitedSince(world, g), 2000u) << "ForEachChunk write-visit did not stamp";

    g = world.GetGlobalSystemVersion();
    world.Query<Position>().Parallel([](EntityHandle, Position&) {});
    EXPECT_EQ(VisitedSince(world, g), 2000u) << "Parallel write-visit did not stamp";
}


// ---------------------------------------------------------------------------
// GetEntityColumnVersion: the single-entity point probe over the same
// write-grant versions (the Inspector live-value gate's shape — a Changed<>
// query scan is wrong for watching ONE entity). Chunk-granular by contract:
// a co-located neighbor's write moves the probed version (over-report is
// legal; a miss is not).
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, EntityColumnVersionPointProbe) {
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(&jobSystem);
    auto handles = Populate(world, 8); // one chunk

    const ComponentTypeId posId = GetComponentTypeId<Position>();
    const ComponentTypeId healthId = GetComponentTypeId<Health>();

    // Invalid entity and missing component both read 0.
    EXPECT_EQ(world.GetEntityColumnVersion(EntityHandle{}, posId), 0u);
    EXPECT_EQ(world.GetEntityColumnVersion(handles[0], healthId), 0u);

    // Fresh entities carry the creation stamp (visibility from Append, C7).
    const uint64_t v0 = world.GetEntityColumnVersion(handles[0], posId);
    EXPECT_GT(v0, 0u);

    // Const read does not move the version.
    (void)world.GetComponent<Position>(handles[0]);
    EXPECT_EQ(world.GetEntityColumnVersion(handles[0], posId), v0);

    // Gate contract: a write after an entry sample compares strictly greater.
    const uint64_t gate = world.GetGlobalSystemVersion();
    auto* p = world.GetComponentForWrite<Position>(handles[0]);
    ASSERT_NE(p, nullptr);
    p->x = 1.0f;
    EXPECT_GT(world.GetEntityColumnVersion(handles[0], posId), gate);

    // Chunk granularity: the neighbor shares the column version (documented
    // over-report — the consumer's value compare absorbs it).
    EXPECT_EQ(world.GetEntityColumnVersion(handles[1], posId),
              world.GetEntityColumnVersion(handles[0], posId));
}

// ---------------------------------------------------------------------------
// StampComponentWriteBatch stamps each run of entities that share a chunk
// once, with the batch's one version: a bulk writer's entities, sorted by
// index, sit in long same-chunk runs. Runs are keyed by table and chunk, so
// entities of two archetypes interleaved by index (each table's chunk 0) are
// all stamped.
// ---------------------------------------------------------------------------
TEST(ChangeFilterP1, StampBatchStampsEachChunkRunOnce) {
    World world(nullptr);
    std::vector<EntityHandle> positionOnly;
    std::vector<EntityHandle> interleaved;
    for (int i = 0; i < 8; ++i) {
        const EntityHandle plain = world.CreateHandle(Position{static_cast<float32>(i), 0, 0});
        const EntityHandle withHealth = world.CreateHandle(Position{}, Health{10});
        positionOnly.push_back(plain);
        interleaved.push_back(plain);
        interleaved.push_back(withHealth);
    }
    world.ProcessCommands();
    const ComponentTypeId posId = GetComponentTypeId<Position>();

    // One table, one chunk: one stamp for the eight entities.
    uint64_t gate = world.GetGlobalSystemVersion();
    uint64_t stampsBefore = world.GetColumnStampCount();
    world.StampComponentWriteBatch(positionOnly.data(), positionOnly.size(), posId);
    EXPECT_EQ(world.GetColumnStampCount() - stampsBefore, 1u);
    for (EntityHandle handle : positionOnly)
        EXPECT_GT(world.GetEntityColumnVersion(handle, posId), gate);

    // Alternating tables at the same chunk index: every entity starts a new
    // run, and every one of them is stamped.
    gate = world.GetGlobalSystemVersion();
    stampsBefore = world.GetColumnStampCount();
    world.StampComponentWriteBatch(interleaved.data(), interleaved.size(), posId);
    EXPECT_EQ(world.GetColumnStampCount() - stampsBefore, interleaved.size());
    for (EntityHandle handle : interleaved)
        EXPECT_GT(world.GetEntityColumnVersion(handle, posId), gate) << "entity " << handle.index;
    EXPECT_EQ(VisitedSince(world, gate), interleaved.size());

    // One table spanning two chunks: a run ends at the chunk boundary, so the
    // entities past the first chunk are stamped too.
    // The capacity comes from a separate world, so no probe entity shares the
    // spanning world's first chunk.
    World probeWorld(nullptr);
    auto* archetype = probeWorld.GetEntityArchetype(probeWorld.CreateHandle(Position{}));
    ASSERT_NE(archetype, nullptr);
    const size_t chunkCapacity = std::as_const(*archetype).GetTable().GetLayout().Capacity;
    World spanWorld(nullptr);
    std::vector<EntityHandle> spanning = Populate(spanWorld, chunkCapacity + 1);
    spanWorld.ProcessCommands();
    gate = spanWorld.GetGlobalSystemVersion();
    stampsBefore = spanWorld.GetColumnStampCount();
    spanWorld.StampComponentWriteBatch(spanning.data(), spanning.size(), posId);
    EXPECT_EQ(spanWorld.GetColumnStampCount() - stampsBefore, 2u);
    EXPECT_EQ(VisitedSince(spanWorld, gate), chunkCapacity + 1);
}
