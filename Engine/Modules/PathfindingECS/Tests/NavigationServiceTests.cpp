#include "PathfindingECS/NavigationService.h"
#include "Types/Types.h"

#include <gtest/gtest.h>

using namespace GameEngine;              // for uint64 etc.
using namespace GameEngine::PathfindingECS;

class NavigationServiceTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        if (NavigationService::IsInitialized())
            NavigationService::Shutdown();
    }
};

TEST_F(NavigationServiceTest, InitializeCreatesWorld)
{
    EXPECT_FALSE(NavigationService::IsInitialized());
    NavigationService::Initialize();
    EXPECT_TRUE(NavigationService::IsInitialized());
}

TEST_F(NavigationServiceTest, TryGetReturnsNonNullAfterInitialize)
{
    EXPECT_EQ(NavigationService::TryGet(), nullptr);
    NavigationService::Initialize();
    EXPECT_NE(NavigationService::TryGet(), nullptr);
}

TEST_F(NavigationServiceTest, ShutdownClearsWorld)
{
    NavigationService::Initialize();
    ASSERT_TRUE(NavigationService::IsInitialized());

    NavigationService::Shutdown();
    EXPECT_FALSE(NavigationService::IsInitialized());
    EXPECT_EQ(NavigationService::TryGet(), nullptr);
}

TEST_F(NavigationServiceTest, GenerationIncrementsOnInitializeAndShutdown)
{
    uint64 genBefore = NavigationService::GetGeneration();

    NavigationService::Initialize();
    uint64 genAfterInit = NavigationService::GetGeneration();
    EXPECT_GT(genAfterInit, genBefore);

    NavigationService::Shutdown();
    uint64 genAfterShutdown = NavigationService::GetGeneration();
    EXPECT_GT(genAfterShutdown, genAfterInit);
}

TEST_F(NavigationServiceTest, GetDebugDataReturnsWritableReference)
{
    NavigationService::Initialize();

    NavDebugData& debugData = NavigationService::GetDebugData();
    EXPECT_TRUE(debugData.Lines.empty());
    EXPECT_TRUE(debugData.Triangles.empty());

    // Verify we can write to it
    debugData.Lines.push_back({0, 0, 0, 1, 1, 1, 1, 0, 0, 1});
    EXPECT_EQ(NavigationService::GetDebugData().Lines.size(), 1u);

    // Shutdown discards the instance (and its debug data). A subsequent
    // Initialize creates a fresh instance with empty debug data — which
    // is how callers observe the "Shutdown cleared it" contract. (GetDebugData
    // requires IsInitialized, mirroring Get().)
    NavigationService::Shutdown();
    NavigationService::Initialize();
    EXPECT_TRUE(NavigationService::GetDebugData().Lines.empty());
    EXPECT_TRUE(NavigationService::GetDebugData().Triangles.empty());
}

TEST_F(NavigationServiceTest, DuplicateInitializeIsIgnored)
{
    NavigationService::Initialize();
    uint64 genAfterFirst = NavigationService::GetGeneration();
    auto* worldPtr = NavigationService::TryGet();

    NavigationService::Initialize(); // duplicate
    EXPECT_EQ(NavigationService::GetGeneration(), genAfterFirst);
    EXPECT_EQ(NavigationService::TryGet(), worldPtr);
}

TEST_F(NavigationServiceTest, ShutdownWithoutInitializeIsIgnored)
{
    uint64 genBefore = NavigationService::GetGeneration();
    NavigationService::Shutdown(); // no-op
    EXPECT_EQ(NavigationService::GetGeneration(), genBefore);
}
