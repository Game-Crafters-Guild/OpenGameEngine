// The Metal device's HDR output decision, without a GPU: the startup request
// it takes from DeviceDesc and the state that request resolves to against the
// EDR headroom of the window's screen. The device applies the startup request
// through SetHdrOutputMode, the runtime setter, when its first window target
// becomes active; both resolve through ResolveMetalHdrOutputState.

#include <gtest/gtest.h>

#include "../Source/Metal/MetalHdrOutput.h"

#include <optional>

using namespace GameEngine::Rendering;

namespace
{

constexpr float kPaperWhiteNits = 240.0f;
constexpr float kEdrCurrentHeadroom = 2.0f;
constexpr float kEdrPotentialHeadroom = 4.0f;

DeviceDesc MakeHdrDesc(HdrOutputMode mode)
{
    DeviceDesc desc{};
    desc.hdrEnabled = true;
    desc.hdrMode = mode;
    desc.hdrSwapchainBitDepth = HdrSwapchainBitDepth::Bit10;
    desc.hdrStaticMetadata.paperWhiteNits = kPaperWhiteNits;
    return desc;
}

HdrOutputState ResolveRequest(const MetalHdrOutputRequest& request, const MetalEdrHeadroom& headroom)
{
    return ResolveMetalHdrOutputState(HdrOutputState{}, request.mode, &request.metadata, request.bitDepth, headroom);
}

} // namespace

TEST(MetalHdrOutput, StartupScRGBRequestOnEdrScreenStartsInScRGB)
{
    const std::optional<MetalHdrOutputRequest> request = MetalStartupHdrRequest(MakeHdrDesc(HdrOutputMode::ScRGB));
    ASSERT_TRUE(request.has_value());

    const HdrOutputState state =
        ResolveRequest(*request, MetalEdrHeadroom{kEdrCurrentHeadroom, kEdrPotentialHeadroom, false});

    EXPECT_TRUE(state.enabled);
    EXPECT_EQ(state.requestedMode, HdrOutputMode::ScRGB);
    EXPECT_EQ(state.activeMode, HdrOutputMode::ScRGB);
    EXPECT_EQ(state.swapchainBitDepth, HdrSwapchainBitDepth::Float16);
    EXPECT_EQ(state.staticMetadata.paperWhiteNits, kPaperWhiteNits);
    EXPECT_EQ(GetScRGBFramebufferWhiteNits(state), kPaperWhiteNits);
    EXPECT_TRUE(state.display.hdrAvailable);
    EXPECT_EQ(state.display.outputMaxLinearValue, kEdrCurrentHeadroom);
    EXPECT_EQ(state.display.maxLuminance, kEdrPotentialHeadroom * kPaperWhiteNits);
}

TEST(MetalHdrOutput, StartupScRGBRequestOnSdrScreenMatchesTheRuntimeRequest)
{
    const DeviceDesc desc = MakeHdrDesc(HdrOutputMode::ScRGB);
    const std::optional<MetalHdrOutputRequest> request = MetalStartupHdrRequest(desc);
    ASSERT_TRUE(request.has_value());
    const MetalEdrHeadroom sdrScreen{};

    const HdrOutputState startup = ResolveRequest(*request, sdrScreen);
    const HdrOutputState runtime = ResolveMetalHdrOutputState(HdrOutputState{}, HdrOutputMode::ScRGB,
                                                              &desc.hdrStaticMetadata, desc.hdrSwapchainBitDepth,
                                                              sdrScreen);

    EXPECT_TRUE(startup.enabled);
    EXPECT_EQ(startup.requestedMode, HdrOutputMode::ScRGB);
    EXPECT_EQ(startup.activeMode, HdrOutputMode::Off);
    EXPECT_EQ(startup.swapchainBitDepth, HdrSwapchainBitDepth::Bit10);
    EXPECT_FALSE(startup.display.hdrAvailable);
    EXPECT_EQ(startup.enabled, runtime.enabled);
    EXPECT_EQ(startup.requestedMode, runtime.requestedMode);
    EXPECT_EQ(startup.activeMode, runtime.activeMode);
    EXPECT_EQ(startup.swapchainBitDepth, runtime.swapchainBitDepth);
    EXPECT_EQ(startup.staticMetadata, runtime.staticMetadata);
    EXPECT_EQ(startup.display.hdrAvailable, runtime.display.hdrAvailable);
    EXPECT_EQ(startup.display.resolvedMode, runtime.display.resolvedMode);
}

TEST(MetalHdrOutput, StartupScRGBRequestWithForcedEdrStartsInScRGBOnSdrScreen)
{
    const std::optional<MetalHdrOutputRequest> request = MetalStartupHdrRequest(MakeHdrDesc(HdrOutputMode::ScRGB));
    ASSERT_TRUE(request.has_value());

    const HdrOutputState state = ResolveRequest(*request, MetalEdrHeadroom{1.0f, 1.0f, true});

    EXPECT_TRUE(state.enabled);
    EXPECT_EQ(state.activeMode, HdrOutputMode::ScRGB);
    EXPECT_EQ(state.swapchainBitDepth, HdrSwapchainBitDepth::Float16);
    EXPECT_EQ(state.display.outputMaxLinearValue, 1.0f);
    ASSERT_EQ(state.display.diagnosticHints.size(), 1u);
}

TEST(MetalHdrOutput, SdrLaunchMakesNoStartupRequest)
{
    DeviceDesc disabled = MakeHdrDesc(HdrOutputMode::ScRGB);
    disabled.hdrEnabled = false;
    EXPECT_FALSE(MetalStartupHdrRequest(disabled).has_value());

    EXPECT_FALSE(MetalStartupHdrRequest(MakeHdrDesc(HdrOutputMode::Off)).has_value());
    EXPECT_FALSE(MetalStartupHdrRequest(DeviceDesc{}).has_value());
}
