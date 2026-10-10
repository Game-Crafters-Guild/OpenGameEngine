// The headless device a UI test executable's tests share: created by the first test that
// asks for it, shut down after the last test. Creating a Vulkan device is most of a
// device-backed UI test's cost, and a test that only drives a UIManager asserts nothing
// about the device itself. A test that does (device loss and rebuild, device resource
// counts) creates its own with MakeHeadlessDevice().
//
// Tests run on the gtest main thread one after another, so the lazy creation needs no lock.
// Retirement (SharedHeadlessDevice.h) runs at two points: fixture destruction, for fixtures
// built in a loop inside one test, and every test's end, for the tests that build a
// UIManager as a local of the test body.
#include "SharedHeadlessDevice.h"

#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

namespace
{

std::unique_ptr<GameEngine::Rendering::IDevice> g_SharedDevice;
bool g_SharedDeviceCreated = false;

class SharedHeadlessDeviceEnvironment : public ::testing::Environment
{
  public:
    // Resets the creation latch too: with --gtest_recreate_environments_when_repeating
    // the next iteration creates a fresh device instead of handing every test null.
    void TearDown() override
    {
        if (g_SharedDevice)
            g_SharedDevice->Shutdown();
        g_SharedDevice.reset();
        g_SharedDeviceCreated = false;
    }
};

// Runs after the test's fixture and the test body's locals are destroyed, so everything
// the test built has released its buffers.
class SharedHeadlessDeviceTestEndRetirement : public ::testing::EmptyTestEventListener
{
  public:
    void OnTestEnd(const ::testing::TestInfo&) override { RetireSharedDeviceReleases(); }
};

// GTest takes ownership and runs TearDown() after the last test.
[[maybe_unused]] const ::testing::Environment* const g_SharedHeadlessDeviceEnvironment =
    ::testing::AddGlobalTestEnvironment(new SharedHeadlessDeviceEnvironment());

// GTest takes ownership of the listener.
bool AppendTestEndRetirement()
{
    ::testing::UnitTest::GetInstance()->listeners().Append(new SharedHeadlessDeviceTestEndRetirement());
    return true;
}

[[maybe_unused]] const bool g_TestEndRetirementAppended = AppendTestEndRetirement();

} // namespace

GameEngine::Rendering::IDevice* SharedHeadlessDevice()
{
    if (!g_SharedDeviceCreated)
    {
        g_SharedDeviceCreated = true;
        g_SharedDevice = MakeHeadlessDevice();
    }
    return g_SharedDevice.get();
}

// WaitForIdle is the device's retirement point outside a frame loop: it idles the queues
// and destroys every queued release, whether or not anything was ever submitted.
void RetireSharedDeviceReleases()
{
    if (g_SharedDevice)
        g_SharedDevice->WaitForIdle();
}
