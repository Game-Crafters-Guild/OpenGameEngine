#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Memory/Testing/ScopedAllocationFault.h"
#include "Rendering/Core/GPUScene.h"
#include "TestDeviceHelper.h"
#include <array>
#include <gtest/gtest.h>
#include <stdexcept>
#include <type_traits>

using namespace GameEngine;
using namespace GameEngine::Rendering;

static_assert(!std::is_copy_constructible_v<BucketPoolAllocator>);
static_assert(!std::is_copy_assignable_v<BucketPoolAllocator>);
static_assert(std::is_nothrow_move_constructible_v<BucketPoolAllocator>);
static_assert(std::is_nothrow_move_assignable_v<BucketPoolAllocator>);
static_assert(std::is_nothrow_move_constructible_v<MeshGPUStreamPool>);

TEST(MeshRetirementHandleTest, DestroyProvisionsFifoStorageWhenCreatingHandles)
{
    HandleManager manager;
    std::array<Handle, 64> handles;
    for (auto& handle : handles)
        handle = manager.Create();
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            manager.Destroy(handles[0]);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_FALSE(manager.IsAlive(handles[0]));
    EXPECT_EQ(manager.Size(), 63u);
    const auto replacement = manager.Create();
    EXPECT_EQ(replacement.Index(), handles[0].Index());
    EXPECT_NE(replacement, handles[0]);
}

namespace
{
class MeshRetirementTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "Vulkan device unavailable";
        Registry.Initialize(Device.get());
    }
    void TearDown() override
    {
        Registry.Shutdown();
        if (Scene)
            Scene->Shutdown();
        if (Device)
            Device->Shutdown();
    }
    Mesh Triangle()
    {
        Mesh mesh;
        mesh.Vertices.resize(3);
        mesh.Vertices[1].Position[0] = 1;
        mesh.Vertices[2].Position[1] = 1;
        for (auto& vertex : mesh.Vertices)
            vertex.Normal[2] = 1;
        mesh.Indices = {0, 1, 2};
        return mesh;
    }
    void WithScene()
    {
        Scene = std::make_unique<GPUScene>(Device.get());
        ASSERT_TRUE(Scene->Initialize(16, 16));
        Registry.SetGPUScene(Scene.get());
    }
    std::unique_ptr<IDevice> Device;
    std::unique_ptr<GPUScene> Scene;
    MeshGPURegistry Registry;
};
} // namespace

TEST_F(MeshRetirementTest, LaterStreamAllocationFailureCannotDoubleQueueEarlierRange)
{
    const MeshGPUKey key{GUID::Generate(), 0};
    const auto handle = Registry.RegisterSubmesh(key, Triangle());
    ASSERT_TRUE(handle.IsValid());
    const auto before = Registry.GetPoolStats();
    ASSERT_EQ(before.size(), 2u);
    bool failed = false;
    // Both fresh per-stream DeferredFree vectors allocate one 24-byte record.
    // Baseline queues Core then fails IB16; a correct owner either prepared
    // both or had reserved its retirement capacity when allocating the mesh.
    {
        Memory::Testing::ScopedAllocationFault fault{3 * sizeof(uint64_t), 2, Memory::Testing::FaultRepeat::Once};
        try
        {
            Registry.UnregisterSubmesh(key);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    if (failed)
    {
        EXPECT_NE(Registry.Find(handle), nullptr);
        for (const auto& pool : Registry.GetPoolStats())
            EXPECT_EQ(pool.Allocator.deferredCount, 0u) << pool.Stream;
        EXPECT_TRUE(Registry.UnregisterSubmesh(key));
    }
    EXPECT_EQ(Registry.Find(handle), nullptr);
    for (const auto& pool : Registry.GetPoolStats())
        EXPECT_EQ(pool.Allocator.deferredCount, 1u) << pool.Stream;
    EXPECT_FALSE(Registry.UnregisterSubmesh(key));
    // Do not reclaim a deliberately corrupted baseline queue; the preceding
    // assertions already diagnose that double transfer without a double free.
    for (const auto& pool : Registry.GetPoolStats())
        if (pool.Allocator.deferredCount != 1u)
            return;
    Registry.BeginFrame(16);
    for (const auto& pool : Registry.GetPoolStats())
    {
        EXPECT_EQ(pool.Allocator.used, 0u);
        EXPECT_EQ(pool.Allocator.deferredCount, 0u);
        EXPECT_EQ(pool.Allocator.freeRangeCount, 1u);
        EXPECT_EQ(pool.Allocator.largestFreeRange, pool.Allocator.capacity);
    }
    const auto replacement = Registry.RegisterSubmesh(key, Triangle());
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_NE(replacement, handle);
    EXPECT_EQ(Registry.GetPoolStats().size(), before.size());
    const auto reused = Registry.GetPoolStats();
    for (size_t i = 0; i < before.size(); ++i)
        EXPECT_EQ(reused[i].Allocator.used, before[i].Allocator.used);
}

TEST_F(MeshRetirementTest, ThrowingSubscriberCannotSkipLaterRowInvalidation)
{
    WithScene();
    const MeshGPUKey key{GUID::Generate(), 0};
    const auto handle = Registry.RegisterSubmesh(key, Triangle());
    ASSERT_TRUE(handle.IsValid());
    const auto row = Registry.Find(handle)->gpuMeshIndex;
    unsigned called = 0;
    auto throwing = Registry.SubscribeReload([](const GUID&)
                                             { throw std::runtime_error("injected subscriber"); });
    auto later = Registry.SubscribeReload([&](const GUID& guid)
                                          {
        ++called;
        EXPECT_EQ(guid, key.assetGuid);
        EXPECT_EQ(Registry.Find(handle), nullptr);
        EXPECT_EQ(Scene->GetMeshes()[row].indexCount, 0u); });
    EXPECT_NO_THROW(EXPECT_TRUE(Registry.UnregisterSubmesh(key)));
    EXPECT_EQ(called, 1u);
    EXPECT_EQ(Registry.Find(handle), nullptr);
    EXPECT_FALSE(Registry.UnregisterSubmesh(key));
    EXPECT_EQ(called, 1u);
}

TEST_F(MeshRetirementTest, RemovingMeshRowNeedsNoAllocationAfterAcquisition)
{
    WithScene();
    GPUMesh mesh{};
    mesh.vertexCount = 3;
    mesh.indexCount = 3;
    const auto row = Scene->AddMesh(mesh);
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            Scene->RemoveMesh(row);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_EQ(Scene->GetMeshCount(), 0u);
    EXPECT_EQ(Scene->GetMeshes()[row].indexCount, 0u);
    Scene->RemoveMesh(row);
    EXPECT_EQ(Scene->GetMeshCount(), 0u);
    EXPECT_EQ(Scene->AddMesh(mesh), row);
}

TEST_F(MeshRetirementTest, CompleteUnregisterAndInvalidationNeedNoRetirementAllocation)
{
    WithScene();
    const MeshGPUKey key{GUID::Generate(), 0};
    const auto handle = Registry.RegisterSubmesh(key, Triangle());
    ASSERT_TRUE(handle.IsValid());
    unsigned called = 0;
    auto subscription = Registry.SubscribeReload([&](const GUID&)
                                                 { ++called; });
    bool failed = false;
    bool removed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            removed = Registry.UnregisterSubmesh(key);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_TRUE(removed);
    EXPECT_EQ(called, 1u);
    EXPECT_EQ(Registry.Find(handle), nullptr);
    EXPECT_EQ(Scene->GetMeshCount(), 0u);
}

TEST_F(MeshRetirementTest, InstanceRemovalCannotFailAfterTombstoningBeforeFreeSlotTransfer)
{
    WithScene();
    GPUInstance instance{};
    instance.meshIndex = 0;
    instance.materialIndex = 0;
    instance.boundingRadius = 1;
    const auto removed = Scene->AddInstance(instance);
    const auto survivor = Scene->AddInstance(instance);
    bool failed = false;
    // Baseline allocates its sorted one-row copy, then the free-slot vector.
    // The second failure occurs after writing the tombstone and batch removal.
    {
        Memory::Testing::ScopedAllocationFault fault{0, 2, Memory::Testing::FaultRepeat::Once};
        try
        {
            Scene->RemoveInstance(removed);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_EQ(Scene->GetLiveInstanceCount(), 1u);
    EXPECT_EQ(Scene->GetInstances()[survivor].meshIndex, 0u);
    EXPECT_EQ(Scene->GetInstances()[removed].meshIndex, ~0u);
    Scene->RemoveInstance(removed);
    EXPECT_EQ(Scene->GetLiveInstanceCount(), 1u);
    EXPECT_EQ(Scene->AddInstance(instance), removed);
}

TEST_F(MeshRetirementTest, BulkInstanceRemovalDoesNotLeavePartialFreeSlotOwnership)
{
    WithScene();
    GPUInstance instance{};
    instance.meshIndex = 0;
    instance.materialIndex = 0;
    instance.boundingRadius = 1;
    const auto first = Scene->AddInstance(instance);
    const auto second = Scene->AddInstance(instance);
    const auto survivor = Scene->AddInstance(instance);
    const uint32_t rows[]{first, second};
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 3, Memory::Testing::FaultRepeat::Once};
        try
        {
            Scene->RemoveInstances(rows);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_EQ(Scene->GetLiveInstanceCount(), 1u);
    EXPECT_EQ(Scene->GetInstances()[survivor].meshIndex, 0u);
    Scene->RemoveInstances(rows);
    EXPECT_EQ(Scene->GetLiveInstanceCount(), 1u);
    const auto reusedA = Scene->AddInstance(instance);
    const auto reusedB = Scene->AddInstance(instance);
    EXPECT_NE(reusedA, reusedB);
    EXPECT_TRUE(reusedA == first || reusedA == second);
    EXPECT_TRUE(reusedB == first || reusedB == second);
}

TEST(MeshRetirementAllocatorTest, OutstandingDeferredAndFragmentedRangesRetireWithoutAllocation)
{
    BucketPoolAllocator allocator;
    allocator.Initialize(4096, 16);
    std::array<BucketPoolAllocator::Allocation, 48> ranges;
    for (unsigned i = 0; i < 24; ++i)
        ranges[i] = allocator.Allocate(16);
    for (unsigned i = 0; i < 24; i += 2)
        allocator.FreeDeferred(ranges[i], 2);
    // Outstanding old deferred owners must count when later acquisition grows
    // retirement capacity. Retire out of order to force free-list insertions.
    for (unsigned i = 24; i < ranges.size(); ++i)
        ranges[i] = allocator.Allocate(16);
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            allocator.ReclaimUpToFrame(2);
            for (unsigned i = 1; i < 24; i += 2)
                allocator.Free(ranges[i]);
            for (unsigned i = 24; i < ranges.size(); ++i)
                allocator.FreeDeferred(ranges[i], 3);
            allocator.ReclaimUpToFrame(3);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    ASSERT_FALSE(failed);
    const auto stats = allocator.GetStats();
    EXPECT_EQ(stats.used, 0u);
    EXPECT_EQ(stats.deferredCount, 0u);
    EXPECT_EQ(stats.freeRangeCount, 1u);
    EXPECT_EQ(stats.largestFreeRange, 4096u);
    const auto full = allocator.Allocate(4096);
    EXPECT_EQ(full.offset, 0u);
    EXPECT_EQ(full.size, 4096u);
    allocator.Free(full);
}

TEST(MeshRetirementAllocatorTest, FailedCapacityGrowthDoesNotChangeExistingAllocationOwnership)
{
    BucketPoolAllocator allocator;
    allocator.Initialize(4096, 16);
    std::array<BucketPoolAllocator::Allocation, 64> ranges;
    unsigned acquired = 1;
    ranges[0] = allocator.Allocate(16);
    bool failed = false;
    for (; acquired < ranges.size(); ++acquired)
    {
        const auto before = allocator.GetStats();
        {
            Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
            try
            {
                ranges[acquired] = allocator.Allocate(16);
                failed = !ranges[acquired].IsValid();
            }
            catch (const std::bad_alloc&)
            {
                ADD_FAILURE() << "Metadata refusal must use Allocate's invalid-result contract";
                failed = true;
            }
        }
        if (failed)
        {
            EXPECT_EQ(allocator.GetStats().used, before.used);
            EXPECT_EQ(allocator.GetStats().freeRangeCount, before.freeRangeCount);
            EXPECT_EQ(allocator.GetStats().largestFreeRange, before.largestFreeRange);
            break;
        }
    }
    ASSERT_TRUE(failed);
    // Retry succeeds; existing live ranges remain independently returnable.
    ranges[acquired++] = allocator.Allocate(16);
    for (unsigned i = 0; i < acquired; ++i)
        allocator.Free(ranges[i]);
    EXPECT_EQ(allocator.GetStats().largestFreeRange, 4096u);
    EXPECT_TRUE(allocator.IsEmpty());
}

TEST(MeshRetirementHandleTest, FifoWrapMoveAndResetPreserveGenerationIdentity)
{
    HandleManager manager;
    std::array<Handle, 8> handles;
    for (auto& h : handles)
        h = manager.Create();
    for (unsigned i = 0; i < 6; ++i)
        manager.Destroy(handles[i]);
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto old = handles[i];
        handles[i] = manager.Create();
        EXPECT_EQ(handles[i].Index(), old.Index());
        EXPECT_NE(handles[i], old);
    }
    for (unsigned i = 0; i < 4; ++i)
        manager.Destroy(handles[i]);
    HandleManager moved(std::move(manager));
    EXPECT_EQ(manager.Size(), 0u);
    const unsigned expected[]{4, 5, 0, 1, 2, 3};
    for (auto index : expected)
        EXPECT_EQ(moved.Create().Index(), index);
    EXPECT_EQ(moved.Size(), 8u);
    HandleManager assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.Size(), 8u);
    assigned.Reset();
    EXPECT_EQ(assigned.Size(), 0u);
    EXPECT_EQ(assigned.Create().Index(), 0u);
}

TEST(MeshRetirementHandleTest, FailedGrowthKeepsOldHandlesAliveAndReusable)
{
    HandleManager manager;
    std::array<Handle, 64> handles;
    unsigned count = 0;
    bool failed = false;
    handles[count++] = manager.Create();
    for (; count < handles.size(); ++count)
    {
        {
            Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
            try
            {
                handles[count] = manager.Create();
            }
            catch (const std::bad_alloc&)
            {
                failed = true;
            }
        }
        if (failed)
            break;
    }
    ASSERT_TRUE(failed);
    EXPECT_EQ(manager.Size(), count);
    for (unsigned i = 0; i < count; ++i)
        EXPECT_TRUE(manager.IsAlive(handles[i]));
    handles[count++] = manager.Create();
    for (unsigned i = 0; i < count; ++i)
        manager.Destroy(handles[i]);
    EXPECT_EQ(manager.Size(), 0u);
    for (unsigned i = 0; i < count; ++i)
        EXPECT_EQ(manager.Create().Index(), handles[i].Index());
}

TEST_F(MeshRetirementTest, SubscriberResetDetachesCapturesAndNewListenersWaitForNextEvent)
{
    WithScene();
    const MeshGPUKey first{GUID::Generate(), 0};
    const MeshGPUKey second{GUID::Generate(), 0};
    ASSERT_TRUE(Registry.RegisterSubmesh(first, Triangle()).IsValid());
    ASSERT_TRUE(Registry.RegisterSubmesh(second, Triangle()).IsValid());
    auto selfCapture = std::make_shared<int>(1);
    auto laterCapture = std::make_shared<int>(2);
    std::weak_ptr<int> weakSelf = selfCapture, weakLater = laterCapture;
    ScopedSubscription self, removed, added;
    unsigned oldCalls = 0, newCalls = 0, removedCalls = 0;
    bool aliveDuringCall = false, removedDetached = false;
    self = Registry.SubscribeReload([&, keep = selfCapture](const GUID&)
                                    {
        self.Reset();
        removed.Reset();
        aliveDuringCall = !weakSelf.expired();
        removedDetached = weakLater.expired();
        added = Registry.SubscribeReload([&](const GUID&) { ++newCalls; }); });
    removed = Registry.SubscribeReload([&, keep = laterCapture](const GUID&)
                                       { ++removedCalls; });
    auto original = Registry.SubscribeReload([&](const GUID&)
                                             { ++oldCalls; });
    selfCapture.reset();
    laterCapture.reset();
    ASSERT_TRUE(Registry.UnregisterSubmesh(first));
    EXPECT_TRUE(aliveDuringCall);
    EXPECT_TRUE(removedDetached);
    EXPECT_TRUE(weakSelf.expired());
    EXPECT_EQ(removedCalls, 0u);
    EXPECT_EQ(oldCalls, 1u);
    EXPECT_EQ(newCalls, 0u);
    ASSERT_TRUE(Registry.UnregisterSubmesh(second));
    EXPECT_EQ(oldCalls, 2u);
    EXPECT_EQ(newCalls, 1u);
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            added.Reset();
            original.Reset();
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
}

TEST_F(MeshRetirementTest, ReentrantUnregisterDoesNotLoseOriginalSubscribers)
{
    WithScene();
    const MeshGPUKey first{GUID::Generate(), 0}, nested{GUID::Generate(), 0};
    ASSERT_TRUE(Registry.RegisterSubmesh(first, Triangle()).IsValid());
    ASSERT_TRUE(Registry.RegisterSubmesh(nested, Triangle()).IsValid());
    std::array<GUID, 2> observed{};
    unsigned called = 0;
    auto reentrant = Registry.SubscribeReload([&](const GUID& guid)
                                              {
        if (guid == first.assetGuid) Registry.UnregisterSubmesh(nested); });
    auto later = Registry.SubscribeReload([&](const GUID& guid)
                                          { observed[called++] = guid; });
    EXPECT_TRUE(Registry.UnregisterSubmesh(first));
    EXPECT_EQ(called, 2u);
    EXPECT_EQ(observed[0], nested.assetGuid);
    EXPECT_EQ(observed[1], first.assetGuid);
    EXPECT_EQ(Scene->GetMeshCount(), 0u);
}

TEST_F(MeshRetirementTest, FailedSubscriptionGrowthPreservesExistingListener)
{
    WithScene();
    const MeshGPUKey key{GUID::Generate(), 0};
    ASSERT_TRUE(Registry.RegisterSubmesh(key, Triangle()).IsValid());
    unsigned calls = 0;
    auto existing = Registry.SubscribeReload([&](const GUID&)
                                             { ++calls; });
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            auto unavailable = Registry.SubscribeReload([](const GUID&) {});
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_TRUE(failed);
    EXPECT_TRUE(Registry.UnregisterSubmesh(key));
    EXPECT_EQ(calls, 1u);
}

TEST_F(MeshRetirementTest, DirtyWordsAndBulkFreeSlotsAreProvisionedAcrossFrameSlots)
{
    WithScene();
    GPUInstance instance{};
    instance.meshIndex = 0;
    instance.materialIndex = 0;
    instance.boundingRadius = 1;
    std::array<uint32_t, 130> rows;
    for (auto& row : rows)
        row = Scene->AddInstance(instance);
    // Flush all frame slots so removal must repopulate sparse dirty-word lists.
    for (unsigned i = 0; i < 4; ++i)
    {
        Scene->FlushGPUBuffers();
        Scene->AdvanceFrameSlot();
    }
    const uint32_t removed[]{rows[0], rows[65], rows[128], rows[65]};
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            Scene->RemoveInstances(removed);
        }
        catch (const std::bad_alloc&)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_EQ(Scene->GetLiveInstanceCount(), 127u);
    EXPECT_EQ(Scene->GetInstances()[rows[129]].meshIndex, 0u);
    Scene->FlushGPUBuffers();
}

TEST_F(MeshRetirementTest, SubscriberAndDiagnosticAllocationFailureCannotVetoRetirement)
{
    WithScene();
    const MeshGPUKey key{GUID::Generate(), 0};
    const auto handle = Registry.RegisterSubmesh(key, Triangle());
    ASSERT_TRUE(handle.IsValid());
    unsigned calls = 0;
    auto throwing = Registry.SubscribeReload([](const GUID&)
                                             { throw std::bad_alloc{}; });
    auto later = Registry.SubscribeReload([&](const GUID&)
                                          { ++calls; });
    bool failed = false, removed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            removed = Registry.UnregisterSubmesh(key);
        }
        catch (...)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_TRUE(removed);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(Registry.Find(handle), nullptr);
    EXPECT_EQ(Scene->GetMeshCount(), 0u);
}

TEST(MeshRetirementSubscriptionTest, MovedTokenMayOutliveRegistryWithoutRetainingCallable)
{
    auto registry = std::make_unique<MeshGPURegistry>();
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = capture;
    auto subscription = registry->SubscribeReload([keep = capture](const GUID&) {});
    ScopedSubscription moved = std::move(subscription);
    EXPECT_FALSE(static_cast<bool>(subscription));
    capture.reset();
    EXPECT_FALSE(weak.expired());
    registry.reset();
    EXPECT_TRUE(weak.expired());
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            moved.Reset();
        }
        catch (...)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
}

TEST(MeshRetirementAllocatorTest, MovingLivePoolPreservesPreparedRetirementCapacity)
{
    BucketPoolAllocator source;
    source.Initialize(4096, 16);
    const auto first = source.Allocate(16), second = source.Allocate(16);
    source.FreeDeferred(first, 2);
    BucketPoolAllocator moved(std::move(source));
    BucketPoolAllocator assigned;
    assigned = std::move(moved);
    bool failed = false;
    {
        Memory::Testing::ScopedAllocationFault fault{0, 1, Memory::Testing::FaultRepeat::Once};
        try
        {
            assigned.FreeDeferred(second, 2);
            assigned.ReclaimUpToFrame(2);
        }
        catch (...)
        {
            failed = true;
        }
    }
    EXPECT_FALSE(failed);
    EXPECT_TRUE(assigned.IsEmpty());
    const auto whole = assigned.Allocate(4096);
    EXPECT_EQ(whole.offset, 0u);
    EXPECT_EQ(whole.size, 4096u);
    assigned.Free(whole);
    // Moved-from metadata is unspecified; reset by assignment before re-init.
    source = BucketPoolAllocator{};
    source.Initialize(128, 16);
    EXPECT_EQ(source.Allocate(128).size, 128u);
}

TEST_F(MeshRetirementTest, LaterStreamMetadataFailureReleasesAttemptedVertexGroup)
{
    auto mesh = Triangle();
    for (auto& vertex : mesh.Vertices)
        vertex.Tangent[0] = 1;
    std::array<MeshGPUKey, 9> keys;
    for (auto& key : keys)
        key = {GUID::Generate(), 0};
    for (unsigned i = 0; i < 8; ++i)
        ASSERT_TRUE(Registry.RegisterSubmesh(keys[i], mesh).IsValid());
    uint64_t beforeBytes = 0;
    for (const auto& pool : Registry.GetPoolStats())
        beforeBytes += pool.Allocator.used;
    ASSERT_GT(beforeBytes, 0u);
    MeshGPUHandle last;
    bool escaped = false;
    // The ninth owner grows deferred metadata 8 -> 16 records (384 bytes).
    // Core succeeds; Tangent's reserve fails after Core acquired its range.
    {
        Memory::Testing::ScopedAllocationFault fault{16 * 3 * sizeof(uint64_t), 2, Memory::Testing::FaultRepeat::Once};
        try
        {
            last = Registry.RegisterSubmesh(keys[8], mesh);
        }
        catch (const std::bad_alloc&)
        {
            escaped = true;
        }
        ASSERT_TRUE(fault.Fired());
    }
    EXPECT_FALSE(escaped);
    uint64_t afterBytes = 0;
    for (const auto& pool : Registry.GetPoolStats())
        afterBytes += pool.Allocator.used;
    const auto* entry = Registry.Find(last);
    const bool acquired = entry && entry->coreSubAlloc.IsValid();
    EXPECT_EQ(afterBytes, beforeBytes + (acquired ? beforeBytes / 8u : 0u));
    // Successful sibling-pool fallback is valid; rejected acquisition is too.
    // In either case no unpublished range may remain charged after release.
    for (const auto& key : keys)
        Registry.UnregisterSubmesh(key);
    Registry.BeginFrame(16);
    for (const auto& pool : Registry.GetPoolStats())
    {
        EXPECT_EQ(pool.Allocator.used, 0u) << pool.Stream;
        EXPECT_EQ(pool.Allocator.deferredCount, 0u) << pool.Stream;
        EXPECT_EQ(pool.Allocator.largestFreeRange, pool.Allocator.capacity) << pool.Stream;
    }
}
