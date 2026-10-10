// HdrOutputMatchTests — the guard that keeps a settings-driven HDR re-apply
// free when nothing changed.
//
// Opening a project re-applies the incoming project's rendering.hdr.* settings,
// and an HDR apply costs a device idle, a 1.5 s render-suppression window with
// 30 skipped frames, and a world re-record (plus a swapchain recreate on some
// backends). So every project open would stall unless this predicate reports
// "already satisfied" for an unchanged request. These tests lock that.
//
// The trap they lock hardest: HdrOutputState carries BOTH the accepted request
// (enabled/requestedMode) and what the surface resolved to (activeMode). A
// display with no HDR colorspace resolves activeMode to Off while still holding
// the HDR request, so a predicate keyed on activeMode reports "unsatisfied"
// forever and re-applies on every call — on exactly the machines where HDR does
// nothing. FallenBackToSdrIsStillSatisfied is that case, and
// OffRequestAgainstStaleHdrRequestIsNotSatisfied is the same device state asked
// for Off, where the stale request does have to be cleared.

#include "Display/HdrOutputMatch.h"

#include <gtest/gtest.h>

namespace ed = GameEngine::Editor;
namespace gr = GameEngine::Rendering;

namespace
{

// A live state that has accepted an HDR10_PQ request and resolved it on a
// capable display.
gr::HdrOutputState LiveHdr10(gr::HdrSwapchainBitDepth bitDepth = gr::HdrSwapchainBitDepth::Bit10)
{
    gr::HdrOutputState state{};
    state.enabled = true;
    state.requestedMode = gr::HdrOutputMode::HDR10_PQ;
    state.activeMode = gr::HdrOutputMode::HDR10_PQ;
    state.swapchainBitDepth = bitDepth;
    return state;
}

gr::HdrOutputState LiveOff()
{
    gr::HdrOutputState state{};
    state.enabled = false;
    state.requestedMode = gr::HdrOutputMode::Off;
    state.activeMode = gr::HdrOutputMode::Off;
    return state;
}

// An accepted Auto request, resolved to scRGB. Auto reaches the device
// unresolved (ApplyForMonitor passes hdrRequest.mode straight through), and
// ChooseSwapSurfaceFormat tries PQ, then HLG, then scRGB for it — so a surface
// exposing only scRGB yields requestedMode=Auto with activeMode=ScRGB. The
// device records the request, never the resolution.
gr::HdrOutputState LiveAuto()
{
    gr::HdrOutputState state{};
    state.enabled = true;
    state.requestedMode = gr::HdrOutputMode::Auto;
    state.activeMode = gr::HdrOutputMode::ScRGB;
    state.swapchainBitDepth = gr::HdrSwapchainBitDepth::Bit10;
    return state;
}

bool Satisfies(const gr::HdrOutputState& live, gr::HdrOutputMode mode,
               gr::HdrSwapchainBitDepth bitDepth = gr::HdrSwapchainBitDepth::Bit10,
               const gr::HdrStaticMetadata& metadata = gr::HdrStaticMetadata{})
{
    return ed::HdrOutputStateSatisfiesRequest(live, mode, bitDepth, metadata);
}

// ---------------------------------------------------------------------------
// The no-op cases: re-applying these must not touch the swapchain.
// ---------------------------------------------------------------------------

TEST(HdrOutputMatch, UnchangedHdrRequestIsSatisfied)
{
    EXPECT_TRUE(Satisfies(LiveHdr10(), gr::HdrOutputMode::HDR10_PQ));
}

TEST(HdrOutputMatch, UnchangedOffRequestIsSatisfied)
{
    EXPECT_TRUE(Satisfies(LiveOff(), gr::HdrOutputMode::Off));
}

// The load-bearing case for machines where Windows HDR is off: the device holds
// the HDR request but resolved the surface to SDR. Nothing changed, so opening a
// project must not recreate the swapchain.
TEST(HdrOutputMatch, FallenBackToSdrIsStillSatisfied)
{
    gr::HdrOutputState live = LiveHdr10();
    live.activeMode = gr::HdrOutputMode::Off; // no HDR colorspace on this surface
    EXPECT_TRUE(Satisfies(live, gr::HdrOutputMode::HDR10_PQ));
}

// Auto is the mode projects actually carry: it is what the settings page writes
// when HDR is enabled with no explicit mode, and what LoadEditorHdrOutputRequest
// coerces enabled-plus-Off into. Auto is not Off, so it must take the
// request-comparing branch — folding it in with Off would report every Auto
// project as unsatisfied and re-apply on every open.
TEST(HdrOutputMatch, UnchangedAutoRequestIsSatisfied)
{
    EXPECT_TRUE(Satisfies(LiveAuto(), gr::HdrOutputMode::Auto));
}

// Bit depth and metadata never reach the output chain while HDR is off, so an
// Off request must not be re-applied over them.
TEST(HdrOutputMatch, OffRequestIgnoresBitDepthAndMetadata)
{
    gr::HdrOutputState live = LiveOff();
    live.swapchainBitDepth = gr::HdrSwapchainBitDepth::Float16;
    live.staticMetadata.paperWhiteNits = 999.0f;
    EXPECT_TRUE(Satisfies(live, gr::HdrOutputMode::Off, gr::HdrSwapchainBitDepth::Bit10));
}

// ---------------------------------------------------------------------------
// The must-apply cases: each is a real difference the device has to be told.
// ---------------------------------------------------------------------------

TEST(HdrOutputMatch, TurningHdrOnIsNotSatisfied)
{
    EXPECT_FALSE(Satisfies(LiveOff(), gr::HdrOutputMode::HDR10_PQ));
}

TEST(HdrOutputMatch, TurningHdrOffIsNotSatisfied)
{
    EXPECT_FALSE(Satisfies(LiveHdr10(), gr::HdrOutputMode::Off));
}

// Turning HDR on via Auto against an SDR device has to apply. Treating Auto as
// an Off-equivalent would report this as already satisfied, and enabling HDR
// would silently never reach the device.
TEST(HdrOutputMatch, TurningHdrOnWithAutoIsNotSatisfied)
{
    EXPECT_FALSE(Satisfies(LiveOff(), gr::HdrOutputMode::Auto));
}

TEST(HdrOutputMatch, DifferentModeIsNotSatisfied)
{
    EXPECT_FALSE(Satisfies(LiveHdr10(), gr::HdrOutputMode::HLG));
    EXPECT_FALSE(Satisfies(LiveHdr10(), gr::HdrOutputMode::ScRGB));
    EXPECT_FALSE(Satisfies(LiveHdr10(), gr::HdrOutputMode::HDR10Plus));
}

TEST(HdrOutputMatch, DifferentBitDepthIsNotSatisfied)
{
    EXPECT_FALSE(Satisfies(LiveHdr10(gr::HdrSwapchainBitDepth::Bit10),
                           gr::HdrOutputMode::HDR10_PQ, gr::HdrSwapchainBitDepth::Float16));
    EXPECT_FALSE(Satisfies(LiveHdr10(gr::HdrSwapchainBitDepth::Float16),
                           gr::HdrOutputMode::HDR10_PQ, gr::HdrSwapchainBitDepth::Bit10));
}

// A live state whose `enabled` was never set cannot satisfy an HDR request even
// if requestedMode happens to match — that combination means the request was
// rejected, not accepted.
TEST(HdrOutputMatch, ModeMatchWithoutEnabledIsNotSatisfied)
{
    gr::HdrOutputState live = LiveHdr10();
    live.enabled = false;
    EXPECT_FALSE(Satisfies(live, gr::HdrOutputMode::HDR10_PQ));
}

// An Off request against a state that still reports an active HDR mode must
// apply: the output chain is demonstrably not in SDR yet.
TEST(HdrOutputMatch, OffRequestWithLingeringActiveModeIsNotSatisfied)
{
    gr::HdrOutputState live = LiveOff();
    live.activeMode = gr::HdrOutputMode::HDR10_PQ;
    EXPECT_FALSE(Satisfies(live, gr::HdrOutputMode::Off));
}

// The same fell-back-to-SDR device as FallenBackToSdrIsStillSatisfied, asked for
// Off instead. activeMode is already Off, but the device still holds the HDR
// request, so the Off apply has to go through and clear it. Reachable on any
// machine with Windows HDR off: project A with HDR on leaves the device
// {enabled, requestedMode=HDR10_PQ, activeMode=Off} because
// IsHdrRequestEnabledForMonitor requests optimistically; opening project B with
// HDR off lands here. Skipping the apply would strand the stale HDR request for
// a later resize or monitor change to act on, turning HDR on for a project that
// disabled it.
TEST(HdrOutputMatch, OffRequestAgainstStaleHdrRequestIsNotSatisfied)
{
    gr::HdrOutputState live = LiveHdr10();
    live.activeMode = gr::HdrOutputMode::Off;
    EXPECT_FALSE(Satisfies(live, gr::HdrOutputMode::Off));
}

// ---------------------------------------------------------------------------
// Metadata: every field is a real device input, so each one alone must force an
// apply. A partial comparison would silently strand tuned metadata.
// ---------------------------------------------------------------------------

TEST(HdrOutputMatch, EachMetadataFieldDifferenceForcesApply)
{
    const gr::HdrOutputState live = LiveHdr10();
    const auto expectDiffers = [&live](const gr::HdrStaticMetadata& changed, const char* what) {
        EXPECT_FALSE(Satisfies(live, gr::HdrOutputMode::HDR10_PQ,
                               gr::HdrSwapchainBitDepth::Bit10, changed))
            << "metadata difference ignored: " << what;
    };

    gr::HdrStaticMetadata m{};
    m.paperWhiteNits += 1.0f;
    expectDiffers(m, "paperWhiteNits");

    m = {};
    m.maxMasteringLuminance += 1.0f;
    expectDiffers(m, "maxMasteringLuminance");

    m = {};
    m.minMasteringLuminance += 1.0f;
    expectDiffers(m, "minMasteringLuminance");

    m = {};
    m.maxContentLightLevel += 1.0f;
    expectDiffers(m, "maxContentLightLevel");

    m = {};
    m.maxFrameAverageLightLevel += 1.0f;
    expectDiffers(m, "maxFrameAverageLightLevel");

    m = {};
    m.redPrimary[0] += 0.01f;
    expectDiffers(m, "redPrimary");

    m = {};
    m.greenPrimary[1] += 0.01f;
    expectDiffers(m, "greenPrimary");

    m = {};
    m.bluePrimary[0] += 0.01f;
    expectDiffers(m, "bluePrimary");

    m = {};
    m.whitePoint[0] += 0.01f;
    expectDiffers(m, "whitePoint");
}

TEST(HdrOutputMatch, SameMetadataIsReflexiveAndDefaultsCompareEqual)
{
    const gr::HdrStaticMetadata a{};
    const gr::HdrStaticMetadata b{};
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a == a);
    EXPECT_FALSE(a != b);
}

// Tuned-for-monitor metadata is what actually reaches the device, so a request
// carrying it must be recognised against a live state that already has it.
TEST(HdrOutputMatch, TunedMetadataRoundTripsAsSatisfied)
{
    gr::HdrStaticMetadata tuned{};
    tuned.paperWhiteNits = 203.0f;
    tuned.maxMasteringLuminance = 1400.0f;
    tuned.maxContentLightLevel = 1400.0f;
    tuned.maxFrameAverageLightLevel = 500.0f;
    tuned.minMasteringLuminance = 0.0005f;

    gr::HdrOutputState live = LiveHdr10();
    live.staticMetadata = tuned;

    EXPECT_TRUE(Satisfies(live, gr::HdrOutputMode::HDR10_PQ,
                          gr::HdrSwapchainBitDepth::Bit10, tuned));
}

// ---------------------------------------------------------------------------
// ResolveHdrOutputRequest — project setting vs GE_FORCE_HDR_OUTPUT.
//
// The case that motivated this: a project pinning "hdr": {"enabled": true}
// used to make the env var a no-op, so every lane following the standing
// "launch with GE_FORCE_HDR_OUTPUT=Off" instruction on such a project ran with
// HDR on and reported otherwise. An override that silently loses is worse than
// no override, so an explicitly set one wins.
// ---------------------------------------------------------------------------

TEST(HdrOutputRequestResolution, ExplicitOffOverridesAProjectThatPinsHdrOn)
{
    const ed::HdrOutputRequestResolution resolved =
        ed::ResolveHdrOutputRequest(/*projectEnabled=*/true, gr::HdrOutputMode::Auto,
                                    gr::HdrOutputMode::Off);
    EXPECT_FALSE(resolved.enabled);
    EXPECT_EQ(resolved.mode, gr::HdrOutputMode::Off);
}

TEST(HdrOutputRequestResolution, ExplicitModeOverridesAProjectThatPinsHdrOff)
{
    const ed::HdrOutputRequestResolution resolved =
        ed::ResolveHdrOutputRequest(/*projectEnabled=*/false, gr::HdrOutputMode::Off,
                                    gr::HdrOutputMode::HDR10_PQ);
    EXPECT_TRUE(resolved.enabled);
    EXPECT_EQ(resolved.mode, gr::HdrOutputMode::HDR10_PQ);
}

TEST(HdrOutputRequestResolution, NoOverrideLeavesTheProjectSettingGoverning)
{
    const ed::HdrOutputRequestResolution on =
        ed::ResolveHdrOutputRequest(/*projectEnabled=*/true, gr::HdrOutputMode::HLG, std::nullopt);
    EXPECT_TRUE(on.enabled);
    EXPECT_EQ(on.mode, gr::HdrOutputMode::HLG);

    const ed::HdrOutputRequestResolution off =
        ed::ResolveHdrOutputRequest(/*projectEnabled=*/false, gr::HdrOutputMode::Off, std::nullopt);
    EXPECT_FALSE(off.enabled);
    EXPECT_EQ(off.mode, gr::HdrOutputMode::Off);
}

// An enabled request must never carry mode Off, or ApplyForMonitor would ask the
// device for "on, but Off" and HdrOutputStateSatisfiesRequest would compare
// against the wrong arm.
TEST(HdrOutputRequestResolution, EnabledRequestNeverCarriesModeOff)
{
    const ed::HdrOutputRequestResolution resolved =
        ed::ResolveHdrOutputRequest(/*projectEnabled=*/true, gr::HdrOutputMode::Off, std::nullopt);
    EXPECT_TRUE(resolved.enabled);
    EXPECT_EQ(resolved.mode, gr::HdrOutputMode::Auto);
}

} // namespace
