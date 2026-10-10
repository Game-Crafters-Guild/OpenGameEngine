#include <gtest/gtest.h>

#include "Platform/Capabilities.h"
#include "Platform/Thread.h"

using namespace GameEngine;

// A desktop host lets the application pump its own message loop, so a long
// frame never blocks the input that would end it. The editor keeps the world
// path running behind the boot project picker on exactly this answer; only a
// host that owns the frame loop (the browser's requestAnimationFrame) reports
// true and has the world parked while the picker is up.
TEST(PlatformCapabilities, DesktopHostDoesNotDriveTheFrameLoop)
{
    EXPECT_FALSE(Platform::HostDrivesFrameLoop());
}

TEST(PlatformCapabilities, DesktopThreadsCanWaitForWorkers)
{
    EXPECT_TRUE(Platform::CanBlockCurrentThread());
}

TEST(PlatformCapabilities, DesktopLibrariesCanKeepTheirOwnWorkerPools)
{
    EXPECT_TRUE(Platform::SupportsAuxiliaryThreadPools());
}

// A desktop editor runs its channels' blocking jobs (builds, compiles) on up to
// 64 blocking threads; the threaded web build has room for one beside its
// compute workers in the pre-spawned pthread pool.
TEST(PlatformCapabilities, DesktopBlockingThreadBudgetIs64)
{
    EXPECT_EQ(Platform::BlockingThreadBudget(), 64u);
}
