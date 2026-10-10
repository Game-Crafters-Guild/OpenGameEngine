#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

using namespace GameEngine::Rendering;

// Window-target ownership of the HDR request.
//
// Creating a window target clears only HdrOutputState::activeMode, so the new
// target is born holding the enabled/requestedMode/bitDepth/staticMetadata of
// whichever target was active at the time. Multi-window hosts must therefore
// evaluate a new window's own display at birth (the editor does this from
// HdrOutputController::OnWindowCreated) — nothing in the device does it for
// them. These tests pin both halves of that contract: the inheritance that
// makes the birth-time evaluation necessary, and the per-target scoping that
// makes it safe to run on one window without disturbing its neighbours.
//
// The two request tests read the *request* rather than activeMode or the
// swapchain format: the request is recorded unconditionally, so they are
// meaningful on an agent whose surface exposes no HDR colour space.
//
// PerTargetSwapchainFormatSurvivesActivationSwitches is the exception and reads
// the format itself, because the format is what the rest of the frame consumes:
// the terminal encode sizes its dither and deband to
// EncodeDitherLsbForFormat(GetSwapchainTextureFormat()), which reads the ACTIVE
// target's format off the device. Two live windows on one device really do hold
// different formats — measured here as R16G16B16A16_FLOAT for the scRGB window
// against RGB10A2_UNORM for its SDR neighbour — because ChooseSwapSurfaceFormat
// picks from m_HdrState, and m_HdrState is per-target state that
// Capture/ApplyWindowTargetState swap in and out on every activation. Those two
// formats want a zero step and a 1/1023 step, so a format that does not follow
// the active target is the wrong quantizer for one of the two windows.

namespace
{
#if defined(HAVE_GLFW)
constexpr float kTargetAPaperWhiteNits = 240.0f;
constexpr float kTargetAMaxMasteringNits = 1200.0f;
constexpr float kTargetBPaperWhiteNits = 120.0f;
constexpr float kTargetBMaxMasteringNits = 600.0f;

HdrStaticMetadata MakeMetadata(float paperWhiteNits, float maxMasteringNits)
{
    HdrStaticMetadata metadata{};
    metadata.paperWhiteNits = paperWhiteNits;
    metadata.maxMasteringLuminance = maxMasteringNits;
    metadata.maxContentLightLevel = maxMasteringNits;
    return metadata;
}
#endif
} // namespace

TEST(WindowTargetHdrRequest, NewTargetInheritsRequestOfActiveTargetAndKeepsItsOwnAfterApply)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* windowA = glfwCreateWindow(320, 200, "HdrRequestTargetA", nullptr, nullptr);
    GLFWwindow* windowB = glfwCreateWindow(256, 160, "HdrRequestTargetB", nullptr, nullptr);
    ASSERT_NE(windowA, nullptr);
    ASSERT_NE(windowB, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc))
    {
        glfwDestroyWindow(windowB);
        glfwDestroyWindow(windowA);
        glfwTerminate();
        GTEST_SKIP() << "No Vulkan device available on this agent";
    }

    WindowTargetHandle targetA{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowA, 320, 200, &targetA));

    const HdrStaticMetadata metadataA = MakeMetadata(kTargetAPaperWhiteNits, kTargetAMaxMasteringNits);
    ASSERT_TRUE(dev->SetHdrOutputMode(HdrOutputMode::HDR10_PQ, &metadataA, HdrSwapchainBitDepth::Bit10));

    // Target B is created while A is active and is never told what to display.
    WindowTargetHandle targetB{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowB, 256, 160, &targetB));

    const HdrOutputState bornState = dev->GetHdrOutputState();
    EXPECT_TRUE(bornState.enabled);
    EXPECT_EQ(bornState.requestedMode, HdrOutputMode::HDR10_PQ);
    EXPECT_FLOAT_EQ(bornState.staticMetadata.paperWhiteNits, kTargetAPaperWhiteNits);
    EXPECT_FLOAT_EQ(bornState.staticMetadata.maxMasteringLuminance, kTargetAMaxMasteringNits);

    // B evaluates its own display and applies the result: B changes, A does not.
    const HdrStaticMetadata metadataB = MakeMetadata(kTargetBPaperWhiteNits, kTargetBMaxMasteringNits);
    ASSERT_TRUE(dev->SetHdrOutputMode(HdrOutputMode::HDR10_PQ, &metadataB, HdrSwapchainBitDepth::Bit10));

    const HdrOutputState appliedB = dev->GetHdrOutputState();
    EXPECT_FLOAT_EQ(appliedB.staticMetadata.paperWhiteNits, kTargetBPaperWhiteNits);
    EXPECT_FLOAT_EQ(appliedB.staticMetadata.maxMasteringLuminance, kTargetBMaxMasteringNits);

    ASSERT_TRUE(dev->SetActiveWindowTarget(targetA));
    const HdrOutputState stateA = dev->GetHdrOutputState();
    EXPECT_TRUE(stateA.enabled);
    EXPECT_FLOAT_EQ(stateA.staticMetadata.paperWhiteNits, kTargetAPaperWhiteNits);
    EXPECT_FLOAT_EQ(stateA.staticMetadata.maxMasteringLuminance, kTargetAMaxMasteringNits);

    ASSERT_TRUE(dev->SetActiveWindowTarget(targetB));
    const HdrOutputState stateBAgain = dev->GetHdrOutputState();
    EXPECT_FLOAT_EQ(stateBAgain.staticMetadata.paperWhiteNits, kTargetBPaperWhiteNits);
    EXPECT_FLOAT_EQ(stateBAgain.staticMetadata.maxMasteringLuminance, kTargetBMaxMasteringNits);

    EXPECT_TRUE(dev->DestroyWindowTarget(targetB));
    EXPECT_TRUE(dev->DestroyWindowTarget(targetA));
    dev.reset();

    glfwDestroyWindow(windowB);
    glfwDestroyWindow(windowA);
    glfwTerminate();
#endif
}

// The swapchain FORMAT is per-target state and must ride the same save/restore
// as the HDR request. Two windows are driven to different formats (A takes
// scRGB/Float16, B is put back to SDR) and then activated back and forth; each
// must report its own format every time it becomes active.
//
// Both halves of the mechanism are gated, and each was confirmed by deleting the
// line it guards and observing this test go red:
//   - CaptureWindowTargetState's `state.swapchainImageFormat = ...` — without it
//     a saved target restores VK_FORMAT_UNDEFINED, so B is born reporting
//     Unknown and A comes back Unknown. Caught with or without divergence.
//   - ApplyWindowTargetState's `m_SwapchainImageFormat = ...` — without it the
//     device keeps whichever format was created last, so switching back to A
//     reports B's. Caught only WITH divergence, which is why the test insists on
//     it rather than settling for equal formats.
//
// The lever this test uses to force divergence is the HDR request, which is the
// only per-window input to ChooseSwapSurfaceFormat a caller can move (the 10-bit
// and UNORM-SDR preferences are fixed for the device lifetime), so it needs a
// surface advertising an HDR colour space. Where that is absent the restore half
// cannot be exercised at all and the run reports SKIPPED rather than passing on
// assertions that could not discriminate — the capture-half assertions above the
// skip still run and still count.
TEST(WindowTargetHdrRequest, PerTargetSwapchainFormatSurvivesActivationSwitches)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* windowA = glfwCreateWindow(320, 200, "SwapFormatTargetA", nullptr, nullptr);
    GLFWwindow* windowB = glfwCreateWindow(256, 160, "SwapFormatTargetB", nullptr, nullptr);
    ASSERT_NE(windowA, nullptr);
    ASSERT_NE(windowB, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc))
    {
        glfwDestroyWindow(windowB);
        glfwDestroyWindow(windowA);
        glfwTerminate();
        GTEST_SKIP() << "No Vulkan device available on this agent";
    }

    WindowTargetHandle targetA{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowA, 320, 200, &targetA));

    // A real swapchain must describe itself: Unknown here would mean the whole
    // test is reading a null instrument, and every comparison below would hold
    // trivially.
    ASSERT_NE(dev->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << "window target A reports no swapchain format; nothing below can discriminate";

    // Drive A somewhere the SDR default does not go. Failure to apply is not a
    // test failure — it is an agent without the colour space — so it is folded
    // into the divergence check rather than asserted.
    dev->SetHdrOutputMode(HdrOutputMode::ScRGB, nullptr, HdrSwapchainBitDepth::Float16);
    const TextureFormat formatA = dev->GetSwapchainTextureFormat();

    // B is created while A is active, so it is born holding A's request and A's
    // format; putting it back to SDR gives it a format of its own.
    WindowTargetHandle targetB{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowB, 256, 160, &targetB));
    EXPECT_NE(dev->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << "target B reports no format at birth — the format was not carried through "
           "the capture/restore that CreateWindowTarget performs";

    ASSERT_TRUE(dev->SetHdrOutputMode(HdrOutputMode::Off, nullptr, HdrSwapchainBitDepth::Bit10));
    const TextureFormat formatB = dev->GetSwapchainTextureFormat();
    ASSERT_NE(formatB, TextureFormat::Unknown);

    const bool diverged = formatA != formatB;

    // Back to A. This is where a device that keeps one global format is caught:
    // the last swapchain built belongs to B.
    ASSERT_TRUE(dev->SetActiveWindowTarget(targetA));
    EXPECT_EQ(dev->GetSwapchainTextureFormat(), formatA)
        << "activating target A reported " << ToString(dev->GetSwapchainTextureFormat())
        << ", expected its own " << ToString(formatA);

    ASSERT_TRUE(dev->SetActiveWindowTarget(targetB));
    EXPECT_EQ(dev->GetSwapchainTextureFormat(), formatB)
        << "activating target B reported " << ToString(dev->GetSwapchainTextureFormat())
        << ", expected its own " << ToString(formatB);

    // A second round trip: the first switch away from a target is also the first
    // time its state is re-captured, so a capture that drops the format shows up
    // here even if the initial store happened to be correct.
    ASSERT_TRUE(dev->SetActiveWindowTarget(targetA));
    EXPECT_EQ(dev->GetSwapchainTextureFormat(), formatA);

    EXPECT_TRUE(dev->DestroyWindowTarget(targetB));
    EXPECT_TRUE(dev->DestroyWindowTarget(targetA));
    dev.reset();

    glfwDestroyWindow(windowB);
    glfwDestroyWindow(windowA);
    glfwTerminate();

    if (!diverged)
    {
        GTEST_SKIP() << "both window targets resolved to " << ToString(formatA)
                     << "; this agent cannot produce two different swapchain formats, so the "
                        "restore-side half of the contract went unexercised";
    }
#endif
}

TEST(WindowTargetHdrRequest, TargetBornWhileHdrOffStaysOff)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* windowA = glfwCreateWindow(320, 200, "HdrOffTargetA", nullptr, nullptr);
    GLFWwindow* windowB = glfwCreateWindow(256, 160, "HdrOffTargetB", nullptr, nullptr);
    ASSERT_NE(windowA, nullptr);
    ASSERT_NE(windowB, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc))
    {
        glfwDestroyWindow(windowB);
        glfwDestroyWindow(windowA);
        glfwTerminate();
        GTEST_SKIP() << "No Vulkan device available on this agent";
    }

    WindowTargetHandle targetA{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowA, 320, 200, &targetA));
    ASSERT_TRUE(dev->SetHdrOutputMode(HdrOutputMode::Off, nullptr, HdrSwapchainBitDepth::Bit10));

    WindowTargetHandle targetB{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowB, 256, 160, &targetB));

    // The single-display case the editor's birth-time evaluation must leave
    // alone: same request in, same request out, no swapchain churn earned.
    const HdrOutputState bornState = dev->GetHdrOutputState();
    EXPECT_FALSE(bornState.enabled);
    EXPECT_EQ(bornState.activeMode, HdrOutputMode::Off);

    EXPECT_TRUE(dev->DestroyWindowTarget(targetB));
    EXPECT_TRUE(dev->DestroyWindowTarget(targetA));
    dev.reset();

    glfwDestroyWindow(windowB);
    glfwDestroyWindow(windowA);
    glfwTerminate();
#endif
}
