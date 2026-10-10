// An in-place device rebuild must not strand VkSemaphores on the device it builds.
//
// Vulkan requires every child object to be destroyed before vkDestroyDevice, so
// the property under test is per-device-generation parity: of every semaphore a
// VkDevice created, none outlives it. VulkanDevice keeps that ledger and records
// the residual as each generation closes.
//
// Two things make this suite look heavier than the property it pins:
//
//   1. It needs a REAL window target. RebuildDevice's swapchain-recreation step
//      returns immediately when no target exists, so the code path that can
//      strand a handle is unreachable on a headless device — which is why the
//      63 tests in DeviceRecoveryTests cannot see this. A hidden GLFW window is
//      the same offscreen-surface pattern WindowTargetHdrRequestTests uses.
//
//   2. It needs TWO rebuilds. A stranded handle is invisible until the device
//      that owns it is destroyed, and that happens at the START of the NEXT
//      rebuild. One rebuild therefore only reports on the device built by
//      Initialize, which is clean either way; the generation built by rebuild #1
//      is the first one whose teardown can show the defect.
//
// The first residual read is a CONTROL, not a formality: it comes from a device
// generation that never went through the rebuild path, so a non-zero there means
// some unrelated owner is stranding semaphores and every later reading is
// unattributable. The suite says so rather than reporting the leak it came for.

#include "Source/Vulkan/VulkanDevice.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Tests/ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <vector>

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

using namespace GameEngine::Rendering;

namespace
{
#if defined(HAVE_GLFW)
constexpr uint32_t kWindowWidth = 320;
constexpr uint32_t kWindowHeight = 200;
// Frame ordinals climb monotonically across rebuilds and are 1-based. Two losses,
// spaced so each lands well after the previous recovery has produced frames.
constexpr const char* kTwoLossSchedule = "3,12";
// Generous relative to the 3-frame spacing above; a rebuild completes within one
// tick of the loss latching, so this only bounds a pathological agent.
constexpr int kMaxFramesPerRecovery = 60;

using GameEngine::Rendering::Tests::ScopedEnvVar;

// Mirrors EditorApplication::Render: the rebuild retry is driven by the
// unconditional per-tick TickDeviceRecovery, not by BeginFrame.
void RunOneFrame(IDevice& dev)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
    {
        return;
    }
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

// Drives frames until the injected loss has rebuilt the device, then stands in for
// the re-provision consumer. Returns false if no rebuild happened in the budget.
bool DriveOneRecoveryCycle(IDevice& dev)
{
    for (int i = 0; i < kMaxFramesPerRecovery; ++i)
    {
        RunOneFrame(dev);
        if (dev.GetDeviceHealth() == DeviceHealth::AwaitingReprovision)
        {
            dev.NotifyReprovisionComplete();
            return true;
        }
        if (dev.GetDeviceHealth() == DeviceHealth::Failed)
        {
            return false;
        }
    }
    return false;
}
// Two rebuild cycles over `windowCount` hidden window targets, asserting that every
// device generation closed along the way took all of its semaphores with it.
void RunTwoRebuildCyclesAndAssertLedgerParity(int windowCount)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", kTwoLossSchedule);
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    std::vector<GLFWwindow*> windows;
    for (int i = 0; i < windowCount; ++i)
    {
        GLFWwindow* w = glfwCreateWindow((int)kWindowWidth, (int)kWindowHeight, "RebuildSemaphoreLedger", nullptr, nullptr);
        ASSERT_NE(w, nullptr);
        windows.push_back(w);
    }
    auto destroyWindows = [&]()
    {
        for (GLFWwindow* w : windows)
        {
            glfwDestroyWindow(w);
        }
        glfwTerminate();
    };

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc))
    {
        destroyWindows();
        GTEST_SKIP() << "No Vulkan device available on this agent";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    for (GLFWwindow* w : windows)
    {
        WindowTargetHandle target{};
        ASSERT_TRUE(dev->CreateAndActivateWindowTarget(w, kWindowWidth, kWindowHeight, &target));
    }
    // Precondition, not decoration: with no swapchain the rebuild path this suite
    // exists to cover returns before it can strand anything.
    ASSERT_NE(dev->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << "no swapchain on the window target — the rebuild path under test is unreachable";

    // --- Rebuild #1. Closes the generation built by Initialize. --------------
    ASSERT_TRUE(DriveOneRecoveryCycle(*dev)) << "the first injected loss did not rebuild the device";
    ASSERT_NE(dev->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << "the window target's swapchain was not recreated, so the rebuild never ran "
           "the step that can strand a semaphore";
    ASSERT_EQ(vk->GetLastDeviceGenerationSemaphoreResidual(), 0u)
        << "CONTROL FAILED: the device built by Initialize stranded semaphores of its own. "
           "Something other than the rebuild path is leaking; the reading below cannot be "
           "attributed until that is explained.";

    // --- Rebuild #2. Closes the generation built by rebuild #1 — the one whose
    //     window-target recreation could have orphaned its present-ready set. ---
    ASSERT_TRUE(DriveOneRecoveryCycle(*dev)) << "the second injected loss did not rebuild the device";
    ASSERT_NE(dev->GetSwapchainTextureFormat(), TextureFormat::Unknown);
    EXPECT_EQ(vk->GetLastDeviceGenerationSemaphoreResidual(), 0u)
        << "a rebuilt device was destroyed with semaphores still alive on it";

    // Shutdown closes the generation built by rebuild #2 the same way.
    dev->Shutdown();
    EXPECT_EQ(vk->GetLastDeviceGenerationSemaphoreResidual(), 0u)
        << "the final device generation was destroyed with semaphores still alive on it";

    dev.reset();
    destroyWindows();
}
#endif // HAVE_GLFW
} // namespace

TEST(DeviceRebuildSemaphoreLedger, RebuildWithAWindowTargetStrandsNoSemaphores)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    RunTwoRebuildCyclesAndAssertLedgerParity(/*windowCount=*/1);
#endif
}

// A rebuild folds the active target back into m_WindowTargets and walks every
// entry, so a second window doubles the population that teardown has to account
// for. Nothing else covers a multi-window rebuild, and the same ledger answers
// the question directly: if any non-active target's objects were being dropped,
// the residual here would exceed the single-window residual by that target's
// per-image semaphore count.
TEST(DeviceRebuildSemaphoreLedger, RebuildWithTwoWindowTargetsStrandsNoSemaphores)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    RunTwoRebuildCyclesAndAssertLedgerParity(/*windowCount=*/2);
#endif
}

// Declared after both injectors, and gtest runs a suite's tests in declaration
// order: the forced loss must not outlive the test that asked for it, or every
// later device-creating test in this process runs under an injection it never
// requested — and this is the suite that attributes semaphore residuals.
TEST(DeviceRebuildSemaphoreLedger, ForcedDeviceLossDoesNotLeakToLaterTests)
{
    const char* leaked = std::getenv("GE_VK_FORCE_DEVICE_LOST");
    EXPECT_EQ(leaked, nullptr)
        << "GE_VK_FORCE_DEVICE_LOST outlived its test with value '" << (leaked ? leaked : "")
        << "' — later device-creating tests in this process would run under an injected loss "
           "they never asked for";
}
