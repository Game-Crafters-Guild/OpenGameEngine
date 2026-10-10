// I11's declare-time half, at the only place that can break it.
//
// ── WHAT REPLACED WHAT, AND WHY ──────────────────────────────────────────────
// This supersedes HeadlessViewTransportGuardTests and the TransportSourceGuard
// it exercised. That guard scanned the render graph for OTHER passes taking a
// sampled read of the finalized image between S2 and S4. It was correct, and it
// was aimed at the wrong subject.
//
// Its only production declarer sat behind `if (m_PixelPerfectStateRG.Active)`,
// so in the one editor session it was observed in it NEVER EXECUTED. That zero
// was UNARMED — it is not evidence about how often its class occurs, and saying
// otherwise would be restating a check that never ran as a negative result. What
// can be said without overreaching is narrower: the stage contract admits no
// sampled reader in that window, three of its four tests had to construct a
// synthetic offender to produce one, and no in-tree pass was ever found taking
// that read.
//
// The resample the engine actually performs — the terminal encode's own bilinear
// rescale — was invisible to it either way, because a pass cannot catch itself
// by scanning other passes' declarations.
//
// The deciding property is a sampler's filter mode, which no declaration
// carries. But each pass knows its OWN sampler, so the invariant belongs to the
// pass rather than to the graph:
//   PixelPerfectUpscalePass samples NEAREST (NearestClamp) -> replicates texels,
//     cannot low-pass a dither, and is the exemption the invariant is carved
//     around. It needs no check.
//   SRGBEncodePass samples LINEAR (SRGBEncode.LinearClamp) -> at matched extent
//     with aligned texel centres it returns exact texels and the transfer is
//     byte-clean; the instant the extents differ it is a bilinear rescale of an
//     already-dithered image. It asserts the extent rule on itself.
// Every host reaches the terminal encode, so this covers the shipped Player as
// well as the editor — which the scan never did.
//
// ── WHY EACH ASSERTION BELOW CAN FAIL ───────────────────────────────────────
// ReportsAnEncodedRescale is the red arm, permanent: the real
// AddSRGBEncodePassRG, an encoded source, a destination at a different extent,
// and the report must name BOTH extents so a reader knows which rescale.
//
// IsSilentAtMatchedExtentOverTheSameSource is its discriminator, and it is the
// assertion that makes the red arm mean something. Same pass, same encoded
// source, same frame — ONLY the destination extent differs. A check that fired
// on "encoded arm" alone rather than on the extent mismatch passes the red arm
// and fails here.
//
// IsSilentOnTheLinearArmAtTheSameMismatch pins the arming condition against the
// same mismatched extents: a linear source has not been dithered yet, so
// rescaling it destroys nothing this invariant protects.
//
// RealChainWithHudAndTransportIsSilent is the production-shape control. It is
// readable only BECAUSE the red arm fires on the same production function.
//
// Skip policy (chain contract rule 6): absent device -> FAIL; absent staged
// asset -> FAIL; absent format capability -> SKIP, naming the format.

#include "HeadlessViewFixture.h"
#include "RGPassQuery.h"
#include "TestDeviceHelper.h"

#include "Engine/Rendering/ViewFinalize.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "UI/UITextureSpace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Testing;
using GameEngine::Rendering::TextureFormat;
namespace RG = GameEngine::Rendering::RenderGraph;
namespace RGQuery = GameEngine::Testing::RGQuery;
namespace Passes = GameEngine::Rendering::Passes;

namespace
{

void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// The dither and deband kill switches latch into function-local statics on
// first read, so they are a property of the PROCESS. Every TU in this target
// pins the same values itself: cross-TU initialization order is unspecified,
// and an instrument that depends on which TU constructed first is not one.
struct PinEnvironment
{
    PinEnvironment()
    {
        SetEnv("GE_HEADLESS_TEST", "1");
        SetEnv("GE_OUTPUT_DITHER", "1");
        SetEnv("GE_DEBAND", "0");
        SetEnv("GE_DEBAND_THRESHOLD", "");
        SetEnv("GE_DEBAND_RADIUS", "");
    }
} g_pinEnvironment;

// A stable substring of the report. Duplicated from production on purpose: the
// message IS the diagnostic surface, so a test that stopped matching it would
// be reporting on a channel nobody reads.
constexpr char kRescaleMarker[] = "rescales an ENCODED source";

// Counts reports and keeps the last for diagnosis. Shared state, not `this`:
// sinks are never removed from the logger, so a callback capturing a probe that
// had gone out of scope would dangle for the rest of the process.
struct ReportLogState
{
    std::atomic<int> Count{0};
    std::string Last;
};

class RescaleLogProbe
{
  public:
    RescaleLogProbe() : m_State(std::make_shared<ReportLogState>())
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        auto state = m_State;
        sink->RegisterCallback(
            [state](const Logger::LogMessage& msg)
            {
                if (msg.Message.find(kRescaleMarker) == Logger::String::npos)
                    return;
                state->Last = msg.Message;
                state->Count.fetch_add(1);
            });
        Logger::Log::AddSink(std::move(sink));
    }

    // The logger drains on its own thread, so a count read without this is a
    // read of whatever happened to have arrived.
    int Count() const
    {
        Logger::Log::Flush();
        return m_State->Count.load();
    }
    const std::string& Last() const
    {
        Logger::Log::Flush();
        return m_State->Last;
    }

  private:
    std::shared_ptr<ReportLogState> m_State;
};

// The frames below are DECLARED and never executed: every assertion reads the
// declaration, which is complete the moment the pass is added.
struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit FramePools(Rendering::IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

constexpr uint32_t kSrcExtent = 64;
constexpr uint32_t kDifferentExtent = 128; // a real rescale, not a rounding wobble

// The fixture's padded-source contract for the real-chain arm: a reference
// window plus one texel of border per side, upscaled by an integer zoom.
constexpr uint32_t kViewExtent = 66;
constexpr uint32_t kReferenceExtent = kViewExtent - 2;
constexpr uint32_t kZoom = 2;
constexpr uint32_t kLetterboxSurplus = 12;

Rendering::TextureDesc ColorDesc(uint32_t extent, const char* name)
{
    Rendering::TextureDesc d{};
    d.width = extent;
    d.height = extent;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = 1;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
              static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    d.debugName = name;
    return d;
}

// Declares the REAL terminal encode. `Quantizer` is None throughout: on the
// encoded arm the source already sits on the output step, which is precisely
// why nothing re-dithers after a rescale and the loss is permanent.
bool DeclareEncode(RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst,
                   Passes::FinalizeInputSpace inputSpace, const char* name)
{
    return Passes::AddSRGBEncodePassRG(
               frame, src, dst,
               {.InputSpace = inputSpace, .Quantizer = Passes::FinalizeQuantizer::None}, name)
        .IsValid();
}

} // namespace

// Per-TU by convention in this target, like every sibling headless suite: the
// skip policy is a property of the suite, so each states its own rather than
// inheriting one a shared header could silently change.
#define HV_REQUIRE_UP(fx)                                                                         \
    do                                                                                            \
    {                                                                                             \
        const HeadlessViewStatus st = (fx).Up();                                                  \
        ASSERT_NE(st, HeadlessViewStatus::NoDevice)                                               \
            << "device absent is a FAILURE for this suite, not a skip: " << (fx).StatusMessage(); \
        ASSERT_NE(st, HeadlessViewStatus::StagedAssetMissing)                                     \
            << "staging is part of the test: " << (fx).StatusMessage();                           \
        ASSERT_EQ(st, HeadlessViewStatus::Ok) << (fx).StatusMessage();                            \
    } while (false)

// ── The red arm, permanently in the tree ────────────────────────────────────
TEST(TransportExtentInvariant, ReportsAnEncodedRescale)
{
    auto device = CreateVulkanDeviceFast();
    ASSERT_TRUE(device) << "device absent is a FAILURE for this suite, not a skip";
    RescaleLogProbe probe;
    {
        FramePools pools(device.get());
        RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        const RG::RGTexture src =
            frame.ImportPersistentTexture("Extent.Src", ColorDesc(kSrcExtent, "Extent.Src"));
        const RG::RGTexture dst = frame.ImportPersistentTexture(
            "Extent.BigDst", ColorDesc(kDifferentExtent, "Extent.BigDst"));
        ASSERT_TRUE(src.IsValid() && dst.IsValid());

        ASSERT_TRUE(DeclareEncode(frame, src, dst, Passes::FinalizeInputSpace::EncodedSrgb,
                                  "Extent.RescalingEncode"))
            << "the encode declared nothing — srgb_encode.shaderpkg unstaged is a staged-asset "
               "FAILURE for this suite, and without S4 there is no invariant to break";

        ASSERT_EQ(probe.Count(), 1)
            << "an encoded " << kSrcExtent << "x" << kSrcExtent << " source was rescaled to "
            << kDifferentExtent << "x" << kDifferentExtent
            << " through a linear sampler and nothing said so — the assertion cannot fail, which "
               "makes it worse than none";
        EXPECT_NE(probe.Last().find("Extent.RescalingEncode"), std::string::npos)
            << "the report did not name the offending pass: " << probe.Last();
        // Both extents, so a reader knows WHICH rescale without a debugger.
        EXPECT_NE(probe.Last().find("64x64"), std::string::npos)
            << "the report did not name the source extent: " << probe.Last();
        EXPECT_NE(probe.Last().find("128x128"), std::string::npos)
            << "the report did not name the destination extent: " << probe.Last();

        std::cout << "[ MEASURED ] " << probe.Last() << std::endl;
    }
    device->Shutdown();
}

// ── The discriminator: only the extent differs ──────────────────────────────
// Without this, the red arm above is satisfied by a check that fires on the
// encoded arm regardless of extent — which would report on every byte-clean
// transfer in the engine.
TEST(TransportExtentInvariant, IsSilentAtMatchedExtentOverTheSameSource)
{
    auto device = CreateVulkanDeviceFast();
    ASSERT_TRUE(device) << "device absent is a FAILURE for this suite, not a skip";
    RescaleLogProbe probe;
    {
        FramePools pools(device.get());
        RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        const RG::RGTexture src =
            frame.ImportPersistentTexture("Match.Src", ColorDesc(kSrcExtent, "Match.Src"));
        const RG::RGTexture same = frame.ImportPersistentTexture(
            "Match.SameSizeDst", ColorDesc(kSrcExtent, "Match.SameSizeDst"));
        ASSERT_TRUE(src.IsValid() && same.IsValid());

        ASSERT_TRUE(DeclareEncode(frame, src, same, Passes::FinalizeInputSpace::EncodedSrgb,
                                  "Match.CleanEncode"));
        EXPECT_EQ(probe.Count(), 0)
            << "a matched-extent encode of an encoded source is the BYTE-CLEAN transfer this "
               "invariant exists to permit, and it reported: "
            << probe.Last();

        // Same frame, same source, same pass, same encoded arm — only the
        // destination extent changes. If this does not fire, the check is not
        // reading the extent at all and the silence above proves nothing.
        const RG::RGTexture bigger = frame.ImportPersistentTexture(
            "Match.BiggerDst", ColorDesc(kDifferentExtent, "Match.BiggerDst"));
        ASSERT_TRUE(DeclareEncode(frame, src, bigger, Passes::FinalizeInputSpace::EncodedSrgb,
                                  "Match.RescalingEncode"));
        EXPECT_EQ(probe.Count(), 1)
            << "the mismatched arm did not report over the SAME source the matched arm just "
               "passed, so the silence above says nothing about the extent test";
    }
    device->Shutdown();
}

// ── The other half of the arming condition: who owns the step ───────────────
// A rescale is only lossy when NOTHING re-dithers afterwards. When this transfer
// owns the destination's quantizer it re-dithers at the destination's resolution
// AFTER its sample, so a mismatched extent under FinalizeQuantizer::Destination
// is legitimate and reporting it would be a false alarm — with a message whose
// stated diagnosis ("re-rounded undithered") is untrue of that path.
TEST(TransportExtentInvariant, IsSilentWhenThisTransferOwnsTheStep)
{
    auto device = CreateVulkanDeviceFast();
    ASSERT_TRUE(device) << "device absent is a FAILURE for this suite, not a skip";
    RescaleLogProbe probe;
    {
        FramePools pools(device.get());
        RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        const RG::RGTexture src =
            frame.ImportPersistentTexture("Own.Src", ColorDesc(kSrcExtent, "Own.Src"));
        const RG::RGTexture dst = frame.ImportPersistentTexture(
            "Own.BigDst", ColorDesc(kDifferentExtent, "Own.BigDst"));
        ASSERT_TRUE(src.IsValid() && dst.IsValid());

        ASSERT_TRUE(Passes::AddSRGBEncodePassRG(
                        frame, src, dst,
                        {.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb,
                         .Quantizer = Passes::FinalizeQuantizer::Destination},
                        "Own.RequantizingEncode")
                        .IsValid());
        EXPECT_EQ(probe.Count(), 0)
            << "reported a rescale that this transfer re-dithers itself: " << probe.Last();

        // Same source, same mismatch, same encoded arm — only the quantizer
        // changes. Without this the silence above would also be produced by a
        // check that had simply stopped working.
        const RG::RGTexture dst2 = frame.ImportPersistentTexture(
            "Own.BigDst2", ColorDesc(kDifferentExtent, "Own.BigDst2"));
        ASSERT_TRUE(DeclareEncode(frame, src, dst2, Passes::FinalizeInputSpace::EncodedSrgb,
                                  "Own.NonOwningEncode"));
        EXPECT_EQ(probe.Count(), 1)
            << "the None-quantizer arm did not report at the SAME mismatch the owning arm just "
               "passed, so the silence above says nothing about the quantizer test";
    }
    device->Shutdown();
}

// ── The arming condition, against the same mismatch ─────────────────────────
TEST(TransportExtentInvariant, IsSilentOnTheLinearArmAtTheSameMismatch)
{
    auto device = CreateVulkanDeviceFast();
    ASSERT_TRUE(device) << "device absent is a FAILURE for this suite, not a skip";
    RescaleLogProbe probe;
    {
        FramePools pools(device.get());
        RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        const RG::RGTexture src =
            frame.ImportPersistentTexture("Arm.Src", ColorDesc(kSrcExtent, "Arm.Src"));
        const RG::RGTexture dst = frame.ImportPersistentTexture(
            "Arm.BigDst", ColorDesc(kDifferentExtent, "Arm.BigDst"));
        ASSERT_TRUE(src.IsValid() && dst.IsValid());

        ASSERT_TRUE(
            DeclareEncode(frame, src, dst, Passes::FinalizeInputSpace::Linear, "Arm.LinearEncode"));
        EXPECT_EQ(probe.Count(), 0)
            << "the linear arm reported: " << probe.Last()
            << " — a linear source carries no dither yet, so rescaling it destroys nothing this "
               "invariant protects";

        // Same mismatch, encoded arm: proves the silence above is about arming
        // and not about the extents being somehow acceptable.
        const RG::RGTexture dst2 = frame.ImportPersistentTexture(
            "Arm.BigDst2", ColorDesc(kDifferentExtent, "Arm.BigDst2"));
        ASSERT_TRUE(DeclareEncode(frame, src, dst2, Passes::FinalizeInputSpace::EncodedSrgb,
                                  "Arm.EncodedEncode"));
        EXPECT_EQ(probe.Count(), 1)
            << "the encoded arm did not report at the SAME mismatch the linear arm just passed";
    }
    device->Shutdown();
}

// ── Silence over the full production chain ──────────────────────────────────
TEST(TransportExtentInvariant, RealChainWithHudAndTransportIsSilent)
{
    RescaleLogProbe probe;
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);

    HeadlessViewDesc d{};
    d.Width = kViewExtent;
    d.Height = kViewExtent;
    d.Driver = FinalizeDriver::HostPolicy;
    d.PresentedFormat = TextureFormat::RGBA8_UNORM;
    d.Hud = HudDesc{R"(<uielement id="root"><uielement id="op"/></uielement>)",
                    R"(
#root { display: flex; flex-direction: column; width: 66px; height: 66px; }
#op { width: 32px; height: 16px; margin-left: 8px; margin-top: 8px; background-color: #6633cc; }
)"};
    // A REAL transport on the ENCODED arm. Without this the fixture declares no
    // upscale at all (HeadlessViewFixture.h:313-315), the only encode in the
    // frame is the finalize's own — hard-coded Linear at matched extent — and
    // the check never arms, which makes the whole test green against any
    // mutation of the extent comparison. It was written that way and it proved
    // nothing; the transport is what gives its silence content.
    TransportDesc t{};
    t.DestinationWidth = kReferenceExtent * kZoom + kLetterboxSurplus;
    t.DestinationHeight = kReferenceExtent * kZoom + kLetterboxSurplus;
    t.DestinationFormat = TextureFormat::RGBA8_UNORM;
    t.Zoom = kZoom;
    t.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb;
    d.Transport = t;

    const HeadlessViewStatus st = fx.Render(d);
    if (st == HeadlessViewStatus::FormatUnsupported)
        GTEST_SKIP() << "capability skip: " << fx.StatusMessage();
    ASSERT_EQ(st, HeadlessViewStatus::Ok) << ToString(st) << ": " << fx.StatusMessage();

    // Non-vacuity, and each clause earns its place: the finalize must have run
    // (else no encoded source exists), the transport must be SCHEDULED and
    // scheduled AFTER it (else the window this is silent about never formed),
    // and it must have produced bytes (else nothing transported).
    const std::optional<std::size_t> finalizeIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalized"});
    const std::optional<std::size_t> transportIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Transport"});
    ASSERT_TRUE(fx.Facts().Step.has_value()) << "the host policy declined — no encoded source";
    ASSERT_TRUE(finalizeIdx.has_value()) << "no scheduled finalize, so there is no encoded source";
    ASSERT_TRUE(transportIdx.has_value())
        << "no scheduled transport — this test is then silent for the wrong reason";
    EXPECT_LT(*finalizeIdx, *transportIdx);
    ASSERT_GT(fx.TransportOutput().Bytes.size(), 0u) << "the transport produced nothing";

    EXPECT_EQ(probe.Count(), 0) << "the production chain reported a rescale: " << probe.Last();

    std::cout << "[ MEASURED ] real chain silent: finalize at " << *finalizeIdx << ", transport at "
              << *transportIdx << " of " << fx.ScheduledPassNames().size()
              << " scheduled passes; transport output " << fx.TransportOutput().Width << "x"
              << fx.TransportOutput().Height << std::endl;
}
