#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h" // typed World operation definitions
#include "ECS/ArchetypeTable.h"
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

// ============================================================================
// Singleton API Tests
// ============================================================================

struct GameTime {
    float DeltaTime = 0.0f;
    float TotalTime = 0.0f;
    uint32_t FrameCount = 0;
};

struct InputState {
    bool KeyW = false;
    bool KeyA = false;
    bool KeyS = false;
    bool KeyD = false;
};

TEST(SingletonTest, SetAndGetSingleton) {
    World w(nullptr);

    GameTime time{0.016f, 1.5f, 90};
    w.SetSingleton(time);

    auto* t = w.GetSingleton<GameTime>();
    ASSERT_NE(t, nullptr);
    EXPECT_FLOAT_EQ(t->DeltaTime, 0.016f);
    EXPECT_FLOAT_EQ(t->TotalTime, 1.5f);
    EXPECT_EQ(t->FrameCount, 90u);
}

TEST(SingletonTest, GetSingletonReturnsNullIfNotSet) {
    World w(nullptr);
    EXPECT_EQ(w.GetSingleton<GameTime>(), nullptr);
}

TEST(SingletonTest, HasSingleton) {
    World w(nullptr);
    EXPECT_FALSE(w.HasSingleton<GameTime>());

    w.SetSingleton(GameTime{});
    EXPECT_TRUE(w.HasSingleton<GameTime>());
}

TEST(SingletonTest, RemoveSingleton) {
    World w(nullptr);
    w.SetSingleton(GameTime{0.016f, 0.0f, 0});

    EXPECT_TRUE(w.HasSingleton<GameTime>());
    w.RemoveSingleton<GameTime>();
    EXPECT_FALSE(w.HasSingleton<GameTime>());
    EXPECT_EQ(w.GetSingleton<GameTime>(), nullptr);
}

TEST(SingletonTest, OverwriteSingleton) {
    World w(nullptr);
    w.SetSingleton(GameTime{0.016f, 1.0f, 60});

    // Overwrite
    w.SetSingleton(GameTime{0.033f, 2.0f, 120});

    auto* t = w.GetSingleton<GameTime>();
    ASSERT_NE(t, nullptr);
    EXPECT_FLOAT_EQ(t->DeltaTime, 0.033f);
    EXPECT_FLOAT_EQ(t->TotalTime, 2.0f);
    EXPECT_EQ(t->FrameCount, 120u);
}

TEST(SingletonTest, MultipleSingletonTypes) {
    World w(nullptr);
    w.SetSingleton(GameTime{0.016f, 0.0f, 0});
    w.SetSingleton(InputState{true, false, false, true});

    auto* time = w.GetSingleton<GameTime>();
    auto* input = w.GetSingleton<InputState>();

    ASSERT_NE(time, nullptr);
    ASSERT_NE(input, nullptr);
    EXPECT_FLOAT_EQ(time->DeltaTime, 0.016f);
    EXPECT_TRUE(input->KeyW);
    EXPECT_TRUE(input->KeyD);
    EXPECT_FALSE(input->KeyA);
}

TEST(SingletonTest, ConstAccess) {
    World w(nullptr);
    w.SetSingleton(GameTime{0.016f, 5.0f, 300});

    const World& cw = w;
    const auto* t = cw.GetSingleton<GameTime>();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->FrameCount, 300u);
}

// ============================================================================
// OnAdd / OnSet Hook Tests
// ============================================================================

namespace {
    // Counters for hook invocations
    static int g_OnAddCount = 0;
    static int g_OnSetCount = 0;
    static int g_OnRemoveCount = 0;
    static float g_LastAddedX = 0.0f;
    static float g_LastSetX = 0.0f;
    static float g_LastRemovedX = 0.0f;

    // Firing-order recorder for the hook-order parity tests.
    enum HookKind { kHookAdd = 1, kHookSet = 2, kHookRemove = 3 };
    static std::vector<int> g_HookSequence;

    void ResetHookCounters() {
        g_OnAddCount = 0;
        g_OnSetCount = 0;
        g_OnRemoveCount = 0;
        g_LastAddedX = 0.0f;
        g_LastSetX = 0.0f;
        g_LastRemovedX = 0.0f;
        g_HookSequence.clear();
    }

    void OnPositionAdded(Position& p) {
        g_OnAddCount++;
        g_LastAddedX = p.x;
    }

    void OnPositionSet(Position& p) {
        g_OnSetCount++;
        g_LastSetX = p.x;
    }

    void OnPositionRemoved(Position& p) {
        g_OnRemoveCount++;
        g_LastRemovedX = p.x;
    }

    void SeqOnPositionAdded(Position&) { g_HookSequence.push_back(kHookAdd); }
    void SeqOnPositionSet(Position&) { g_HookSequence.push_back(kHookSet); }
    void SeqOnPositionRemoved(Position&) { g_HookSequence.push_back(kHookRemove); }
}

class HookTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetHookCounters();
    }
};

TEST_F(HookTest, OnAddFiresOnFirstComponentAddition) {
    World w(nullptr);
    w.RegisterOnAdd<Position>(OnPositionAdded);

    auto e = w.Create();
    e.Set(Position{42.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    EXPECT_EQ(g_OnAddCount, 1);
    EXPECT_FLOAT_EQ(g_LastAddedX, 42.0f);
}

TEST_F(HookTest, OnAddDoesNotFireOnUpdate) {
    World w(nullptr);
    w.RegisterOnAdd<Position>(OnPositionAdded);

    auto e = w.Create();
    e.Set(Position{1.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    EXPECT_EQ(g_OnAddCount, 1);

    // Update existing component — should NOT fire OnAdd again
    w.AddComponent(e.GetHandle(), Position{2.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    EXPECT_EQ(g_OnAddCount, 1); // Still 1, not 2
}

TEST_F(HookTest, OnSetFiresOnAddAndUpdate) {
    World w(nullptr);
    w.RegisterOnSet<Position>(OnPositionSet);

    auto e = w.Create();
    e.Set(Position{10.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    EXPECT_EQ(g_OnSetCount, 1);
    EXPECT_FLOAT_EQ(g_LastSetX, 10.0f);

    // Update — should fire OnSet again
    w.AddComponent(e.GetHandle(), Position{20.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    EXPECT_EQ(g_OnSetCount, 2);
    EXPECT_FLOAT_EQ(g_LastSetX, 20.0f);
}

TEST_F(HookTest, OnRemoveFiresOnComponentRemoval) {
    World w(nullptr);
    w.RegisterOnRemove<Position>(OnPositionRemoved);

    auto e = w.Create();
    e.Set(Position{1.0f, 2.0f, 3.0f});
    w.ProcessCommands();

    e.Remove<Position>();
    w.ProcessCommands();

    EXPECT_EQ(g_OnRemoveCount, 1);
}

TEST_F(HookTest, AllThreeHooksFire) {
    World w(nullptr);
    w.RegisterOnAdd<Position>(OnPositionAdded);
    w.RegisterOnSet<Position>(OnPositionSet);
    w.RegisterOnRemove<Position>(OnPositionRemoved);

    auto e = w.Create();
    e.Set(Position{5.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    // OnAdd + OnSet both fire on first add
    EXPECT_EQ(g_OnAddCount, 1);
    EXPECT_EQ(g_OnSetCount, 1);
    EXPECT_EQ(g_OnRemoveCount, 0);

    // Update — only OnSet fires
    w.AddComponent(e.GetHandle(), Position{15.0f, 0.0f, 0.0f});
    w.ProcessCommands();

    EXPECT_EQ(g_OnAddCount, 1);
    EXPECT_EQ(g_OnSetCount, 2);

    // Remove — only OnRemove fires
    e.Remove<Position>();
    w.ProcessCommands();

    EXPECT_EQ(g_OnRemoveCount, 1);
}

TEST_F(HookTest, MultipleEntitiesTriggerHooks) {
    World w(nullptr);
    w.RegisterOnAdd<Position>(OnPositionAdded);

    for (int i = 0; i < 10; ++i) {
        auto e = w.Create();
        e.Set(Position{static_cast<float>(i), 0.0f, 0.0f});
    }
    w.ProcessCommands();

    EXPECT_EQ(g_OnAddCount, 10);
    EXPECT_FLOAT_EQ(g_LastAddedX, 9.0f);
}

// ============================================================================
// Structural-body parity tests: every path of an operation class funnels
// through one shared body (DestroyEntityInternal / AddOrSetComponentBytesInternal /
// RemoveComponentInternal). These pin hook firing, firing order, and the
// live-data contract across all paths of each class.
// ============================================================================

TEST_F(HookTest, OnRemoveFiresOnEveryPerEntityDestroyPath) {
    World w(nullptr);
    w.RegisterOnRemove<Position>(OnPositionRemoved);

    // Path 1: DestroyEntityImmediate.
    auto e1 = w.Create();
    e1.Set(Position{1.0f, 0.0f, 0.0f});
    w.ProcessCommands();
    w.DestroyEntityImmediate(e1.GetHandle());
    EXPECT_EQ(g_OnRemoveCount, 1);
    EXPECT_FLOAT_EQ(g_LastRemovedX, 1.0f); // hook saw the live component

    // Path 2: DestroyEntityImmediatePreserveHandle (editor undo/redo shape).
    auto e2 = w.Create();
    e2.Set(Position{2.0f, 0.0f, 0.0f});
    w.ProcessCommands();
    EntityHandle h2 = e2.GetHandle();
    w.DestroyEntityImmediatePreserveHandle(h2);
    EXPECT_EQ(g_OnRemoveCount, 2);
    EXPECT_FLOAT_EQ(g_LastRemovedX, 2.0f);
    EXPECT_FALSE(w.IsValid(h2));
    // The handle stays revivable — the preserve-handle contract.
    EXPECT_TRUE(w.ReviveEntityImmediatePreserveHandle(h2));
    EXPECT_TRUE(w.IsValid(h2));

    // Path 3: deferred DESTROY_ENTITY playback.
    auto e3 = w.Create();
    e3.Set(Position{3.0f, 0.0f, 0.0f});
    w.ProcessCommands();
    w.DestroyEntity(e3.GetHandle());
    EXPECT_EQ(g_OnRemoveCount, 2); // not yet — deferred
    w.ProcessCommands();
    EXPECT_EQ(g_OnRemoveCount, 3);
    EXPECT_FLOAT_EQ(g_LastRemovedX, 3.0f);
}

TEST_F(HookTest, HookOrderParityAcrossTypedAndTypeErasedAdd) {
    World w(nullptr);
    w.RegisterOnAdd<Position>(SeqOnPositionAdded);
    w.RegisterOnSet<Position>(SeqOnPositionSet);

    // Typed structural add: OnAdd fires before OnSet.
    auto e1 = w.CreateEntity();
    w.AddComponentImmediate(e1, Position{1.0f, 0.0f, 0.0f});
    EXPECT_EQ(g_HookSequence, (std::vector<int>{kHookAdd, kHookSet}));

    // Typed data-only update: OnSet only.
    g_HookSequence.clear();
    w.AddComponentImmediate(e1, Position{1.5f, 0.0f, 0.0f});
    EXPECT_EQ(g_HookSequence, (std::vector<int>{kHookSet}));

    // Type-erased structural add: same order as typed.
    g_HookSequence.clear();
    auto e2 = w.CreateEntity();
    const Position p2{2.0f, 0.0f, 0.0f};
    EXPECT_TRUE(w.SetComponentBytesImmediate(e2, GetComponentTypeId<Position>(), &p2, sizeof(p2)));
    EXPECT_EQ(g_HookSequence, (std::vector<int>{kHookAdd, kHookSet}));

    // Type-erased data-only update: OnSet only, same as typed.
    g_HookSequence.clear();
    const Position p2b{2.5f, 0.0f, 0.0f};
    EXPECT_TRUE(w.SetComponentBytesImmediate(e2, GetComponentTypeId<Position>(), &p2b, sizeof(p2b)));
    EXPECT_EQ(g_HookSequence, (std::vector<int>{kHookSet}));

    // Both lanes end with identical component data semantics.
    EXPECT_FLOAT_EQ(w.GetComponent<Position>(e1)->x, 1.5f);
    EXPECT_FLOAT_EQ(w.GetComponent<Position>(e2)->x, 2.5f);
}

TEST_F(HookTest, RemoveParityAcrossTypedAndTypeErased) {
    World w(nullptr);
    w.RegisterOnRemove<Position>(OnPositionRemoved);

    // Typed remove fires OnRemove with the live component value.
    auto e1 = w.CreateEntity();
    w.AddComponentImmediate(e1, Position{7.0f, 0.0f, 0.0f});
    w.RemoveComponentImmediate<Position>(e1);
    EXPECT_EQ(g_OnRemoveCount, 1);
    EXPECT_FLOAT_EQ(g_LastRemovedX, 7.0f);
    EXPECT_FALSE(w.HasComponent<Position>(e1));

    // Type-erased remove: same hook, same live-data contract.
    auto e2 = w.CreateEntity();
    w.AddComponentImmediate(e2, Position{8.0f, 0.0f, 0.0f});
    EXPECT_TRUE(w.RemoveComponentByTypeIdImmediate(e2, GetComponentTypeId<Position>()));
    EXPECT_EQ(g_OnRemoveCount, 2);
    EXPECT_FLOAT_EQ(g_LastRemovedX, 8.0f);
    EXPECT_FALSE(w.HasComponent<Position>(e2));

    // Removing an absent component fires no hook on either lane.
    EXPECT_FALSE(w.RemoveComponentByTypeIdImmediate(e2, GetComponentTypeId<Position>()));
    w.RemoveComponentImmediate<Position>(e1);
    EXPECT_EQ(g_OnRemoveCount, 2);
}

TEST_F(HookTest, OnRemoveSeesLiveDataWhenSwapRemoveClobbersTheSlot) {
    // SwapRemove of a chunk's LAST row leaves its bytes intact, so live-T&
    // assertions on single-entity chunks cannot distinguish hook-before-
    // destruction from hook-after. Here two entities share a chunk and the
    // FIRST row dies: the survivor's bytes are swapped into the vacated slot,
    // so a hook fired after the row teardown would observe the survivor's
    // value instead of the dying entity's. Exercised for all three destroy
    // paths and both remove-component lanes.

    // Path 1: DestroyEntityImmediate.
    {
        World w(nullptr);
        w.RegisterOnRemove<Position>(OnPositionRemoved);
        auto first = w.CreateEntity();
        w.AddComponentImmediate(first, Position{100.0f, 0.0f, 0.0f});
        auto second = w.CreateEntity();
        w.AddComponentImmediate(second, Position{200.0f, 0.0f, 0.0f});

        w.DestroyEntityImmediate(first);
        EXPECT_EQ(g_OnRemoveCount, 1);
        EXPECT_FLOAT_EQ(g_LastRemovedX, 100.0f); // not the swapped-in 200
        ASSERT_NE(w.GetComponent<Position>(second), nullptr);
        EXPECT_FLOAT_EQ(w.GetComponent<Position>(second)->x, 200.0f);
    }

    // Path 2: DestroyEntityImmediatePreserveHandle.
    ResetHookCounters();
    {
        World w(nullptr);
        w.RegisterOnRemove<Position>(OnPositionRemoved);
        auto first = w.CreateEntity();
        w.AddComponentImmediate(first, Position{100.0f, 0.0f, 0.0f});
        auto second = w.CreateEntity();
        w.AddComponentImmediate(second, Position{200.0f, 0.0f, 0.0f});

        w.DestroyEntityImmediatePreserveHandle(first);
        EXPECT_EQ(g_OnRemoveCount, 1);
        EXPECT_FLOAT_EQ(g_LastRemovedX, 100.0f);
        EXPECT_FLOAT_EQ(w.GetComponent<Position>(second)->x, 200.0f);
    }

    // Path 3: deferred DESTROY_ENTITY playback.
    ResetHookCounters();
    {
        World w(nullptr);
        w.RegisterOnRemove<Position>(OnPositionRemoved);
        auto first = w.CreateEntity();
        w.AddComponentImmediate(first, Position{100.0f, 0.0f, 0.0f});
        auto second = w.CreateEntity();
        w.AddComponentImmediate(second, Position{200.0f, 0.0f, 0.0f});

        w.DestroyEntity(first);
        w.ProcessCommands();
        EXPECT_EQ(g_OnRemoveCount, 1);
        EXPECT_FLOAT_EQ(g_LastRemovedX, 100.0f);
        EXPECT_FLOAT_EQ(w.GetComponent<Position>(second)->x, 200.0f);
    }

    // Remove-component lanes: MoveEntity swap-removes the source row, so
    // removing from the first row has the same discrimination.
    ResetHookCounters();
    {
        World w(nullptr);
        w.RegisterOnRemove<Position>(OnPositionRemoved);
        auto first = w.CreateEntity();
        w.AddComponentImmediate(first, Position{100.0f, 0.0f, 0.0f});
        auto second = w.CreateEntity();
        w.AddComponentImmediate(second, Position{200.0f, 0.0f, 0.0f});

        w.RemoveComponentImmediate<Position>(first);
        EXPECT_EQ(g_OnRemoveCount, 1);
        EXPECT_FLOAT_EQ(g_LastRemovedX, 100.0f);
        EXPECT_FLOAT_EQ(w.GetComponent<Position>(second)->x, 200.0f);

        // Type-erased twin, again from a non-last row: after the move above,
        // `second` sits at row 0; append a third row so the removal target is
        // not the last row and the swap genuinely clobbers its slot.
        auto third = w.CreateEntity();
        w.AddComponentImmediate(third, Position{300.0f, 0.0f, 0.0f});
        EXPECT_TRUE(w.RemoveComponentByTypeIdImmediate(second, GetComponentTypeId<Position>()));
        EXPECT_EQ(g_OnRemoveCount, 2);
        EXPECT_FLOAT_EQ(g_LastRemovedX, 200.0f); // not the swapped-in 300
        EXPECT_FLOAT_EQ(w.GetComponent<Position>(third)->x, 300.0f);
    }
}

TEST_F(HookTest, RemoveAbsentComponentVersionSemantics) {
    // Pins the divergent legacy semantics preserved by the unification:
    // the typed remove path bumps the structural version even when the entity
    // lacks the component; the type-erased path does not.
    World w(nullptr);

    auto e = w.CreateEntity();
    w.AddComponentImmediate(e, Velocity{1.0f, 0.0f, 0.0f});

    const std::size_t before = w.GetStructuralChangeVersion();
    EXPECT_FALSE(w.RemoveComponentByTypeIdImmediate(e, GetComponentTypeId<Position>()));
    EXPECT_EQ(w.GetStructuralChangeVersion(), before); // type-erased: no bump

    w.RemoveComponentImmediate<Position>(e);
    EXPECT_EQ(w.GetStructuralChangeVersion(), before + 1); // typed: legacy bump
}

TEST_F(HookTest, DeferredAbsentRemovalAdvancesStructuralVersion)
{
    World world(nullptr);
    world.RegisterOnRemove<Position>(OnPositionRemoved);
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, Velocity{1.0f, 0.0f, 0.0f});
    ASSERT_FALSE(world.HasComponent<Position>(entity));
    const std::size_t before = world.GetStructuralChangeVersion();

    world.RemoveComponent<Position>(entity);
    EXPECT_EQ(world.GetStructuralChangeVersion(), before);
    world.ProcessCommands();

    EXPECT_EQ(world.GetStructuralChangeVersion(), before + 1);
    EXPECT_FALSE(world.HasComponent<Position>(entity));
    EXPECT_TRUE(world.HasComponent<Velocity>(entity));
    EXPECT_EQ(g_OnRemoveCount, 0);
}

// ============================================================================
// ArchetypeTable / ColumnLayout Tests
// ============================================================================

TEST(ColumnLayoutTest, BasicLayout) {
    std::vector<ComponentMeta> metas = {
        {1, sizeof(Position), alignof(Position)},   // 12 bytes
        {2, sizeof(Velocity), alignof(Velocity)},   // 12 bytes
    };
    std::sort(metas.begin(), metas.end(),
              [](const ComponentMeta& a, const ComponentMeta& b) { return a.TypeId < b.TypeId; });

    auto layout = BuildColumnLayout(std::span<const ComponentMeta>(metas));

    EXPECT_GT(layout.Capacity, 0u);
    EXPECT_EQ(layout.Columns.size(), 2u);
    EXPECT_EQ(layout.Columns[0].TypeId, 1u);
    EXPECT_EQ(layout.Columns[1].TypeId, 2u);
    EXPECT_EQ(layout.Columns[0].Size, sizeof(Position));
    EXPECT_EQ(layout.Columns[1].Size, sizeof(Velocity));
    // Total bytes used should fit in a chunk
    EXPECT_LE(layout.TotalBytesUsed, kArchetypeChunkBytes);
}

TEST(ColumnLayoutTest, CapacityReasonable) {
    // Position (12 bytes) + Velocity (12 bytes) + EntityHandle (4 bytes) = 28 bytes per entity
    // 16384 / 28 ≈ 585, minus alignment overhead
    std::vector<ComponentMeta> metas = {
        {1, 12, 4},
        {2, 12, 4},
    };

    auto layout = BuildColumnLayout(std::span<const ComponentMeta>(metas));

    // Should fit hundreds of entities
    EXPECT_GT(layout.Capacity, 100u);
    EXPECT_LT(layout.Capacity, 1000u);
}

TEST(ArchetypeChunkTest, AppendAndSwapRemove) {
    std::vector<ComponentMeta> metas = {{1, sizeof(float), alignof(float)}};
    auto layout = BuildColumnLayout(std::span<const ComponentMeta>(metas));

    // Allocate chunk memory
    std::vector<std::byte> memory(kArchetypeChunkBytes, std::byte{0});
    ArchetypeChunk chunk(memory.data(), &layout);

    EXPECT_TRUE(chunk.IsEmpty());
    EXPECT_EQ(chunk.GetCount(), 0u);

    // Append entities
    EntityHandle e1(1, 1);
    EntityHandle e2(2, 1);
    EntityHandle e3(3, 1);

    uint32_t idx1 = chunk.Append(e1);
    uint32_t idx2 = chunk.Append(e2);
    uint32_t idx3 = chunk.Append(e3);

    EXPECT_EQ(idx1, 0u);
    EXPECT_EQ(idx2, 1u);
    EXPECT_EQ(idx3, 2u);
    EXPECT_EQ(chunk.GetCount(), 3u);

    // Write component data
    float* col = chunk.GetColumn<float>(0);
    col[0] = 10.0f;
    col[1] = 20.0f;
    col[2] = 30.0f;

    // SwapRemove middle element (index 1)
    EntityHandle moved = chunk.SwapRemove(1);
    EXPECT_EQ(moved, e3); // e3 was swapped into index 1
    EXPECT_EQ(chunk.GetCount(), 2u);
    EXPECT_FLOAT_EQ(col[0], 10.0f);  // unchanged
    EXPECT_FLOAT_EQ(col[1], 30.0f);  // e3's data moved here

    // SwapRemove last element (no swap needed)
    EntityHandle movedLast = chunk.SwapRemove(1);
    EXPECT_EQ(movedLast, EntityHandle::Invalid()); // was the last element
    EXPECT_EQ(chunk.GetCount(), 1u);
}

// The single-uint32 ArchetypeChunk::m_Version stub and its tests were deleted
// with the change-signaling P1 mechanism (design M14) — the per-(chunk x
// column) version array in ArchetypeTable subsumes it. See
// Tests/ECS/ChangeFilterP1Test.cpp.

TEST(ArchetypeTableTest, AddAndRemoveEntities) {
    std::vector<ComponentMeta> metas = {
        {1, sizeof(Position), RoundUpPow2(std::min(static_cast<uint32_t>(sizeof(Position)), 16u))},
    };
    auto metaSpan = std::span<const ComponentMeta>(metas);
    ArchetypeTable table{metaSpan};

    EXPECT_EQ(table.GetEntityCount(), 0u);

    auto loc1 = table.AddEntity(EntityHandle(1, 1));
    auto loc2 = table.AddEntity(EntityHandle(2, 1));
    [[maybe_unused]] auto loc3 = table.AddEntity(EntityHandle(3, 1));

    EXPECT_EQ(table.GetEntityCount(), 3u);
    EXPECT_EQ(loc1.ChunkIndex, 0u);
    EXPECT_EQ(loc1.IndexInChunk, 0u);

    // Remove middle entity
    EntityHandle moved = table.RemoveEntity(loc2);
    EXPECT_EQ(table.GetEntityCount(), 2u);
    // Entity 3 was swapped into entity 2's slot
    EXPECT_EQ(moved, EntityHandle(3, 1));
}

TEST(ArchetypeTableTest, ChunkAllocation) {
    // Use a large component to get few entities per chunk
    struct BigComponent { char data[4096]; };
    std::vector<ComponentMeta> metas = {{1, sizeof(BigComponent), 16}};
    auto metaSpan = std::span<const ComponentMeta>(metas);
    ArchetypeTable table{metaSpan};

    auto cap = table.GetLayout().Capacity;
    EXPECT_GT(cap, 0u);

    // Fill first chunk
    for (uint32_t i = 0; i < cap; ++i) {
        table.AddEntity(EntityHandle(i + 1, 1));
    }
    EXPECT_EQ(table.GetChunkCount(), 1u);

    // Adding one more should create a second chunk
    auto loc = table.AddEntity(EntityHandle(cap + 1, 1));
    EXPECT_EQ(table.GetChunkCount(), 2u);
    EXPECT_EQ(loc.ChunkIndex, 1u);
    EXPECT_EQ(loc.IndexInChunk, 0u);
}

// ============================================================================
// CommandData SBO Tests
// ============================================================================

TEST(CommandDataTest, InlineStorage) {
    CommandData cd;
    float value = 42.0f;
    cd.Assign(&value, sizeof(float));

    EXPECT_EQ(cd.Size(), sizeof(float));
    EXPECT_FALSE(cd.Empty());

    float result;
    std::memcpy(&result, cd.Data(), sizeof(float));
    EXPECT_FLOAT_EQ(result, 42.0f);
}

TEST(CommandDataTest, HeapFallbackForLargeData) {
    CommandData cd;
    // 128 bytes > 64 byte inline capacity
    char largeData[128];
    std::memset(largeData, 0xAB, 128);
    cd.Assign(largeData, 128);

    EXPECT_EQ(cd.Size(), 128u);
    EXPECT_EQ(static_cast<const unsigned char*>(static_cast<const void*>(cd.Data()))[0], 0xAB);
    EXPECT_EQ(static_cast<const unsigned char*>(static_cast<const void*>(cd.Data()))[127], 0xAB);
}

TEST(CommandDataTest, MoveSemantics) {
    CommandData cd1;
    float value = 99.0f;
    cd1.Assign(&value, sizeof(float));

    CommandData cd2 = std::move(cd1);
    EXPECT_EQ(cd2.Size(), sizeof(float));
    EXPECT_EQ(cd1.Size(), 0u); // moved-from

    float result;
    std::memcpy(&result, cd2.Data(), sizeof(float));
    EXPECT_FLOAT_EQ(result, 99.0f);
}
