// The per-view temporal history rotates one sample — unjittered camera plus the
// two deformation clock lanes — per rendered frame, keyed on the frame stamp
// changing AND on the frame that produced the outgoing sample having reached
// submission (RGFrame::SubmittedFrameCount moved). These rows pin the contract
// the temporal consumers and the motion pass rely on, with no device involved: the previous sample is the one THIS
// view last rendered with (not last frame's, not the current clock minus a
// delta), the first rendered frame reports no previous sample, later callers
// within one frame do not rotate, a released view starts over, a declared and
// abandoned frame never becomes a later frame's previous, and the deformation
// lane is rebased against one origin every view shares, which only moves where
// the motion history is already invalid.

#include "Engine/Rendering/ViewTemporalHistory.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

using GameEngine::Engine::Renderer::kDeformationOriginCeilingSeconds;
using GameEngine::Engine::Renderer::ViewDeformationClock;
using GameEngine::Engine::Renderer::ViewTemporalHistory;
using GameEngine::Engine::Renderer::ViewTemporalSample;
using GameEngine::Rendering::CameraData;
using GameEngine::Rendering::ViewId;

namespace
{
constexpr ViewId kView = static_cast<ViewId>(7);
constexpr ViewId kOtherView = static_cast<ViewId>(11);
constexpr float kScrollPerSecond = 0.25f;
// The submitted-frame count a declare of `frameIndex` reads in a stream where
// every earlier frame was submitted: it has moved since the previous declare,
// which is all the rotation compares. Every row whose subject is not the gate
// passes this.
constexpr GameEngine::uint64 Submitted(GameEngine::uint64 frameIndex) { return frameIndex - 1; }

// A sample whose camera and both clock lanes are all derived from one tag, so
// any lane copied from the wrong frame is visible on its own.
ViewTemporalSample MakeSample(float tag)
{
    ViewTemporalSample sample{};
    for (int i = 0; i < 16; ++i)
        sample.Camera.viewProj[i] = tag * 100.0f + static_cast<float>(i);
    sample.Camera.cameraPos[0] = tag;
    sample.DeformationTimeSeconds = tag;
    sample.DeformationScrollSeconds = tag * kScrollPerSecond;
    return sample;
}

bool SameSample(const ViewTemporalSample& a, const ViewTemporalSample& b)
{
    return std::memcmp(&a.Camera, &b.Camera, sizeof(CameraData)) == 0 &&
           a.DeformationTimeSeconds == b.DeformationTimeSeconds &&
           a.DeformationScrollSeconds == b.DeformationScrollSeconds &&
           a.DeformationOrigin == b.DeformationOrigin;
}
} // namespace

TEST(ViewTemporalHistory, FirstRenderedFrameHasNoPreviousSample)
{
    ViewTemporalHistory history;
    const ViewTemporalSample current = MakeSample(1.0f);

    bool previousValid = true;
    const ViewTemporalSample* previous = history.Advance(kView, 1, current, Submitted(1), &previousValid);

    ASSERT_NE(previous, nullptr);
    EXPECT_FALSE(previousValid);
    // Zero motion and zero clock delta: the current sample stands in for the previous.
    EXPECT_TRUE(SameSample(*previous, current));
}

// Design row T9: a view renders, lapses for many frames while the frame index
// and the clock keep advancing, and resumes. The previous sample must be the
// one the view last rendered with, in every lane.
TEST(ViewTemporalHistory, ResumedViewGetsTheSampleItLastRenderedWith)
{
    ViewTemporalHistory history;
    const ViewTemporalSample frameOne = MakeSample(1.0f);
    const ViewTemporalSample frameTwo = MakeSample(1.5f);
    history.Advance(kView, 1, frameOne, Submitted(1));
    history.Advance(kView, 2, frameTwo, Submitted(2));

    // Forty frames pass in which this view does not render (hidden tab); the
    // clock is now 9.0 s and the stream counter jumped to 42.
    const ViewTemporalSample resumed = MakeSample(9.0f);
    bool previousValid = false;
    const ViewTemporalSample* previous = history.Advance(kView, 42, resumed, Submitted(42), &previousValid);

    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, frameTwo));
    EXPECT_FLOAT_EQ(previous->DeformationTimeSeconds, 1.5f);
    EXPECT_FLOAT_EQ(previous->DeformationScrollSeconds, 1.5f * kScrollPerSecond);
    // Neither the current clock nor a one-frame-delta guess from it.
    EXPECT_NE(previous->DeformationTimeSeconds, resumed.DeformationTimeSeconds);
    EXPECT_NE(previous->DeformationTimeSeconds, resumed.DeformationTimeSeconds - 0.5f);
    EXPECT_EQ(std::memcmp(previous->Camera.viewProj, frameTwo.Camera.viewProj,
                          sizeof(frameTwo.Camera.viewProj)),
              0);
}

TEST(ViewTemporalHistory, LaterCallersInTheSameFrameDoNotRotate)
{
    ViewTemporalHistory history;
    const ViewTemporalSample frameOne = MakeSample(1.0f);
    const ViewTemporalSample frameTwo = MakeSample(2.0f);
    const ViewTemporalSample frameTwoOtherCaller = MakeSample(2.5f);
    const ViewTemporalSample frameThree = MakeSample(3.0f);
    history.Advance(kView, 1, frameOne, Submitted(1));

    bool previousValid = false;
    const ViewTemporalSample* previous = history.Advance(kView, 2, frameTwo, Submitted(2), &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, frameOne));

    // Second consumer of the same rendered frame: same pair, no rotation.
    previousValid = false;
    previous = history.Advance(kView, 2, frameTwoOtherCaller, Submitted(2), &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, frameOne));

    // The next rendered frame pairs with the FIRST caller's sample for frame 2.
    previous = history.Advance(kView, 3, frameThree, Submitted(3), &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, frameTwo));
}

TEST(ViewTemporalHistory, ViewsRotateIndependently)
{
    ViewTemporalHistory history;
    history.Advance(kView, 1, MakeSample(1.0f), Submitted(1));
    history.Advance(kOtherView, 1, MakeSample(10.0f), Submitted(1));
    history.Advance(kOtherView, 2, MakeSample(11.0f), Submitted(2));
    history.Advance(kOtherView, 3, MakeSample(12.0f), Submitted(3));

    bool previousValid = false;
    const ViewTemporalSample* previous =
        history.Advance(kView, 4, MakeSample(4.0f), Submitted(4), &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, MakeSample(1.0f)));
}

// A released view (and a fresh history, which is what a RenderServices rebuild
// after device loss creates) has no previous rendered frame to pair with.
TEST(ViewTemporalHistory, ReleasedViewStartsOver)
{
    ViewTemporalHistory history;
    history.Advance(kView, 1, MakeSample(1.0f), Submitted(1));
    history.Advance(kView, 2, MakeSample(2.0f), Submitted(2));

    history.ReleaseView(kView);

    const ViewTemporalSample afterRelease = MakeSample(3.0f);
    bool previousValid = true;
    const ViewTemporalSample* previous =
        history.Advance(kView, 3, afterRelease, Submitted(3), &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_FALSE(previousValid);
    EXPECT_TRUE(SameSample(*previous, afterRelease));

    previous = history.Advance(kView, 4, MakeSample(4.0f), Submitted(4), &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, afterRelease));
}

// ── The declared-but-unsubmitted gate ─────────────────────────────────────────
// Every Advance call site is a declare path, and a host can abandon a frame
// after the graph is built. The frame stream's submitted-frame count
// (RGFrame::SubmittedFrameCount) is the evidence that a declared frame reached
// submission; the sequence below is its own control, because the count advances
// around the abandoned frame.

TEST(ViewTemporalHistory, DeclaredButUnsubmittedFrameNeverBecomesTheNextFramesPrevious)
{
    ViewTemporalHistory history;
    GameEngine::uint64 submitted = 0;

    // Frame 1 declared and submitted.
    history.Advance(kView, 1, MakeSample(1.0f), submitted);
    ++submitted;

    // Frame 2 declared and submitted: frame 1 becomes previous.
    bool previousValid = false;
    const ViewTemporalSample* previous =
        history.Advance(kView, 2, MakeSample(2.0f), submitted, &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, MakeSample(1.0f)));
    ++submitted;

    // Frame 3 declared, then abandoned — the count does not move.
    previous = history.Advance(kView, 3, MakeSample(3.0f), submitted, &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, MakeSample(2.0f)));

    // Frame 4 declared and submitted. Its previous is the last SUBMITTED frame's
    // sample — frame 2 — not the abandoned frame 3's, which never reached a pixel.
    previous = history.Advance(kView, 4, MakeSample(4.0f), submitted, &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, MakeSample(2.0f)))
        << "the abandoned frame's sample became a later frame's previous";
    ++submitted;

    // And rotation resumes on the frame after a submitted one, so the step above
    // is the gate rather than a history that stopped rotating.
    previous = history.Advance(kView, 5, MakeSample(5.0f), submitted, &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, MakeSample(4.0f)));
}

// An abandoned FIRST frame leaves the view with no previous at all rather than
// with a sample that never rendered.
TEST(ViewTemporalHistory, AbandonedFirstFrameLeavesNoPrevious)
{
    ViewTemporalHistory history;
    const GameEngine::uint64 submitted = 0;

    bool previousValid = true;
    history.Advance(kView, 1, MakeSample(1.0f), submitted, &previousValid);
    EXPECT_FALSE(previousValid);

    const ViewTemporalSample frameTwo = MakeSample(2.0f);
    const ViewTemporalSample* previous =
        history.Advance(kView, 2, frameTwo, submitted, &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_FALSE(previousValid);
    EXPECT_TRUE(SameSample(*previous, frameTwo));

    // Frame 2 is submitted; frame 3 pairs with it.
    previous = history.Advance(kView, 3, MakeSample(3.0f), 1ull, &previousValid);
    ASSERT_NE(previous, nullptr);
    EXPECT_TRUE(previousValid);
    EXPECT_TRUE(SameSample(*previous, frameTwo));
}

// ── The rebased deformation clock ─────────────────────────────────────────────

TEST(ViewTemporalHistory, DeformationOriginAnchorsOnceAndHoldsAcrossFrames)
{
    ViewTemporalHistory history;
    constexpr double kUptime = 12345.678;
    constexpr double kFrame = 1.0 / 60.0;

    const ViewDeformationClock first = history.ResolveDeformationClock(kUptime);
    EXPECT_DOUBLE_EQ(first.Origin, kUptime);
    EXPECT_FLOAT_EQ(first.TimeSeconds, 0.0f);

    // Same application frame, second caller: identical answer, so the light
    // buffers and the samples cannot disagree about the lane.
    const ViewDeformationClock again = history.ResolveDeformationClock(kUptime);
    EXPECT_DOUBLE_EQ(again.Origin, first.Origin);
    EXPECT_FLOAT_EQ(again.TimeSeconds, first.TimeSeconds);

    const ViewDeformationClock next = history.ResolveDeformationClock(kUptime + kFrame);
    EXPECT_DOUBLE_EQ(next.Origin, kUptime) << "an ordinary frame must not re-anchor";
    EXPECT_FLOAT_EQ(next.TimeSeconds, static_cast<float>(kFrame));
}

// One origin for every view: a view opened later adopts it rather than
// anchoring its own, so two views of one deforming surface see it at the same
// point in its wave, and the later view's first frame — which has no previous
// anyway — is where it joins.
TEST(ViewTemporalHistory, AViewOpenedLaterAdoptsTheSharedDeformationOrigin)
{
    ViewTemporalHistory history;
    ASSERT_DOUBLE_EQ(history.ResolveDeformationClock(1000.0).Origin, 1000.0);
    history.Advance(kView, 1, MakeSample(1.0f), Submitted(1));

    // 600 s later a second view renders its first frame.
    const ViewDeformationClock later = history.ResolveDeformationClock(1600.0);
    EXPECT_DOUBLE_EQ(later.Origin, 1000.0) << "a new view must not re-anchor the shared origin";
    EXPECT_FLOAT_EQ(later.TimeSeconds, 600.0f);
    bool previousValid = true;
    history.Advance(kOtherView, 40, MakeSample(40.0f), Submitted(40), &previousValid);
    EXPECT_FALSE(previousValid);
}

TEST(ViewTemporalHistory, ReleasedViewLeavesTheSharedDeformationOriginAlone)
{
    ViewTemporalHistory history;
    ASSERT_DOUBLE_EQ(history.ResolveDeformationClock(500.0).Origin, 500.0);
    history.Advance(kView, 1, MakeSample(1.0f), Submitted(1));
    history.Advance(kOtherView, 1, MakeSample(10.0f), Submitted(1));

    history.ReleaseView(kView);

    // The other view keeps its phase: nothing a view does on its own may move
    // every other view's wave.
    const ViewDeformationClock afterRelease = history.ResolveDeformationClock(900.0);
    EXPECT_DOUBLE_EQ(afterRelease.Origin, 500.0);
    EXPECT_FLOAT_EQ(afterRelease.TimeSeconds, 400.0f);
}

TEST(ViewTemporalHistory, DeformationOriginReanchorsAtThePrecisionCeiling)
{
    ViewTemporalHistory history;
    constexpr double kAnchor = 10000.0;
    ASSERT_DOUBLE_EQ(history.ResolveDeformationClock(kAnchor).Origin, kAnchor);

    // Exactly at the ceiling is still the same epoch.
    const ViewDeformationClock atCeiling =
        history.ResolveDeformationClock(kAnchor + kDeformationOriginCeilingSeconds);
    EXPECT_DOUBLE_EQ(atCeiling.Origin, kAnchor);
    EXPECT_FLOAT_EQ(atCeiling.TimeSeconds, static_cast<float>(kDeformationOriginCeilingSeconds));

    // One frame past it re-anchors, and the lane restarts at zero.
    const double past = kAnchor + kDeformationOriginCeilingSeconds + 1.0 / 60.0;
    const ViewDeformationClock reanchored = history.ResolveDeformationClock(past);
    EXPECT_DOUBLE_EQ(reanchored.Origin, past);
    EXPECT_FLOAT_EQ(reanchored.TimeSeconds, 0.0f);
}

// The re-anchor's phase discontinuity is only safe because the frame it lands on
// is already invalid, and what says so downstream is the origin on the rotated
// sample: two samples formed against different origins are not differenceable.
// The origin is shared, so the ceiling re-anchor marks EVERY view's next pair —
// not only the view whose caller happened to cross the ceiling first.
TEST(ViewTemporalHistory, ACeilingReanchorMarksEveryViewsNextPairAsUndifferenceable)
{
    ViewTemporalHistory history;
    constexpr double kAnchor = 10000.0;
    constexpr double kFrame = 1.0 / 60.0;

    const ViewDeformationClock before = history.ResolveDeformationClock(kAnchor);
    for (const ViewId view : {kView, kOtherView})
    {
        ViewTemporalSample frameOne = MakeSample(1.0f);
        frameOne.DeformationTimeSeconds = before.TimeSeconds;
        frameOne.DeformationOrigin = before.Origin;
        history.Advance(view, 1, frameOne, Submitted(1));
    }

    const double past = kAnchor + kDeformationOriginCeilingSeconds + kFrame;
    const ViewDeformationClock after = history.ResolveDeformationClock(past);
    ASSERT_NE(after.Origin, before.Origin);
    for (const ViewId view : {kView, kOtherView})
    {
        ViewTemporalSample frameTwo = MakeSample(2.0f);
        frameTwo.DeformationTimeSeconds = after.TimeSeconds;
        frameTwo.DeformationOrigin = after.Origin;

        bool previousValid = false;
        const ViewTemporalSample* previous =
            history.Advance(view, 2, frameTwo, Submitted(2), &previousValid);
        ASSERT_NE(previous, nullptr);
        // The camera pair is untouched — the re-anchor invalidates the
        // deformation endpoints, not the reprojection the temporal consumers
        // run on every frame.
        EXPECT_TRUE(previousValid);
        EXPECT_NE(previous->DeformationOrigin, frameTwo.DeformationOrigin)
            << "a re-anchored frame must be distinguishable from its previous, or a consumer "
               "would difference two clocks measured from different zeros";

        // The frame after pairs two samples of the new epoch again.
        ViewTemporalSample frameThree = MakeSample(3.0f);
        frameThree.DeformationOrigin = after.Origin;
        previous = history.Advance(view, 3, frameThree, Submitted(3), &previousValid);
        EXPECT_TRUE(previousValid);
        EXPECT_EQ(previous->DeformationOrigin, frameThree.DeformationOrigin);
    }
}

// ── T26 ───────────────────────────────────────────────────────────────────────
// The differenced motion survives long uptime. Pure arithmetic, no device: the
// two endpoints of one frame are formed exactly as the motion variant forms
// them, at simulated uptimes of 1 h, 18.2 h, 72.8 h and 3 days, for both
// representations. The rebased pair differences to the frame delta within the
// fp32 spacing at the OFFSET — which the ceiling bounds, so it does not grow
// with uptime — while the absolute fp32 lane quantises the delta and, at
// 2^18 s, rounds both endpoints to one value and reads exactly zero.
TEST(ViewTemporalHistory, RebasedEndpointDifferencingSurvivesLongUptimeWhereAbsoluteDoesNot)
{
    constexpr double kFrame = 1.0 / 60.0;
    constexpr double kUptimes[] = {3600.0, 65520.0, 262080.0, 259200.0}; // 1 h, 18.2 h, 72.8 h, 3 d
    // How far into its 2^12 s epoch the history is when the pair is taken. Zero is
    // the frame after a re-anchor; the second is the worst case the ceiling
    // admits, so no uptime and no epoch position can do worse than it.
    constexpr double kOffsets[] = {0.0, kDeformationOriginCeilingSeconds - kFrame};

    for (const double uptime : kUptimes)
    {
        // The same arithmetic on the absolute cumulative clock, which is what
        // this lane carried before the rebase.
        const double absoluteDelta = static_cast<double>(static_cast<float>(uptime))
                                     - static_cast<double>(static_cast<float>(uptime - kFrame));
        if (uptime >= 65520.0)
        {
            // Measured: 6.25 % of a frame at each of 18.2 h, 72.8 h and 3 days,
            // against 0.39 % at 1 h. The threshold sits under the measurement so
            // the row fails loudly if the lane's arithmetic changes.
            EXPECT_GT(std::abs(absoluteDelta - kFrame), 0.05 * kFrame)
                << "the absolute lane is expected to have degraded by " << uptime
                << " s of uptime; if it has not, the premise of the rebase has changed";
        }

        for (const double offset : kOffsets)
        {
            ViewTemporalHistory history;
            // Anchor where this epoch started, then take the two endpoints of
            // one frame at `uptime` — exactly as a frame does: the previous
            // endpoint comes from the sample the previous frame stored against
            // the same origin.
            const double origin = uptime - kFrame - offset;
            ASSERT_DOUBLE_EQ(history.ResolveDeformationClock(origin).Origin, origin);
            const ViewDeformationClock previous =
                history.ResolveDeformationClock(uptime - kFrame);
            const ViewDeformationClock current = history.ResolveDeformationClock(uptime);
            ASSERT_DOUBLE_EQ(previous.Origin, current.Origin)
                << "both endpoints of a frame must share one origin";
            ASSERT_DOUBLE_EQ(current.Origin, origin) << "the pair must not have re-anchored";

            const double rebasedDelta =
                static_cast<double>(current.TimeSeconds) - static_cast<double>(previous.TimeSeconds);
            // fp32 spacing at the CURRENT endpoint's offset, which the ceiling
            // bounds at 2^12 s regardless of uptime — that bound is the whole
            // point. Two of them covers the rounding of each endpoint.
            const double spacingAtOffset =
                std::max(offset + kFrame, 1.0) * std::pow(2.0, -23.0);
            EXPECT_NEAR(rebasedDelta, kFrame, 2.0 * spacingAtOffset)
                << "rebased lane lost the frame delta at uptime " << uptime << " s, offset "
                << offset << " s";

            if (uptime >= 65520.0)
                EXPECT_LT(std::abs(rebasedDelta - kFrame), std::abs(absoluteDelta - kFrame))
                    << "the rebased lane must be strictly better than the absolute one here";
        }
    }
}

// The absolute lane's failure is not gradual noise at the top end: past 2^18 s
// of uptime its spacing (31.25 ms) exceeds a frame's delta, so for roughly half
// the uptimes in that binade both endpoints round to one float and the
// differenced deformation reads exactly zero — a silent return to the defect
// this arc exists to fix — and for the rest it reads 31.25 ms, nearly double.
// Below 2^18 s the spacing is at most 15.625 ms, so the delta can never be
// zero there. The rebased lane at the same uptime is unaffected, because its
// magnitude is the offset and not the uptime.
TEST(ViewTemporalHistory, AbsoluteEndpointsRoundTogetherAtLongUptimeAndTheRebasedPairDoesNot)
{
    constexpr double kFrame = 1.0 / 60.0;
    constexpr double kUptime = 262144.01; // just past 2^18 s, where the spacing is 2^-5 s

    const double absoluteDelta = static_cast<double>(static_cast<float>(kUptime))
                                 - static_cast<double>(static_cast<float>(kUptime - kFrame));
    EXPECT_EQ(absoluteDelta, 0.0)
        << "both absolute endpoints were expected to round to one float here";

    ViewTemporalHistory history;
    const double origin = kUptime - kFrame;
    ASSERT_DOUBLE_EQ(history.ResolveDeformationClock(origin).Origin, origin);
    const ViewDeformationClock previous = history.ResolveDeformationClock(kUptime - kFrame);
    const ViewDeformationClock current = history.ResolveDeformationClock(kUptime);
    const double rebasedDelta =
        static_cast<double>(current.TimeSeconds) - static_cast<double>(previous.TimeSeconds);
    EXPECT_NEAR(rebasedDelta, kFrame, 2.0 * std::pow(2.0, -23.0));
}
