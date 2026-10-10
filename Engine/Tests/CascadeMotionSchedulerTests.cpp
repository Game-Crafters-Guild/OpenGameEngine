// CascadeMotionSchedulerTests — the camera-motion round-robin budget
// (GE_SHADOW_MOTION_CAP lane). Pure CPU: no device, no RenderServices.
// Covers cap-off, correctness-class bypass (every dirty cause), nearest-first
// budget selection, the staleness bound (defer allowed only while content
// drift <= maxAge; forced renders displace nearer cascades and may exceed the
// budget), unknown-content protection, and the steady-state cycle the design
// promises for cap 2 / maxAge 2: {0,1},{0,1},{2,3} — 2 renders per frame with
// near cascades at most 1 frame stale and far cascades at most 2.
#include "Engine/Rendering/CascadeMotionScheduler.h"
#include "Engine/Rendering/CascadeShadowCache.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <iterator>

using GameEngine::Engine::Renderer::CascadeCacheDirtyCause;
using GameEngine::Engine::Renderer::CascadeMotionScheduler;
using GameEngine::Engine::Renderer::CascadeUpdateClass;
using GameEngine::Engine::Renderer::ClassifyCascadeDirtyCause;
using GameEngine::Engine::Renderer::ClassifyCascadePair;

namespace
{

constexpr uint32_t kView = 3;
constexpr uint32_t kN = CascadeMotionScheduler::kMaxCascades;
using Classes = std::array<CascadeUpdateClass, kN>;
using Staleness = std::array<uint64_t, kN>;

Classes AllMotion()
{
    Classes c{};
    c.fill(CascadeUpdateClass::Motion);
    return c;
}

// Drives consecutive frames the way the feature does: content stamps advance
// for rendered slots, deferred slots keep theirs, and staleness is recomputed
// from ground truth each frame.
struct SchedulerDriver
{
    CascadeMotionScheduler Scheduler;
    uint64_t Frame = 100; // arbitrary start
    std::array<uint64_t, kN> ContentStamp{};

    SchedulerDriver()
    {
        // All content freshly rendered on the frame before the first plan.
        ContentStamp.fill(Frame);
    }

    uint32_t Plan(const Classes& classes, uint32_t cap, uint32_t maxAge,
                  uint32_t numCascades = kN)
    {
        ++Frame;
        Staleness s{};
        for (uint32_t c = 0; c < kN; ++c)
            s[c] = Frame - ContentStamp[c];
        const uint32_t mask =
            Scheduler.PlanFrame(kView, numCascades, classes, s, cap, maxAge).DeferMask;
        for (uint32_t c = 0; c < numCascades; ++c)
        {
            // Rendered or cache-skipped (Cached == byte-identical content)
            // slots refresh their stamp; deferred slots keep it.
            if ((mask & (1u << c)) == 0)
                ContentStamp[c] = Frame;
        }
        return mask;
    }
};

TEST(CascadeMotionScheduler, CapZeroNeverDefers)
{
    SchedulerDriver drv;
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(drv.Plan(AllMotion(), /*cap=*/0, /*maxAge=*/2), 0u);
}

TEST(CascadeMotionScheduler, CorrectnessClassesNeverDefer)
{
    // Every non-motion cause maps to Correctness and must never defer, no
    // matter how tight the cap.
    const CascadeCacheDirtyCause correctness[] = {
        CascadeCacheDirtyCause::FirstRender,       CascadeCacheDirtyCause::ContributorPresent,
        CascadeCacheDirtyCause::ExecUnderDraw,     CascadeCacheDirtyCause::ContributorChanged,
        CascadeCacheDirtyCause::PhysicalChanged,   CascadeCacheDirtyCause::CasterContentChanged,
        CascadeCacheDirtyCause::ConfigChanged,     CascadeCacheDirtyCause::CullNotSettled,
    };
    for (const auto cause : correctness)
        EXPECT_EQ(ClassifyCascadeDirtyCause(cause), CascadeUpdateClass::Correctness);
    EXPECT_EQ(ClassifyCascadeDirtyCause(CascadeCacheDirtyCause::Cached),
              CascadeUpdateClass::Cached);
    EXPECT_EQ(ClassifyCascadeDirtyCause(CascadeCacheDirtyCause::CameraChanged),
              CascadeUpdateClass::Motion);
    EXPECT_EQ(ClassifyCascadeDirtyCause(CascadeCacheDirtyCause::CascadeFitChanged),
              CascadeUpdateClass::Motion);

    SchedulerDriver drv;
    Classes c{};
    c.fill(CascadeUpdateClass::Correctness);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(drv.Plan(c, /*cap=*/1, /*maxAge=*/2), 0u);
}

TEST(CascadeMotionScheduler, CachedSlotsNeitherDeferNorConsumeBudget)
{
    SchedulerDriver drv;
    Classes c{};
    c.fill(CascadeUpdateClass::Cached);
    c[3] = CascadeUpdateClass::Motion;
    // Budget 1 goes entirely to the single motion slot: nothing defers.
    EXPECT_EQ(drv.Plan(c, /*cap=*/1, /*maxAge=*/2), 0u);
}

TEST(CascadeMotionScheduler, NearestFirstWithinBudget)
{
    SchedulerDriver drv;
    // First motion frame, cap 2: cascades 0 and 1 render, 2 and 3 defer.
    EXPECT_EQ(drv.Plan(AllMotion(), /*cap=*/2, /*maxAge=*/2), 0b1100u);
}

TEST(CascadeMotionScheduler, SteadyStateCycleCap2MaxAge2)
{
    SchedulerDriver drv;
    // Documented steady state: {0,1},{0,1},{2,3} repeating — 2 renders per
    // frame, far cascades at most 2 frames stale, near at most 1.
    const uint32_t expected[] = {0b1100u, 0b1100u, 0b0011u,
                                 0b1100u, 0b1100u, 0b0011u,
                                 0b1100u, 0b1100u, 0b0011u};
    for (size_t i = 0; i < std::size(expected); ++i)
        EXPECT_EQ(drv.Plan(AllMotion(), /*cap=*/2, /*maxAge=*/2), expected[i]) << "frame " << i;
}

TEST(CascadeMotionScheduler, StalenessBoundHolds)
{
    SchedulerDriver drv;
    constexpr uint32_t kMaxAge = 2;
    for (int i = 0; i < 60; ++i)
    {
        const uint32_t mask = drv.Plan(AllMotion(), /*cap=*/2, kMaxAge);
        for (uint32_t c = 0; c < kN; ++c)
        {
            if (mask & (1u << c))
            {
                // A deferred slot's sampled content drift never exceeds maxAge.
                EXPECT_LE(drv.Frame - drv.ContentStamp[c], kMaxAge)
                    << "cascade " << c << " frame " << i;
            }
        }
        // The budget bounds motion renders on every non-forced frame; forced
        // frames may displace but never exceed the slot count.
        const uint32_t renders = kN - std::popcount(mask);
        EXPECT_LE(renders, kN);
    }
}

TEST(CascadeMotionScheduler, ForcedRendersDisplaceNearCascades)
{
    SchedulerDriver drv;
    // Reach the forced frame of the cap-2 cycle: frames 1-2 defer {2,3};
    // frame 3 forces them and displaces {0,1}.
    drv.Plan(AllMotion(), 2, 2);
    drv.Plan(AllMotion(), 2, 2);
    const uint32_t mask = drv.Plan(AllMotion(), 2, 2);
    EXPECT_EQ(mask, 0b0011u); // 0 and 1 deferred; 2 and 3 render (forced)
}

TEST(CascadeMotionScheduler, ForcedBurstMayExceedCap)
{
    SchedulerDriver drv;
    // cap 1: cascade 0 monopolizes the budget until 1..3 all hit the bound,
    // then ALL of them render the same frame — the staleness bound is the
    // contract, the cap is best-effort.
    EXPECT_EQ(drv.Plan(AllMotion(), 1, 2), 0b1110u);
    EXPECT_EQ(drv.Plan(AllMotion(), 1, 2), 0b1110u);
    const uint32_t mask = drv.Plan(AllMotion(), 1, 2);
    EXPECT_EQ(mask & 0b1110u, 0u) << "all three stale cascades must render";
    const auto& stats = drv.Scheduler.GetStats(kView);
    EXPECT_GT(stats.ForcedByAge, 0u);
    EXPECT_GT(stats.ForcedBeyondCap, 0u);
}

TEST(CascadeMotionScheduler, UnknownContentNeverDefers)
{
    CascadeMotionScheduler sched;
    Staleness s{};
    s.fill(CascadeMotionScheduler::kStalenessUnknown);
    // No committed content (cold start, post-invalidate, hidden-view regrow):
    // everything renders regardless of the cap.
    const auto plan = sched.PlanFrame(kView, kN, AllMotion(), s, /*cap=*/1, /*maxAge=*/2);
    EXPECT_EQ(plan.DeferMask, 0u);
}

TEST(CascadeMotionScheduler, StaleContentAfterGapForcesRender)
{
    SchedulerDriver drv;
    // Simulate a view hidden for 50 frames: stamps freeze, frame advances.
    drv.Frame += 50;
    // Content is 51 frames old on reappearance — nothing may defer against it.
    EXPECT_EQ(drv.Plan(AllMotion(), /*cap=*/2, /*maxAge=*/2), 0u);
    // The very next frame the cycle re-arms normally.
    EXPECT_EQ(drv.Plan(AllMotion(), /*cap=*/2, /*maxAge=*/2), 0b1100u);
}

TEST(CascadeMotionScheduler, ReducedCascadeCountIgnoresUpperSlots)
{
    SchedulerDriver drv;
    // Two active cascades and cap 2: nothing defers, upper slots untouched.
    EXPECT_EQ(drv.Plan(AllMotion(), /*cap=*/2, /*maxAge=*/2, /*numCascades=*/2), 0u);
}

TEST(CascadeMotionScheduler, DeferredStatsAccumulate)
{
    SchedulerDriver drv;
    drv.Plan(AllMotion(), 2, 2);
    const auto& stats = drv.Scheduler.GetStats(kView);
    EXPECT_EQ(stats.Deferred, 2u);
    EXPECT_EQ(drv.Scheduler.GetStats(kView + 99).Deferred, 0u); // unknown view = zero stats
}

TEST(CascadeMotionScheduler, TintPairLockstepGate)
{
    // Transmission-visibility gap, then reappearance: the tint layer's
    // retained content is from an old fit while the depth family kept
    // rendering (fresh stamp). The depth staleness bound says nothing about
    // the tint layer, so a desynced Motion pair must classify Correctness
    // (render both) — never defer.
    const auto camera = CascadeCacheDirtyCause::CameraChanged;
    const auto fit = CascadeCacheDirtyCause::CascadeFitChanged;
    const auto cached = CascadeCacheDirtyCause::Cached;

    // R1 variant A: both families Motion, content fits desynced.
    EXPECT_EQ(ClassifyCascadePair(camera, &camera, /*contentFitsLockstep=*/false),
              CascadeUpdateClass::Correctness);
    EXPECT_EQ(ClassifyCascadePair(fit, &camera, false), CascadeUpdateClass::Correctness);

    // R1 variant B: still camera — depth Cached (record matches current),
    // tint Motion (its record predates the gap). Pair severity is Motion;
    // the desync promotes it to Correctness.
    EXPECT_EQ(ClassifyCascadePair(cached, &camera, false), CascadeUpdateClass::Correctness);

    // Lockstep pairs stay deferrable.
    EXPECT_EQ(ClassifyCascadePair(camera, &camera, true), CascadeUpdateClass::Motion);
    EXPECT_EQ(ClassifyCascadePair(cached, &camera, true), CascadeUpdateClass::Motion);

    // Correctness on either family dominates regardless of lockstep.
    const auto under = CascadeCacheDirtyCause::ExecUnderDraw;
    EXPECT_EQ(ClassifyCascadePair(camera, &under, true), CascadeUpdateClass::Correctness);
    EXPECT_EQ(ClassifyCascadePair(under, &camera, true), CascadeUpdateClass::Correctness);

    // Both Cached: lockstep holds by construction (both records byte-equal
    // the same current inputs) — the pair stays Cached.
    EXPECT_EQ(ClassifyCascadePair(cached, &cached, true), CascadeUpdateClass::Cached);

    // No tint family: depth-only classification passes through untouched.
    EXPECT_EQ(ClassifyCascadePair(camera, nullptr, false), CascadeUpdateClass::Motion);
    EXPECT_EQ(ClassifyCascadePair(cached, nullptr, false), CascadeUpdateClass::Cached);
    EXPECT_EQ(ClassifyCascadePair(under, nullptr, false), CascadeUpdateClass::Correctness);
}

} // namespace
