// The editor constructs its PolyhavenDownloadManager before the engine initializes
// (EditorApplication's constructor), so the manager creates its "Polyhaven" channel on the
// first transfer, never at construction, and never once Shutdown has run.

#include <gtest/gtest.h>

#include "Assets/PolyhavenDownloadManager.h"
#include "Core/Engine.h"

// The engine is never initialized in this process, so the engine's job system does not
// exist: a channel created here would register with no pool and crash.
TEST(PolyhavenDownloadManager, ConstructionAndShutdownCreateNoChannel)
{
    ASSERT_FALSE(GameEngine::EngineCore::GetInstance().IsInitialized())
        << "This suite needs a process whose engine never initializes; the test proves nothing otherwise";

    GameEngine::PolyhavenDownloadManager manager;
    manager.Shutdown();
    EXPECT_EQ(manager.Transfers(), nullptr) << "Transfers() created a channel after Shutdown";
}
