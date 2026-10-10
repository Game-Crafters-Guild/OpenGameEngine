// CascadeShadowCacheTests — the static-scene cascade shadow cache's
// invalidation state machine (GE_SHADOW_STATIC_CACHE lane). Pure CPU: no
// device, no RenderServices. Covers the settle rule (one-frame cull lag with
// frame-stamp gap detection), every dirty cause in priority order, the
// exec→declare under-draw feedback, contributor pin + disappearance, the
// disabled-measurement mode, and the physical-lifetime invalidation hooks.
#include "Engine/Rendering/CascadeCacheLogFormat.h"
#include "Engine/Rendering/CascadeShadowCache.h"
#include "Engine/Rendering/DepthDrawRecorder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

using GameEngine::Engine::Renderer::CascadeCacheDirtyCause;
using GameEngine::Engine::Renderer::CascadeRenderInputs;
using GameEngine::Engine::Renderer::CascadeShadowCache;
using GameEngine::Engine::Renderer::ShadowCasterChangeSet;
using GameEngine::Engine::Renderer::ShadowCasterChangeSphere;

namespace
{

constexpr uint32_t kView = 7;
constexpr uint32_t kSlot = CascadeShadowCache::kDepthSlotBase + 1;

CascadeRenderInputs BaseInputs()
{
    CascadeRenderInputs in{};
    for (int i = 0; i < 16; ++i)
    {
        in.LightVP[i] = static_cast<float>(i) * 0.5f;
        in.LightVPRel[i] = static_cast<float>(i) * 0.5f;
        in.CameraViewProj[i] = static_cast<float>(i) * 0.25f + 1.0f;
    }
    in.CasterFootprint[0] = -0.5f;
    in.CasterFootprint[1] = -0.25f;
    in.CasterFootprint[2] = 0.75f;
    in.CasterFootprint[3] = 0.5f;
    in.CasterEpoch = 42;
    in.PhysicalId = 0xABCDEF01u;
    in.Resolution = 2048;
    in.NumCascades = 4;
    in.LODForceLevel = 0xFFFFFFFFu;
    return in;
}

// Drives frames the way the declare site does: each Frame() advances the
// stamp by one; FrameAt() sets an explicit stamp (Evaluate-gap simulation);
// a non-skipped Evaluate commits via OnRendered. CameraInSettle mirrors the
// declare site's fit-freeze wiring (includeCameraInSettle).
struct CacheDriver
{
    CascadeShadowCache Cache;
    uint64_t Stamp = 0;
    bool CameraInSettle = false;

    CascadeShadowCache::Decision Frame(const CascadeRenderInputs& in, bool enabled = true,
                                       uint32_t slot = kSlot)
    {
        return FrameAt(Stamp + 1, in, enabled, slot);
    }
    CascadeShadowCache::Decision FrameAt(uint64_t stamp, const CascadeRenderInputs& in,
                                         bool enabled = true, uint32_t slot = kSlot)
    {
        Stamp = stamp;
        const auto d = Cache.Evaluate(kView, slot, stamp, in, enabled, CameraInSettle);
        if (!d.Skip)
            Cache.OnRendered(kView, slot, in);
        return d;
    }
};

TEST(CascadeShadowCache, FirstRenderThenSettleThenSkip)
{
    CacheDriver drv;
    const auto in = BaseInputs();

    // Frame 1: never rendered.
    auto d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::FirstRender);

    // Frame 2: record matches, but frame 1's cull consumed the previous fit
    // (unknown) — the settle rule demands one more render.
    d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CullNotSettled);

    // Frame 3+: settled — skip.
    d = drv.Frame(in);
    EXPECT_TRUE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::Cached);
    d = drv.Frame(in);
    EXPECT_TRUE(d.Skip);
}

TEST(CascadeShadowCache, EachInputChangeMissesWithItsCause)
{
    struct Case
    {
        const char* Name;
        void (*Mutate)(CascadeRenderInputs&);
        CascadeCacheDirtyCause Expected;
    };
    const Case cases[] = {
        {"physical", [](CascadeRenderInputs& in) { in.PhysicalId ^= 1u; },
         CascadeCacheDirtyCause::PhysicalChanged},
        {"epoch", [](CascadeRenderInputs& in) { ++in.CasterEpoch; },
         CascadeCacheDirtyCause::CasterContentChanged},
        {"world", [](CascadeRenderInputs& in) { ++in.WorldId; },
         CascadeCacheDirtyCause::CasterContentChanged},
        {"camera", [](CascadeRenderInputs& in) { in.CameraViewProj[12] += 0.001f; },
         CascadeCacheDirtyCause::CameraChanged},
        {"lightVP", [](CascadeRenderInputs& in) { in.LightVP[12] += 0.01f; },
         CascadeCacheDirtyCause::CascadeFitChanged},
        {"lightVPRel", [](CascadeRenderInputs& in) { in.LightVPRel[13] += 0.01f; },
         CascadeCacheDirtyCause::CascadeFitChanged},
        {"casterFootprint", [](CascadeRenderInputs& in) { in.CasterFootprint[2] += 0.05f; },
         CascadeCacheDirtyCause::CascadeFitChanged},
        {"originSector", [](CascadeRenderInputs& in) { in.RenderOriginSector[1] += 1; },
         CascadeCacheDirtyCause::CascadeFitChanged},
        {"resolution", [](CascadeRenderInputs& in) { in.Resolution = 1024; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"numCascades", [](CascadeRenderInputs& in) { in.NumCascades = 3; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"lodBias", [](CascadeRenderInputs& in) { in.ShadowLODBias = 1.0f; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"lodForce", [](CascadeRenderInputs& in) { in.LODForceLevel = 2; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"renderLayers", [](CascadeRenderInputs& in) { in.RenderLayerMask = 0x80000000u; },
         CascadeCacheDirtyCause::ConfigChanged},
        // A selection-mode switch re-derives every mesh row's switch points, so a
        // retained cascade would hold LOD picks the camera pass no longer makes.
        {"lodSelectionMode", [](CascadeRenderInputs& in) { in.SelectionMode = 2u; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"sseCoverageScale", [](CascadeRenderInputs& in) { in.SseThresholdToCoverage += 0.001f; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"casterReduction", [](CascadeRenderInputs& in) { in.CasterReduction = true; },
         CascadeCacheDirtyCause::ConfigChanged},
        {"contributorAppears",
         [](CascadeRenderInputs& in) { in.HasContributorDrawCommands = true; },
         CascadeCacheDirtyCause::ContributorPresent},
    };

    for (const Case& c : cases)
    {
        CacheDriver drv;
        auto in = BaseInputs();
        drv.Frame(in);                          // FirstRender
        drv.Frame(in);                          // settle
        ASSERT_TRUE(drv.Frame(in).Skip) << c.Name;

        c.Mutate(in);
        const auto d = drv.Frame(in);
        EXPECT_FALSE(d.Skip) << c.Name;
        EXPECT_EQ(d.Cause, c.Expected) << c.Name;
    }
}

TEST(CascadeShadowCache, FitTransitionRequiresOneSettleRender)
{
    CacheDriver drv;
    auto in = BaseInputs();
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);

    // Camera orbit steps the snapped fit: miss, then ONE settle render (the
    // transition frame's cull consumed the old fit), then skips again.
    in.LightVP[12] += 0.02f;
    in.CameraViewProj[14] += 0.01f;
    auto d = drv.Frame(in);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CameraChanged); // camera checked before fit
    d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CullNotSettled);
    d = drv.Frame(in);
    EXPECT_TRUE(d.Skip);
}

TEST(CascadeShadowCache, FrozenCameraStepAloneRequiresResettleUnderFreeze)
{
    // Fit-freeze settle blind spot (rev-2 F1): under the freeze the cull
    // consumes the FROZEN camera out of frameData(N-1). A refit that steps
    // only the frozen camera while landing byte-identical fit bytes (LOD-
    // bound dolly on the static-splits path) rendered against the PREVIOUS
    // frozen snapshot — it must commit UNSETTLED, costing one extra render,
    // instead of freezing a layer culled/LOD'd with the old camera.
    CacheDriver drv;
    drv.CameraInSettle = true;
    auto in = BaseInputs();
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);

    in.CameraViewProj[12] += 0.01f; // frozen-camera step, fit bytes identical
    auto d = drv.Frame(in);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CameraChanged);
    d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CullNotSettled)
        << "camera-step render was culled with the previous frozen snapshot";
    EXPECT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowCache, CameraStepSettlesImmediatelyWithFreezeOff)
{
    // Flag-off exactness: without the freeze the camera stays out of the
    // settle equality — a camera-only step renders once and skips again, the
    // shipped baseline behavior (the residual is the pre-existing one-frame
    // cull lag the always-render pipeline also had).
    CacheDriver drv;
    auto in = BaseInputs();
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);

    in.CameraViewProj[12] += 0.01f;
    auto d = drv.Frame(in);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CameraChanged);
    EXPECT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowCache, EpochOnlyChangeSkipsAgainWithoutResettle)
{
    CacheDriver drv;
    auto in = BaseInputs();
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);

    // A caster moved (epoch bump) with an unchanged fit: the re-render's cull
    // consumed the SAME fit, so it is immediately a settled baseline — no
    // second render.
    ++in.CasterEpoch;
    auto d = drv.Frame(in);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CasterContentChanged);
    d = drv.Frame(in);
    EXPECT_TRUE(d.Skip) << "fit never changed - settle must not re-trigger";
}

TEST(CascadeShadowCache, EvaluateGapNeverCommitsSettled)
{
    // Reviewer scenario (cascade count 4→2→4): slots 2/3 stop being evaluated,
    // then return with a fit that EQUALS the last pre-gap Evaluate's. Without
    // stamp tracking the stale PrevFrame would settle the post-gap render —
    // whose cull slice was built by a frame that never scheduled the slot
    // (empty) — freezing an empty far cascade. With stamps, the gap render
    // commits UNSETTLED; only a render preceded by a CONSECUTIVE equal-fit
    // frame settles.
    CacheDriver drv;
    auto inA = BaseInputs();
    drv.FrameAt(1, inA);
    drv.FrameAt(2, inA);
    ASSERT_TRUE(drv.FrameAt(3, inA).Skip);

    auto inB = BaseInputs();
    inB.LightVP[12] += 0.02f; // fit B
    auto d = drv.FrameAt(4, inB); // transition frame: renders, unsettled (B != A)
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CascadeFitChanged);

    // Evaluate gap: stamps 5..14 never evaluated (slot dropped out). Post-gap
    // frame with fit B: PrevFrame fit matches (B) but across a gap → the
    // render must commit unsettled.
    d = drv.FrameAt(15, inB);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CullNotSettled);

    // Consecutive frame: the gap render was unsettled, so render once more —
    // this one is preceded by a consecutive equal-fit frame and settles.
    d = drv.FrameAt(16, inB);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CullNotSettled);
    d = drv.FrameAt(17, inB);
    EXPECT_TRUE(d.Skip);
}

TEST(CascadeShadowCache, ExecUnderDrawDirtiesUntilCleanRender)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);

    // Exec feedback: the last render silently under-drew (async publish gate /
    // stale-survivor walk-skip). The declare site consumes the tracker mark
    // and calls MarkUnderDrew BEFORE Evaluate — no skip this frame.
    drv.Cache.MarkUnderDrew(kView, kSlot);
    auto d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::ExecUnderDraw);

    // The re-render cleared the flag; with a clean exec (no new mark) and an
    // unchanged fit, the very next frame skips again.
    d = drv.Frame(in);
    EXPECT_TRUE(d.Skip);

    // A repeat under-draw re-dirties — the always-render 1-frame self-heal
    // loop, one mark per bad exec.
    drv.Cache.MarkUnderDrew(kView, kSlot);
    d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::ExecUnderDraw);
}

TEST(CascadeShadowCache, ContributorCommandsPinAlwaysRender)
{
    CacheDriver drv;
    auto in = BaseInputs();
    in.HasContributorDrawCommands = true;
    for (int i = 0; i < 4; ++i)
    {
        const auto d = drv.Frame(in);
        EXPECT_FALSE(d.Skip);
        EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::ContributorPresent);
    }
}

TEST(CascadeShadowCache, ContributorDisappearanceDirties)
{
    // Terrain unload: the pin vanishes, but the retained layer still holds the
    // contributor's depth — a matching key must NOT skip until a
    // contributor-free render commits (no phantom terrain shadows).
    CacheDriver drv;
    auto in = BaseInputs();
    in.HasContributorDrawCommands = true;
    drv.Frame(in);
    drv.Frame(in); // both pinned (ContributorPresent), both rendered

    in.HasContributorDrawCommands = false;
    auto d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::ContributorChanged);

    // The contributor-free render is a valid baseline; fit never changed, so
    // the next frame skips.
    d = drv.Frame(in);
    EXPECT_TRUE(d.Skip);
}

TEST(CascadeShadowCache, DisabledNeverSkipsButCountsWouldBeHits)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    for (int i = 0; i < 5; ++i)
    {
        const auto d = drv.Frame(in, /*enabled=*/false);
        EXPECT_FALSE(d.Skip);
    }
    const auto& st = drv.Cache.GetStats(kView);
    EXPECT_EQ(st.Evaluated, 5u);
    EXPECT_EQ(st.Skipped, 0u);
    // Frames 3..5 would have been hits — the measurement mode proves the
    // achievable rate without changing behavior.
    EXPECT_EQ(st.CauseCounts[static_cast<size_t>(CascadeCacheDirtyCause::Cached)], 3u);
}

TEST(CascadeShadowCache, InvalidateViewForcesFirstRenderAndResettle)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);

    drv.Cache.InvalidateView(kView);
    auto d = drv.Frame(in);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::FirstRender);
    d = drv.Frame(in);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::CullNotSettled);
    EXPECT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowCache, ResetDropsStatsAndRecords)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    drv.Frame(in);
    drv.Cache.Reset();
    EXPECT_EQ(drv.Cache.GetStats(kView).Evaluated, 0u);
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::FirstRender);
}

// --- Instrumentation window (the periodic [CascadeCache] log's deltas) ---

TEST(CascadeShadowCache, StatsWindowReportsDeltasNotTotals)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    for (int i = 0; i < 5; ++i)
        drv.Frame(in);
    EXPECT_EQ(drv.Cache.ConsumeStatsWindow(kView).Evaluated, 5u);

    for (int i = 0; i < 3; ++i)
        drv.Frame(in);
    const auto second = drv.Cache.ConsumeStatsWindow(kView);
    EXPECT_EQ(second.Evaluated, 3u);
    // Frames 6..8 are all steady-state hits, so the window's skip and Cached
    // counts move with it rather than restating the run total.
    EXPECT_EQ(second.Skipped, 3u);
    EXPECT_EQ(second.CauseCounts[static_cast<size_t>(CascadeCacheDirtyCause::Cached)], 3u);
}

// The device-rebuild path (ShadowMapRenderFeature::OnDeviceRebuild -> Reset)
// clears the totals. A snapshot that outlived them would make every field of
// the next window underflow uint64 — and no equality guard can catch it,
// because a large snapshot never equals a small total.
TEST(CascadeShadowCache, StatsWindowDoesNotUnderflowAcrossReset)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    for (int i = 0; i < 8; ++i)
        drv.Frame(in);
    ASSERT_EQ(drv.Cache.ConsumeStatsWindow(kView).Evaluated, 8u);
    for (int i = 0; i < 6; ++i)
        drv.Frame(in);

    drv.Cache.Reset();
    for (int i = 0; i < 2; ++i)
        drv.Frame(in);

    const auto window = drv.Cache.ConsumeStatsWindow(kView);
    EXPECT_EQ(window.Evaluated, 2u);
    EXPECT_EQ(window.Skipped, 0u); // both post-Reset frames re-render
    EXPECT_EQ(window.CauseCounts[static_cast<size_t>(CascadeCacheDirtyCause::FirstRender)], 1u);
    EXPECT_EQ(window.CauseCounts[static_cast<size_t>(CascadeCacheDirtyCause::CullNotSettled)], 1u);
}

// Every counter Evaluate touches advances Evaluated too, which is what makes
// "no evaluations this window" a safe reason to emit nothing: an empty window
// carries no unreported skips or causes with it.
TEST(CascadeShadowCache, EmptyWindowIsZeroInEveryField)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    for (int i = 0; i < 4; ++i)
        drv.Frame(in);
    ASSERT_EQ(drv.Cache.ConsumeStatsWindow(kView).Evaluated, 4u);

    const auto empty = drv.Cache.ConsumeStatsWindow(kView);
    EXPECT_EQ(empty.Evaluated, 0u);
    EXPECT_EQ(empty.Skipped, 0u);
    for (size_t i = 0; i < empty.CauseCounts.size(); ++i)
        EXPECT_EQ(empty.CauseCounts[i], 0u) << "cause index " << i;

    // ...and consuming an empty window does not disturb the next real one.
    drv.Frame(in);
    EXPECT_EQ(drv.Cache.ConsumeStatsWindow(kView).Evaluated, 1u);
}

TEST(CascadeShadowCache, StatsWindowIsUnknownViewSafe)
{
    CacheDriver drv;
    const auto window = drv.Cache.ConsumeStatsWindow(kView + 100);
    EXPECT_EQ(window.Evaluated, 0u);
    EXPECT_EQ(window.Skipped, 0u);
}

TEST(CascadeShadowCache, InvalidateViewKeepsTheStatsWindow)
{
    CacheDriver drv;
    const auto in = BaseInputs();
    for (int i = 0; i < 4; ++i)
        drv.Frame(in);
    ASSERT_EQ(drv.Cache.ConsumeStatsWindow(kView).Evaluated, 4u);

    // A physical realloc keeps the instrumentation running (unlike Reset), so
    // the snapshot must survive with the totals it references.
    drv.Cache.InvalidateView(kView);
    drv.Frame(in);
    EXPECT_EQ(drv.Cache.ConsumeStatsWindow(kView).Evaluated, 1u);
}

TEST(CascadeShadowCache, StormEdgeStateExchangesAndDiesWithTheStats)
{
    CacheDriver drv;
    EXPECT_FALSE(drv.Cache.ExchangeStormActive(kView, true));
    EXPECT_TRUE(drv.Cache.ExchangeStormActive(kView, true));
    EXPECT_TRUE(drv.Cache.ExchangeStormActive(kView, false));
    EXPECT_FALSE(drv.Cache.ExchangeStormActive(kView, true));

    // A device rebuild is a new episode: the next storm window must warn
    // rather than be suppressed by the pre-Reset flag.
    drv.Cache.Reset();
    EXPECT_FALSE(drv.Cache.ExchangeStormActive(kView, true));
}

// --- [CascadeCache] log line composition ---

// Distinct per-cause counts, so any permutation of the format arguments shows
// up as a mismatched label rather than passing silently.
TEST(CascadeCacheLogFormat, HistogramNamesEveryCauseInPriorityOrderWithTheCasterVersion)
{
    CascadeShadowCache::Stats window{};
    const auto set = [&window](CascadeCacheDirtyCause c, uint64_t v)
    { window.CauseCounts[static_cast<size_t>(c)] = v; };
    set(CascadeCacheDirtyCause::FirstRender, 1);
    set(CascadeCacheDirtyCause::ContributorPresent, 2);
    set(CascadeCacheDirtyCause::ContributorChanged, 3);
    set(CascadeCacheDirtyCause::ExecUnderDraw, 4);
    set(CascadeCacheDirtyCause::PhysicalChanged, 5);
    set(CascadeCacheDirtyCause::CasterContentChanged, 6);
    set(CascadeCacheDirtyCause::CameraChanged, 7);
    set(CascadeCacheDirtyCause::CascadeFitChanged, 8);
    set(CascadeCacheDirtyCause::ConfigChanged, 9);
    set(CascadeCacheDirtyCause::CullNotSettled, 10);

    EXPECT_EQ(GameEngine::Engine::Renderer::FormatCascadeCacheCauseHistogram(window, 4242u),
              "first 1 contrib 2 contribchg 3 underdraw 4 phys 5 casters 6@v4242 camera 7 fit 8 "
              "config 9 settle 10");
}

// The version is the point of the line: a caster miss count of zero next to a
// static version is exactly the state that was previously undiagnosable.
TEST(CascadeCacheLogFormat, CasterVersionPrintsWithNoCasterMisses)
{
    const CascadeShadowCache::Stats window{};
    EXPECT_NE(GameEngine::Engine::Renderer::FormatCascadeCacheCauseHistogram(window, 9001u)
                  .find("casters 0@v9001"),
              std::string::npos);
}

TEST(CascadeShadowCache, DepthAndTintSlotsAreIndependent)
{
    CacheDriver drv;
    auto depthIn = BaseInputs();
    auto tintIn = BaseInputs();
    tintIn.PhysicalId = 0x5555u; // tint array physical

    const uint32_t depthSlot = CascadeShadowCache::kDepthSlotBase + 2;
    const uint32_t tintSlot = CascadeShadowCache::kTintSlotBase + 2;

    drv.Frame(depthIn, true, depthSlot);
    drv.Frame(depthIn, true, depthSlot);
    ASSERT_TRUE(drv.Frame(depthIn, true, depthSlot).Skip);

    // The tint slot has its own record: first contact is FirstRender even
    // though the depth twin is warm.
    const auto d = drv.Frame(tintIn, true, tintSlot);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::FirstRender);
}

TEST(DepthUnderDrawTracker, MarkConsumeIsKeyedAndOneShot)
{
    GameEngine::Engine::Renderer::DepthUnderDrawTracker tracker;
    EXPECT_FALSE(tracker.Consume(1, 2, 3));

    tracker.Mark(1, 2, 3);
    EXPECT_FALSE(tracker.Consume(1, 2, 0)) << "different slice must not consume";
    EXPECT_FALSE(tracker.Consume(1, 0, 3)) << "different pass family must not consume";
    EXPECT_FALSE(tracker.Consume(9, 2, 3)) << "different view must not consume";
    EXPECT_TRUE(tracker.Consume(1, 2, 3));
    EXPECT_FALSE(tracker.Consume(1, 2, 3)) << "consume is one-shot";

    tracker.Mark(1, 2, 3);
    tracker.Mark(1, 2, 3); // duplicate marks collapse
    EXPECT_TRUE(tracker.Consume(1, 2, 3));
    EXPECT_FALSE(tracker.Consume(1, 2, 3));

    tracker.Mark(4, 5, 6);
    tracker.Reset();
    EXPECT_FALSE(tracker.Consume(4, 5, 6));
}

TEST(DepthUnderDrawTracker, PeekDoesNotConsume)
{
    GameEngine::Engine::Renderer::DepthUnderDrawTracker tracker;
    EXPECT_FALSE(tracker.Peek(1, 2, 3));
    tracker.Mark(1, 2, 3);
    EXPECT_TRUE(tracker.Peek(1, 2, 3));
    EXPECT_TRUE(tracker.Peek(1, 2, 3)) << "peek must not erase";
    EXPECT_FALSE(tracker.Peek(1, 2, 0));
    EXPECT_TRUE(tracker.Consume(1, 2, 3));
    EXPECT_FALSE(tracker.Peek(1, 2, 3));
}

// PeekCause must resolve exactly the cause the same frame's Evaluate resolves
// (the motion plan phase peeks BEFORE the declare sites' single mutating
// Evaluate) — and must not advance the settle tracking or the stats.
TEST(CascadeShadowCache, PeekCauseMatchesEvaluateAndIsReadOnly)
{
    CacheDriver drv;
    auto in = BaseInputs();

    // Unknown view: FirstRender, and still no stats entry afterwards.
    EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, in, false),
              CascadeCacheDirtyCause::FirstRender);
    EXPECT_EQ(drv.Cache.GetStats(kView).Evaluated, 0u);

    // Warm the record to Cached, peeking before every Evaluate: causes agree.
    for (int i = 0; i < 4; ++i)
    {
        const auto peeked = drv.Cache.PeekCause(kView, kSlot, in, false);
        const auto d = drv.FrameAt(drv.Stamp + 1, in);
        EXPECT_EQ(peeked, d.Cause) << "frame " << i;
    }
    ASSERT_EQ(drv.Cache.PeekCause(kView, kSlot, in, false), CascadeCacheDirtyCause::Cached);

    // Each single-field mutation resolves identically through both paths.
    {
        auto camera = in;
        camera.CameraViewProj[5] += 1.0f;
        EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, camera, false),
                  CascadeCacheDirtyCause::CameraChanged);
    }
    {
        auto fit = in;
        fit.LightVP[12] += 0.25f;
        EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, fit, false),
                  CascadeCacheDirtyCause::CascadeFitChanged);
    }
    {
        auto epoch = in;
        epoch.CasterEpoch += 1;
        EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, epoch, false),
                  CascadeCacheDirtyCause::CasterContentChanged);
    }

    // pendingUnderDraw stands in for a not-yet-consumed tracker mark: the peek
    // resolves ExecUnderDraw exactly as Evaluate will after MarkUnderDrew.
    EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, in, true),
              CascadeCacheDirtyCause::ExecUnderDraw);
    drv.Cache.MarkUnderDrew(kView, kSlot);
    EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, in, false),
              CascadeCacheDirtyCause::ExecUnderDraw);
    const auto statsBefore = drv.Cache.GetStats(kView).Evaluated;
    const auto d = drv.FrameAt(drv.Stamp + 1, in);
    EXPECT_EQ(d.Cause, CascadeCacheDirtyCause::ExecUnderDraw);
    EXPECT_EQ(drv.Cache.GetStats(kView).Evaluated, statsBefore + 1)
        << "peeks must not have counted as evaluations";
}


// These matrices describe the raster footprint, including its infinite depth
// extrusion. The tests exercise production attribution and cache decisions.
CascadeRenderInputs FootprintInputs(float halfExtent = 10.0f)
{
    auto in = BaseInputs();
    std::fill(std::begin(in.LightVP), std::end(in.LightVP), 0.0f);
    in.LightVP[0] = in.LightVP[5] = 1.0f / halfExtent;
    in.LightVP[10] = 0.01f;
    in.LightVP[15] = 1.0f;
    in.WorldId = 99;
    return in;
}

uint64_t Track(CacheDriver& drv, CascadeRenderInputs& in, const ShadowCasterChangeSet& changes,
                uint32_t slot = kSlot)
{
    in.CasterEpoch = drv.Cache.TrackCasterChanges(kView, slot, in.WorldId, changes,
                                                 in.LightVP, in.Resolution);
    return in.CasterEpoch;
}

void SettleSpatialCache(CacheDriver& drv, CascadeRenderInputs& in)
{
    Track(drv, in, {10, {}, true});
    drv.Frame(in);
    drv.Frame(in);
    ASSERT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowAttribution, DistantMoverRetainsNearCascadeButRefreshesFarCascade)
{
    CacheDriver near, far;
    auto nearIn = FootprintInputs(10.0f);
    auto farIn = FootprintInputs(100.0f);
    SettleSpatialCache(near, nearIn);
    SettleSpatialCache(far, farIn);
    const std::array spheres{ShadowCasterChangeSphere{40.0f, 0.0f, 0.0f, 2.0f}};
    EXPECT_EQ(Track(near, nearIn, {11, spheres, false}), 10u);
    EXPECT_EQ(Track(far, farIn, {11, spheres, false}), 11u);
    EXPECT_TRUE(near.Frame(nearIn).Skip);
    EXPECT_EQ(far.Frame(farIn).Cause, CascadeCacheDirtyCause::CasterContentChanged);
}

TEST(CascadeShadowAttribution, DepthClampedAndBoundaryCastersAlwaysInvalidate)
{
    for (const auto sphere : {ShadowCasterChangeSphere{0, 0, -1000000, 0.1f},
                              ShadowCasterChangeSphere{0, 0, 1000000, 0.1f},
                              ShadowCasterChangeSphere{11, 0, 0, 1},
                              ShadowCasterChangeSphere{0, -11, 0, 1},
                              // Centre departed, but old/new union still touches.
                              ShadowCasterChangeSphere{20, 0, 0, 11}})
    {
        CacheDriver drv;
        auto in = FootprintInputs();
        SettleSpatialCache(drv, in);
        const std::array spheres{sphere};
        EXPECT_EQ(Track(drv, in, {11, spheres, false}), 11u);
        EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CasterContentChanged);
    }
}

TEST(CascadeShadowAttribution, UsesTheLightAxesAndTranslation)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    // Looking along world X: screen X is world Z, centred on Z=100.
    in.LightVP[0] = in.LightVP[10] = 0.0f;
    in.LightVP[8] = 0.1f;
    in.LightVP[2] = 0.01f;
    in.LightVP[12] = -10.0f;
    SettleSpatialCache(drv, in);
    const std::array inside{ShadowCasterChangeSphere{100000, 0, 100, 1}};
    EXPECT_EQ(Track(drv, in, {11, inside, false}), 11u);
    EXPECT_FALSE(drv.Frame(in).Skip);
    const std::array outside{ShadowCasterChangeSphere{0, 0, 140, 1}};
    EXPECT_EQ(Track(drv, in, {12, outside, false}), 11u);
    EXPECT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowAttribution, MissedVersionsAndUnattributedChangesRefresh)
{
    for (const auto change : {ShadowCasterChangeSet{12, {}, false},
                              ShadowCasterChangeSet{11, {}, true},
                              ShadowCasterChangeSet{2, {}, false}})
    {
        CacheDriver drv;
        auto in = FootprintInputs();
        SettleSpatialCache(drv, in);
        Track(drv, in, change);
        EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CasterContentChanged);
    }
}

TEST(CascadeShadowAttribution, PlanningAndDeclarationCannotConsumeEachOthersChanges)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    SettleSpatialCache(drv, in);
    const std::array spheres{ShadowCasterChangeSphere{0, 0, 0, 1}};
    Track(drv, in, {11, spheres, false});
    EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, in, false),
              CascadeCacheDirtyCause::CasterContentChanged);
    Track(drv, in, {11, spheres, false});
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CasterContentChanged);
    EXPECT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowAttribution, IrrelevantLaterChangeCannotClearAnUnrenderedRelevantChange)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    SettleSpatialCache(drv, in);
    const std::array inside{ShadowCasterChangeSphere{0, 0, 0, 1}};
    const std::array outside{ShadowCasterChangeSphere{40, 0, 0, 1}};
    Track(drv, in, {11, inside, false}); // declaration/exec may never run
    EXPECT_EQ(Track(drv, in, {12, outside, false}), 11u);
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CasterContentChanged);
}

TEST(CascadeShadowAttribution, WorldRebindWithEqualEpochRefreshes)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    SettleSpatialCache(drv, in);
    ++in.WorldId;
    Track(drv, in, {10, {}, false});
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CasterContentChanged);
}

TEST(CascadeShadowAttribution, DepthAndTintSlotsObserveChangesIndependently)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    const uint32_t depth = CascadeShadowCache::kDepthSlotBase;
    const uint32_t tint = CascadeShadowCache::kTintSlotBase;
    Track(drv, in, {10, {}, true}, depth);
    Track(drv, in, {10, {}, true}, tint);
    const std::array spheres{ShadowCasterChangeSphere{0, 0, 0, 1}};
    EXPECT_EQ(Track(drv, in, {11, spheres, false}, depth), 11u);
    EXPECT_EQ(Track(drv, in, {11, spheres, false}, tint), 11u);
}

TEST(CascadeShadowAttribution, InvalidBoundsAndProjectionRefreshConservatively)
{
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(), -1.0f})
    {
        CacheDriver drv;
        auto in = FootprintInputs();
        SettleSpatialCache(drv, in);
        const std::array spheres{ShadowCasterChangeSphere{40, 0, 0, invalid}};
        EXPECT_EQ(Track(drv, in, {11, spheres, false}), 11u);
    }
    CacheDriver drv;
    auto in = FootprintInputs();
    SettleSpatialCache(drv, in);
    in.LightVP[0] = std::numeric_limits<float>::quiet_NaN();
    const std::array spheres{ShadowCasterChangeSphere{40, 0, 0, 1}};
    EXPECT_EQ(Track(drv, in, {11, spheres, false}), 11u);
}

TEST(CascadeShadowAttribution, FitChangesStillRefreshAfterIrrelevantMovement)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    SettleSpatialCache(drv, in);
    const std::array spheres{ShadowCasterChangeSphere{40, 0, 0, 1}};
    EXPECT_EQ(Track(drv, in, {11, spheres, false}), 10u);
    in.LightVP[12] = -4.0f; // move the cascade to include that caster
    Track(drv, in, {11, spheres, false});
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CascadeFitChanged);
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CullNotSettled);
    EXPECT_TRUE(drv.Frame(in).Skip);
}

TEST(CascadeShadowAttribution, MoverInRetainedFootprintCannotBecomeAMotionDeferral)
{
    CacheDriver drv;
    auto in = FootprintInputs();
    SettleSpatialCache(drv, in);
    in.LightVP[12] = -4.0f; // requested footprint moves to X=40
    const std::array spheres{ShadowCasterChangeSphere{0, 0, 0, 1}};
    // The mover misses the requested footprint but affects the retained map.
    // CasterContentChanged is mandatory; CascadeFitChanged could be deferred.
    EXPECT_EQ(Track(drv, in, {11, spheres, false}), 11u);
    EXPECT_EQ(drv.Cache.PeekCause(kView, kSlot, in, false),
              CascadeCacheDirtyCause::CasterContentChanged);
    EXPECT_EQ(drv.Frame(in).Cause, CascadeCacheDirtyCause::CasterContentChanged);
}

} // namespace
