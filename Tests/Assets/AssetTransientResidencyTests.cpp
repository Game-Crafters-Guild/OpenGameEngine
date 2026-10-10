#include <gtest/gtest.h>
#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace GameEngine;

// AssetResidency::Transient: an asset loaded only for one piece of work (a
// thumbnail bake) leaves memory on ReleaseTransientAsset, unless anything else
// reached it in the meantime.
class AssetTransientResidencyTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = TestUtils::MakeUniqueTempDirectory("asset_transient_residency");
        std::filesystem::create_directories(m_Dir);
        m_ModelPath = m_Dir / "triangle.obj";
        std::ofstream(m_ModelPath) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

        m_JobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(4);
        m_Assets = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_Assets->Initialize(m_Dir, m_JobSystem.get()));
        m_Assets->WaitForStartupScan();
        m_Guid = m_Assets->GetRegistry().GetOrCreateAssetGUID(m_ModelPath);
        ASSERT_FALSE(m_Guid.IsNull());

        m_UnloadedHandle = m_Assets->GetEventDispatcher().AddCallback(
            [this](const AssetEvent& event)
            {
                if (event.EventType == AssetEventType::AssetUnloaded && event.AssetGuid == m_Guid)
                    m_UnloadedEvents.fetch_add(1);
            });
    }

    void TearDown() override
    {
        if (m_Assets)
        {
            m_Assets->GetEventDispatcher().RemoveCallback(m_UnloadedHandle);
            m_Assets->Shutdown();
            m_Assets.reset();
        }
        if (m_JobSystem)
        {
            m_JobSystem->Shutdown();
            m_JobSystem.reset();
        }
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    AssetLoadHandle Load(AssetResidency residency)
    {
        AssetLoadRequest request(m_Guid, AssetLoadResultCallback{});
        request.Residency = residency;
        return m_Assets->LoadAsset(request);
    }

    static bool WaitForLoad(const AssetLoadHandle& handle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!handle.IsComplete())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return handle.GetResult() != nullptr;
    }

    std::filesystem::path m_Dir;
    std::filesystem::path m_ModelPath;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    std::unique_ptr<AssetManager> m_Assets;
    GUID m_Guid;
    uint32 m_UnloadedHandle = 0;
    std::atomic<int> m_UnloadedEvents{0};
};

TEST_F(AssetTransientResidencyTest, TransientLoadLeavesMemoryWhenNothingElseReachedIt)
{
    {
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(lease));
    }
    ASSERT_TRUE(m_Assets->IsAssetLoaded(m_Guid));

    EXPECT_TRUE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_FALSE(m_Assets->IsAssetLoaded(m_Guid));
    // The unload is announced, so caches that mirror the asset drop their copies.
    EXPECT_EQ(m_UnloadedEvents.load(), 1);
    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
}

TEST_F(AssetTransientResidencyTest, LoadAssetAsyncPinsATransientAsset)
{
    {
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(lease));
    }
    ASSERT_NE(m_Assets->LoadAssetAsync(m_Guid).get(), nullptr);

    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_TRUE(m_Assets->IsAssetLoaded(m_Guid));
}

TEST_F(AssetTransientResidencyTest, GetAssetPinsATransientAsset)
{
    {
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(lease));
    }
    ASSERT_NE(m_Assets->GetAsset(m_Guid), nullptr);

    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_TRUE(m_Assets->IsAssetLoaded(m_Guid));
    EXPECT_EQ(m_UnloadedEvents.load(), 0);
}

TEST_F(AssetTransientResidencyTest, PinnedLoadPinsATransientAsset)
{
    {
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(lease));
        AssetLoadHandle pinned = Load(AssetResidency::Pinned);
        ASSERT_TRUE(WaitForLoad(pinned));
    }

    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_TRUE(m_Assets->IsAssetLoaded(m_Guid));
}

TEST_F(AssetTransientResidencyTest, PinnedRequestDuringATransientLoadPinsIt)
{
    // The pinned request either joins the transient load in flight or finds it
    // resident; both reach the asset, so both pin it.
    {
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        AssetLoadHandle pinned = Load(AssetResidency::Pinned);
        ASSERT_TRUE(WaitForLoad(lease));
        ASSERT_TRUE(WaitForLoad(pinned));
    }

    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_TRUE(m_Assets->IsAssetLoaded(m_Guid));
}

TEST_F(AssetTransientResidencyTest, TransientRequestDoesNotUnpinAPinnedAsset)
{
    {
        AssetLoadHandle pinned = Load(AssetResidency::Pinned);
        ASSERT_TRUE(WaitForLoad(pinned));
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(lease));
    }

    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_TRUE(m_Assets->IsAssetLoaded(m_Guid));
}

TEST_F(AssetTransientResidencyTest, ReleasedAssetLoadsAgainOnTheNextRequest)
{
    {
        AssetLoadHandle lease = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(lease));
    }
    ASSERT_TRUE(m_Assets->ReleaseTransientAsset(m_Guid));

    // A later consumer (a scene spawn) needs nothing special: it loads the
    // asset again, pinned.
    AssetLoadHandle pinned = Load(AssetResidency::Pinned);
    ASSERT_TRUE(WaitForLoad(pinned));
    EXPECT_TRUE(pinned.GetResult()->IsLoaded());
    pinned = {};
    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
}

// Either the pinned lookup wins (eviction is refused), or eviction wins and
// the request loads a new resident asset. A successful request must never
// return the old, unloaded payload. Repeat with --gtest_repeat for stress.
TEST_F(AssetTransientResidencyTest, CachedPinnedRequestRacesTransientRelease)
{
    auto transient = Load(AssetResidency::Transient);
    ASSERT_TRUE(WaitForLoad(transient));
    std::atomic<bool> start{false};
    std::thread release([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        m_Assets->ReleaseTransientAsset(m_Guid);
    });
    start.store(true, std::memory_order_release);
    auto pinned = Load(AssetResidency::Pinned);
    release.join();
    ASSERT_TRUE(WaitForLoad(pinned));
    EXPECT_TRUE(pinned.GetResult()->IsLoaded());
    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
}

TEST_F(AssetTransientResidencyTest, CachedAsyncRequestRacesTransientRelease)
{
    auto transient = Load(AssetResidency::Transient);
    ASSERT_TRUE(WaitForLoad(transient));
    std::atomic<bool> start{false};
    std::thread release([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        m_Assets->ReleaseTransientAsset(m_Guid);
    });
    start.store(true, std::memory_order_release);
    auto pinned = m_Assets->LoadAssetAsync(m_Guid);
    release.join();
    auto asset = pinned.get();
    ASSERT_NE(asset, nullptr);
    EXPECT_TRUE(asset->IsLoaded());
    EXPECT_FALSE(m_Assets->ReleaseTransientAsset(m_Guid));
}

TEST_F(AssetTransientResidencyTest, ResidencyBelongsToEachLoadedGeneration)
{
    {
        auto pinned = Load(AssetResidency::Pinned);
        ASSERT_TRUE(WaitForLoad(pinned));
    }
    m_Assets->UnregisterLoadedAsset(m_Guid);
    ASSERT_FALSE(m_Assets->IsAssetLoaded(m_Guid));
    {
        auto transient = Load(AssetResidency::Transient);
        ASSERT_TRUE(WaitForLoad(transient));
    }
    EXPECT_TRUE(m_Assets->ReleaseTransientAsset(m_Guid));
    EXPECT_FALSE(m_Assets->IsAssetLoaded(m_Guid));
}
