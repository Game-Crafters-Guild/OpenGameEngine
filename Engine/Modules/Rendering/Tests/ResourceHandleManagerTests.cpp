// The global resource managers hand out the pointer they STORE, never a pointer
// INTO their storage.
//
// GenerationalVector::Get() returns an interior pointer into a std::vector that
// Create() resizes. TextureManager / BufferManager / PipelineManager /
// SamplerManager hold m_Mutex only for the duration of the accessor, so returning
// that interior pointer published a reference into storage that a concurrent
// registration could reallocate, and the Vulkan backend dereferenced it after the
// lock was already gone (VulkanHandleHelpers.h). Registration reaches
// IDevice::CreateTexture from ECS extraction workers via
// TextureService::GetOrUpload, concurrently with the resolves in
// CreateTextureView / GetTextureFormat / UpdateImageBinding, so the window was
// reachable in the default configuration rather than theoretical.
//
// These pin three things: the premise (Get() really is an interior pointer that
// Create() invalidates), the API shape (the managers return a value, so the old
// void** cannot come back without breaking the build), and the concurrent
// contract (a resolve under a registration storm always observes exactly the
// pointer that was registered).
//
// Deliberately CPU-only: the managers are header-only containers behind a mutex,
// so no device, no GPU work, and the concurrency arm is a plain thread hammer.

#include "Rendering/Core/BufferManager.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineManager.h"
#include "Rendering/Core/SamplerManager.h"
#include "Rendering/Core/TextureManager.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine::Rendering;

namespace
{
// Distinct, stable addresses to register. The managers store the pointer opaquely
// and never dereference it, so any unique address works as a payload identity.
std::vector<std::uint8_t> MakePayloadStorage(size_t count)
{
    return std::vector<std::uint8_t>(count, 0u);
}

// Enough registrations to force several geometric reallocations of the backing
// vector, which is what invalidates an interior pointer handed out earlier.
constexpr size_t kGrowthRegistrations = 4096;

// A slot retires after Detail::kGenerationMask create/destroy cycles, so four
// whole generation ranges exercise four successive retirements.
constexpr uint32_t kReuseCycles = 4 * static_cast<uint32_t>(Detail::kGenerationMask);

class ResourceHandleManagerTest : public ::testing::Test
{
protected:
    void SetUp() override { ClearAll(); }
    void TearDown() override { ClearAll(); }

private:
    static void ClearAll()
    {
        TextureManager::Instance().Clear();
        BufferManager::Instance().Clear();
        PipelineManager::Instance().Clear();
        SamplerManager::Instance().Clear();
    }
};
} // namespace

// --- Premise -----------------------------------------------------------------

// The defect's mechanism, isolated from the managers: Get() hands back &m_Data[i],
// and a later Create() that grows past capacity moves the storage. Only addresses
// are compared — the stale pointer is never dereferenced.
TEST_F(ResourceHandleManagerTest, GenerationalVectorGetReturnsAnInteriorPointer)
{
    GenerationalVector<void*> storage;
    const Handle first = storage.Create(nullptr);

    void** const before = storage.Get(first);
    ASSERT_NE(before, nullptr);

    for (size_t i = 0; i < kGrowthRegistrations; ++i)
    {
        storage.Create(nullptr);
    }

    void** const after = storage.Get(first);
    ASSERT_NE(after, nullptr);

    EXPECT_NE(before, after)
        << "GenerationalVector::Get() is expected to be an interior pointer; if the "
           "storage no longer moves on growth, the managers' copy-out is redundant "
           "and this test should be revisited rather than deleted.";
}

// --- Shape -------------------------------------------------------------------

// Reintroducing void** anywhere in this family fails the build here.
TEST_F(ResourceHandleManagerTest, ManagersResolveToAValueNotAnInteriorPointer)
{
    static_assert(std::is_same_v<decltype(TextureManager::Instance().GetTexture(TextureHandle{})), void*>,
                  "TextureManager::GetTexture must return the stored pointer by value");
    static_assert(std::is_same_v<decltype(BufferManager::Instance().GetBuffer(BufferHandle{})), void*>,
                  "BufferManager::GetBuffer must return the stored pointer by value");
    static_assert(std::is_same_v<decltype(PipelineManager::Instance().GetPipeline(PipelineHandle{})), void*>,
                  "PipelineManager::GetPipeline must return the stored pointer by value");
    static_assert(std::is_same_v<decltype(SamplerManager::Instance().GetSampler(SamplerHandle{})), void*>,
                  "SamplerManager::GetSampler must return the stored pointer by value");

    static_assert(std::is_same_v<decltype(GetTexture(TextureHandle{})), void*>,
                  "free GetTexture must return the stored pointer by value");
    static_assert(std::is_same_v<decltype(GetBuffer(BufferHandle{})), void*>,
                  "free GetBuffer must return the stored pointer by value");
    static_assert(std::is_same_v<decltype(GetPipeline(PipelineHandle{})), void*>,
                  "free GetPipeline must return the stored pointer by value");
    static_assert(std::is_same_v<decltype(GetSampler(SamplerHandle{})), void*>,
                  "free GetSampler must return the stored pointer by value");

    SUCCEED();
}

// --- Behaviour ---------------------------------------------------------------

// The resolved value is the registered payload, before and after the growth that
// used to move the storage under a published interior pointer.
TEST_F(ResourceHandleManagerTest, ResolvedValueSurvivesReallocatingGrowth)
{
    auto payloads = MakePayloadStorage(kGrowthRegistrations + 1);
    void* const tracked = &payloads[0];

    const TextureHandle texture = CreateTexture(tracked);
    const BufferHandle buffer = CreateBuffer(tracked);
    const PipelineHandle pipeline = CreatePipeline(tracked);
    const SamplerHandle sampler = SamplerManager::Instance().CreateSampler(tracked);

    ASSERT_EQ(GetTexture(texture), tracked);
    ASSERT_EQ(GetBuffer(buffer), tracked);
    ASSERT_EQ(GetPipeline(pipeline), tracked);
    ASSERT_EQ(GetSampler(sampler), tracked);

    for (size_t i = 1; i <= kGrowthRegistrations; ++i)
    {
        void* const filler = &payloads[i];
        CreateTexture(filler);
        CreateBuffer(filler);
        CreatePipeline(filler);
        SamplerManager::Instance().CreateSampler(filler);
    }

    EXPECT_EQ(GetTexture(texture), tracked);
    EXPECT_EQ(GetBuffer(buffer), tracked);
    EXPECT_EQ(GetPipeline(pipeline), tracked);
    EXPECT_EQ(GetSampler(sampler), tracked);
}

TEST_F(ResourceHandleManagerTest, InvalidHandleResolvesToNull)
{
    EXPECT_EQ(GetTexture(INVALID_TEXTURE_HANDLE), nullptr);
    EXPECT_EQ(GetBuffer(INVALID_BUFFER_HANDLE), nullptr);
    EXPECT_EQ(GetPipeline(INVALID_PIPELINE_HANDLE), nullptr);
    EXPECT_EQ(GetSampler(SamplerHandle{}), nullptr);

    EXPECT_FALSE(IsValidTexture(INVALID_TEXTURE_HANDLE));
    EXPECT_FALSE(IsValidBuffer(INVALID_BUFFER_HANDLE));
    EXPECT_FALSE(IsValidPipeline(INVALID_PIPELINE_HANDLE));
}

TEST_F(ResourceHandleManagerTest, DestroyedHandleResolvesToNullAndItsIndexIsReusedWithANewGeneration)
{
    auto payloads = MakePayloadStorage(2);

    const TextureHandle first = CreateTexture(&payloads[0]);
    ASSERT_TRUE(IsValidTexture(first));
    ASSERT_EQ(GetTexture(first), &payloads[0]);

    DestroyTexture(first);
    EXPECT_FALSE(IsValidTexture(first));
    EXPECT_EQ(GetTexture(first), nullptr);

    const TextureHandle second = CreateTexture(&payloads[1]);
    EXPECT_EQ(second.Index(), first.Index());
    EXPECT_NE(second.Generation(), first.Generation());
    EXPECT_EQ(GetTexture(second), &payloads[1]);
    // The stale handle must not resurrect through the recycled slot.
    EXPECT_EQ(GetTexture(first), nullptr);
}

TEST(HandleManagerLifetime, RepeatedReuseNeverReturnsInvalidOrResurrectsAnyStaleHandle)
{
    // Exercise both index zero (whose wrapped generation produced raw ID zero)
    // and a nonzero index (whose wrapped generation resurrected old handles).
    for (const bool keepFirstAlive : {false, true})
    {
        SCOPED_TRACE(keepFirstAlive);
        HandleManager manager;
        const Handle anchor = keepFirstAlive ? manager.Create() : Handle{};
        const size_t baseline = keepFirstAlive ? 1u : 0u;
        std::vector<Handle> stale;
        for (uint32_t cycle = 0; cycle < kReuseCycles; ++cycle)
        {
            SCOPED_TRACE(cycle);
            const Handle current = manager.Create();
            ASSERT_TRUE(current.IsValid());
            ASSERT_TRUE(manager.IsAlive(current));
            EXPECT_NE(current.Generation(), 0u);
            EXPECT_EQ(manager.Size(), baseline + 1u);
            for (const Handle old : stale)
                ASSERT_FALSE(manager.IsAlive(old)) << "stale handle resurrected: " << old.id;

            size_t visited = 0;
            manager.ForEachAlive([&](Handle live) {
                EXPECT_TRUE(live.IsValid());
                EXPECT_TRUE(live == current || (keepFirstAlive && live == anchor));
                ++visited;
            });
            EXPECT_EQ(visited, baseline + 1u);
            if (!stale.empty()) manager.Destroy(stale.back());
            EXPECT_TRUE(manager.IsAlive(current)) << "stale destruction affected the live replacement";
            manager.Destroy(current);
            manager.Destroy(current);
            manager.Destroy(Handle{});
            EXPECT_FALSE(manager.IsAlive(current));
            EXPECT_EQ(manager.Size(), baseline);
            visited = 0;
            manager.ForEachAlive([&](Handle live) {
                EXPECT_EQ(live, anchor);
                ++visited;
            });
            EXPECT_EQ(visited, baseline);
            stale.push_back(current);
        }
        if (keepFirstAlive) manager.Destroy(anchor);
        EXPECT_EQ(manager.Size(), 0u);
    }
}

TEST(HandleManagerLifetime, RetiredSlotsPreserveMoveResetAndAliveIterationAccounting)
{
    // Each slot absorbs kGenerationMask create/destroy cycles before it retires,
    // so this retires exactly three slots and leaves the fourth free.
    constexpr uint32_t kRetiredSlots = 3;
    constexpr uint32_t kCyclesUntilRetired = kRetiredSlots * static_cast<uint32_t>(Detail::kGenerationMask);
    HandleManager source;
    std::vector<Handle> retired;
    for (uint32_t cycle = 0; cycle < kCyclesUntilRetired; ++cycle)
    {
        const Handle handle = source.Create();
        ASSERT_TRUE(handle.IsValid());
        source.Destroy(handle);
        retired.push_back(handle);
    }
    const Handle first = source.Create();
    const Handle second = source.Create();
    const Handle freed = source.Create();
    source.Destroy(freed);

    const auto expectLivePair = [&](const HandleManager& manager) {
        EXPECT_EQ(manager.Size(), 2u);
        std::vector<Handle> alive;
        manager.ForEachAlive([&](Handle handle) { alive.push_back(handle); });
        EXPECT_EQ(alive, (std::vector<Handle>{first, second}));
        for (const Handle old : retired) EXPECT_FALSE(manager.IsAlive(old));
        EXPECT_FALSE(manager.IsAlive(freed));
    };
    const auto expectEmpty = [](const HandleManager& manager) {
        EXPECT_EQ(manager.Size(), 0u);
        size_t count = 0;
        manager.ForEachAlive([&](Handle) { ++count; });
        EXPECT_EQ(count, 0u);
    };

    HandleManager moved(std::move(source));
    expectLivePair(moved);
    expectEmpty(source);
    HandleManager assigned;
    assigned.Create();
    assigned = std::move(moved);
    expectLivePair(assigned);
    expectEmpty(moved);

    const Handle recycled = assigned.Create();
    ASSERT_TRUE(recycled.IsValid());
    EXPECT_EQ(recycled.Index(), freed.Index());
    EXPECT_NE(recycled.Generation(), freed.Generation());
    EXPECT_EQ(assigned.Size(), 3u);
    assigned.Destroy(first);
    assigned.Destroy(first);
    EXPECT_EQ(assigned.Size(), 2u);
    assigned.Reset();
    expectEmpty(assigned);
    EXPECT_FALSE(assigned.IsAlive(first));
    EXPECT_FALSE(assigned.IsAlive(second));
    EXPECT_FALSE(assigned.IsAlive(recycled));
    const Handle fresh = assigned.Create();
    EXPECT_TRUE(fresh.IsValid());
    EXPECT_TRUE(assigned.IsAlive(fresh));
    EXPECT_EQ(assigned.Size(), 1u);
}

TEST_F(ResourceHandleManagerTest, RepeatedBufferReuseKeepsEveryCreatedPayloadReachable)
{
    auto payloads = MakePayloadStorage(kReuseCycles);
    std::vector<BufferHandle> stale;
    for (size_t cycle = 0; cycle < payloads.size(); ++cycle)
    {
        SCOPED_TRACE(cycle);
        const BufferHandle current = CreateBuffer(&payloads[cycle]);
        ASSERT_TRUE(current.IsValid());
        ASSERT_TRUE(IsValidBuffer(current));
        EXPECT_EQ(GetBuffer(current), &payloads[cycle]);
        for (const BufferHandle old : stale) ASSERT_EQ(GetBuffer(old), nullptr);
        DestroyBuffer(current);
        EXPECT_EQ(GetBuffer(current), nullptr);
        stale.push_back(current);
    }
}

TEST(HandleManagerLifetime, RetiredGenerationalVectorSlotsReleaseOwnedPayloads)
{
    struct CountedPayload
    {
        explicit CountedPayload(uint32_t& live) : Live(live) { ++Live; }
        ~CountedPayload() { --Live; }
        uint32_t& Live;
    };
    uint32_t live = 0;
    GenerationalVector<std::unique_ptr<CountedPayload>> storage;
    for (uint32_t cycle = 0; cycle < kReuseCycles; ++cycle)
    {
        SCOPED_TRACE(cycle);
        const Handle current = storage.Create(std::make_unique<CountedPayload>(live));
        ASSERT_TRUE(current.IsValid());
        ASSERT_NE(storage.Get(current), nullptr);
        ASSERT_NE(*storage.Get(current), nullptr);
        ASSERT_EQ(live, 1u) << "an exhausted slot retained its owning payload";
        EXPECT_EQ(storage.Size(), 1u);
        uint32_t visited = 0;
        storage.ForEach([&](Handle h, const auto& value) {
            EXPECT_EQ(h, current);
            EXPECT_NE(value, nullptr);
            ++visited;
        });
        EXPECT_EQ(visited, 1u);
        storage.Destroy(current);
        EXPECT_EQ(storage.Get(current), nullptr);
        EXPECT_EQ(storage.Size(), 0u);
        EXPECT_EQ(live, 0u) << "a destroyed slot retained its owning payload";
    }
    storage.Clear();
    EXPECT_EQ(live, 0u);
}

// --- Concurrent contract -----------------------------------------------------

// Registration on one thread, resolves on others: the exact shape that reaches
// TextureManager from the ECS extraction wave. Every resolve must observe the
// pointer that was registered for that handle. With the accessor returning an
// interior pointer this ran the reader into a vector the writer was reallocating;
// with a value copied out under the lock it is well-defined.
TEST_F(ResourceHandleManagerTest, ConcurrentRegistrationDoesNotDisturbResolves)
{
    constexpr size_t kTrackedCount = 64;
    constexpr size_t kReaderThreads = 4;
    constexpr size_t kResolvesPerReader = 20000;

    auto payloads = MakePayloadStorage(kTrackedCount + kGrowthRegistrations);

    std::vector<TextureHandle> tracked;
    tracked.reserve(kTrackedCount);
    for (size_t i = 0; i < kTrackedCount; ++i)
    {
        tracked.push_back(CreateTexture(&payloads[i]));
        ASSERT_TRUE(tracked.back().IsValid());
    }

    std::atomic<bool> start{false};
    std::atomic<bool> writerDone{false};
    std::atomic<uint64_t> mismatches{0};
    std::atomic<uint64_t> nulls{0};

    // Writer: grows the backing vector continuously for the whole run.
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        for (size_t i = 0; i < kGrowthRegistrations; ++i)
        {
            CreateTexture(&payloads[kTrackedCount + i]);
        }
        writerDone.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (size_t t = 0; t < kReaderThreads; ++t)
    {
        readers.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
            for (size_t n = 0; n < kResolvesPerReader; ++n)
            {
                const size_t idx = (n + t) % kTrackedCount;
                // Resolve through the same free function the Vulkan backend uses.
                void* const resolved = GetTexture(tracked[idx]);
                if (resolved == nullptr)
                {
                    nulls.fetch_add(1, std::memory_order_relaxed);
                }
                else if (resolved != &payloads[idx])
                {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& r : readers) { r.join(); }

    EXPECT_TRUE(writerDone.load(std::memory_order_acquire));
    EXPECT_EQ(mismatches.load(), 0u) << "a resolve observed a payload other than the one registered";
    EXPECT_EQ(nulls.load(), 0u) << "a live handle resolved to null while another thread was registering";

    // The tracked handles are still intact after the storm.
    for (size_t i = 0; i < kTrackedCount; ++i)
    {
        ASSERT_EQ(GetTexture(tracked[i]), &payloads[i]);
    }
}
