// RenderServices dynamic-resolution mode state machine. The controller's
// control law is pinned in DynamicResolutionControllerTests; these tests pin
// the layer ABOVE it — mode transitions, the Off (Native) scale pin, the
// once-per-app-frame tick dedup, and the warn-once latching — which is where
// the 2026-07-24 adversarial review found every defect. None of these paths
// touch the device, so a bare RenderServices instance suffices.

#include "Engine/Rendering/RenderServices.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>

using GameEngine::Engine::Renderer::DynamicResolutionConfig;
using GameEngine::Engine::Renderer::DynamicResolutionMode;
using GameEngine::Engine::Renderer::DynamicResolutionSample;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{

DynamicResolutionSample MakeSample(float gpuMs)
{
    DynamicResolutionSample s{};
    s.GpuMs = gpuMs;
    s.Valid = true;
    // AppliedScale 0 = unknown: the controller falls back to its own scale,
    // which is exact here (no even-pixel snap in a headless test).
    return s;
}

// Response detection off (a constant synthetic cost reads as a dead lever and
// the detector would hand the resolution back, masking the mechanic under
// test); short dwell so a handful of ticks crosses the whole scale range.
DynamicResolutionConfig SaturationConfig()
{
    DynamicResolutionConfig c{};
    c.TargetGpuMs = 10.0f;
    c.DwellSeconds = 0.05f;
    c.MinUsefulSavingFraction = 0.0f;
    return c;
}

// Drive the controller with a constant heavy cost for `ticks` app frames.
uint64_t DriveHeavy(RenderServices& rs, uint64_t epoch, int ticks)
{
    for (int i = 0; i < ticks; ++i)
        rs.UpdateDynamicResolution(MakeSample(50.0f), 0.1f, epoch++);
    return epoch;
}

} // namespace

// The review's blocker: at startup the persisted slider value is applied
// BEFORE the mode resolves, and the default mode is already Off — so the
// resolve is a same-mode call. It must still pin the Native scale, or a
// project that used Fixed once and then chose Native silently keeps upscaling
// at the leftover persisted scale after every restart.
TEST(RenderServicesDrsMode, OffResolvedAtStartupPinsNativeScale)
{
    RenderServices rs;
    ASSERT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Off);

    rs.SetDefaultRenderScale(0.76f); // persisted rendering.taaRenderScale, applied first
    ASSERT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.76f);

    rs.SetDynamicResolutionMode(DynamicResolutionMode::Off); // startup mode resolve
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 1.0f)
        << "Native must pin the scale even when the mode is already Off";
}

TEST(RenderServicesDrsMode, StartupFixedKeepsPersistedScaleAndOffPinsIt)
{
    RenderServices rs;
    rs.SetDefaultRenderScale(0.76f);
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Fixed);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.76f);

    rs.SetDynamicResolutionMode(DynamicResolutionMode::Off);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 1.0f);
}

TEST(RenderServicesDrsMode, StartupDynamicStartsAtPersistedScaleAndFixedRestoresIt)
{
    RenderServices rs;
    rs.SetDefaultRenderScale(0.76f);
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.76f);

    rs.SetDynamicResolutionMode(DynamicResolutionMode::Fixed);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.76f);
}

// Leaving Dynamic restores the scale captured at entry — and an explicit set
// AFTER the mode switch wins. The settings panel relies on exactly this order
// (mode first, then re-apply the persisted file value) so that a slider moved
// during Dynamic — persisted but deliberately not applied — is not clobbered
// by the stale entry capture.
TEST(RenderServicesDrsMode, LeavingDynamicRestoresEntryScaleThenExplicitSetWins)
{
    RenderServices rs;
    rs.SetDefaultRenderScale(0.8f);
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Fixed);
    rs.SetDynamicResolutionConfig(SaturationConfig());
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);

    DriveHeavy(rs, 1, 60);
    ASSERT_LT(rs.GetDefaultRenderScale(), 0.8f) << "the controller never moved the scale";

    rs.SetDynamicResolutionMode(DynamicResolutionMode::Fixed);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.8f) << "Dynamic exit must restore the entry scale";

    rs.SetDefaultRenderScale(0.7f); // the panel's file re-apply, after the mode switch
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.7f);
}

// The multi-window contract: the dedup key is the spine's app-frame epoch, so
// a second window declaring in the same app frame must not tick the controller
// again (pre-fix, per-window RGFrame counters let it through and dwell/slew/
// EMA ran at N× real time with N windows).
TEST(RenderServicesDrsMode, UpdateDedupsOnAppFrameEpoch)
{
    RenderServices rs;
    rs.SetDynamicResolutionConfig(SaturationConfig());
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);

    rs.UpdateDynamicResolution(MakeSample(20.0f), 0.05f, 7);
    const auto& stats = rs.GetDynamicResolutionStats();
    ASSERT_EQ(stats.Ticks, 1u);
    const float smoothedAfterFirst = stats.SmoothedGpuMs;
    ASSERT_GT(smoothedAfterFirst, 0.0f);

    // Second window, same app frame: ignored entirely.
    rs.UpdateDynamicResolution(MakeSample(100.0f), 0.05f, 7);
    EXPECT_EQ(stats.Ticks, 1u);
    EXPECT_FLOAT_EQ(stats.SmoothedGpuMs, smoothedAfterFirst);

    // Next app frame: accepted.
    rs.UpdateDynamicResolution(MakeSample(100.0f), 0.05f, 8);
    EXPECT_EQ(stats.Ticks, 2u);
    EXPECT_GT(stats.SmoothedGpuMs, smoothedAfterFirst);
}

// A mode change resets the dedup key: re-entering Dynamic on the same epoch
// value as the last pre-transition tick must not swallow the fresh episode's
// first tick.
TEST(RenderServicesDrsMode, ModeChangeResetsTickDedup)
{
    RenderServices rs;
    rs.SetDynamicResolutionConfig(SaturationConfig());
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);
    rs.UpdateDynamicResolution(MakeSample(20.0f), 0.05f, 5);
    ASSERT_EQ(rs.GetDynamicResolutionStats().Ticks, 1u);

    rs.SetDynamicResolutionMode(DynamicResolutionMode::Off);
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic); // Reset() zeroes Stats

    rs.UpdateDynamicResolution(MakeSample(20.0f), 0.05f, 5);
    EXPECT_EQ(rs.GetDynamicResolutionStats().Ticks, 1u);
    EXPECT_GT(rs.GetDynamicResolutionStats().SmoothedGpuMs, 0.0f)
        << "the first tick of the new episode was swallowed by a stale dedup key";
}

// The unreachable-target notice latches for the episode: exactly one log line
// no matter how long the condition persists, re-armed only by a config or mode
// change (re-arming on the condition dipping false produced 7 identical lines
// in 2 s during the 2026-07-24 gate-scene run).
TEST(RenderServicesDrsMode, UnreachableWarningLatchesUntilConfigOrModeChange)
{
    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto warningCount = std::make_shared<std::atomic<int>>(0);
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [warningCount](const Logger::LogMessage& msg)
        {
            if (msg.Message.find("dynamic resolution is at its minimum scale") !=
                Logger::String::npos)
                warningCount->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    RenderServices rs;
    rs.SetDynamicResolutionConfig(SaturationConfig());
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);

    // 50 ms against a 10 ms target saturates at MinScale and stays over.
    uint64_t epoch = DriveHeavy(rs, 1, 60);
    ASSERT_TRUE(rs.GetDynamicResolutionStats().SaturatedLow);
    Logger::Log::Flush();
    EXPECT_EQ(warningCount->load(), 1);

    // The condition persists — the notice must not repeat.
    epoch = DriveHeavy(rs, epoch, 60);
    Logger::Log::Flush();
    EXPECT_EQ(warningCount->load(), 1);

    // A config change re-arms the episode.
    rs.SetDynamicResolutionConfig(SaturationConfig());
    epoch = DriveHeavy(rs, epoch, 60);
    Logger::Log::Flush();
    EXPECT_EQ(warningCount->load(), 2);

    // So does a mode round-trip.
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Off);
    rs.SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);
    DriveHeavy(rs, epoch, 60);
    Logger::Log::Flush();
    EXPECT_EQ(warningCount->load(), 3);

    sinkPtr->UnregisterCallback(callbackId);
}
