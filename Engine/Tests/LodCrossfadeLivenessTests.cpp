#include <gtest/gtest.h>

#include "Engine/Rendering/LodCrossfadeLiveness.h"

using GameEngine::Engine::Renderer::LodCrossfadeLivenessState;
using GameEngine::Engine::Renderer::LodCrossfadeTailObservation;
using GameEngine::Engine::Renderer::ResolveCrossfadeLiveness;

namespace
{
constexpr float kDuration = 0.25f; // the shipped default when the feature is on

// Drives the rule the way the frame spine does, and models the two lags that
// make this rule non-obvious:
//   - the tail census is reduced from a mirror written by an EARLIER dispatch,
//     so `Dispatch` records what the scatter wrote but does not publish it;
//   - `Reduce` is what publishes it, stamped with the recompute count at that
//     moment. A recompute landing between the two is exactly the case a naive
//     "just read the tail count" rule gets wrong.
class Spine
{
public:
    // A scatter dispatch whose gate found changed inputs: it may start fades.
    void DispatchWithRecompute(uint32_t tailRecords)
    {
        ++m_RecomputeCount;
        m_PendingTail = tailRecords;
    }
    // A dispatch forced by suppression alone. Its gate resolved `Skipped`, which
    // is not a recompute cause, so the count does not move.
    void DispatchWithoutRecompute(uint32_t tailRecords) { m_PendingTail = tailRecords; }

    // BeginArenaFrame's reduce: publish the newest dispatch's census, stamped.
    void Reduce()
    {
        m_Observation.Valid          = true;
        m_Observation.TailRecords    = m_PendingTail;
        m_Observation.RecomputeCount = m_RecomputeCount;
    }
    // A reduce that could not run this frame publishes nothing and leaves the
    // previous reading in place — the producer retains it because the stamp, not
    // the age, is what makes a census trustworthy.
    void ReduceUnavailable() {}
    // A structural invalidation: no slice reduced, or a tail-capable slice fell
    // past the stats array so its tail cursors cannot be read at all.
    void ReduceStructurallyInvalid() { m_Observation = LodCrossfadeTailObservation{}; }

    bool Step(float duration = kDuration)
    {
        return ResolveCrossfadeLiveness(duration, m_Observation, m_RecomputeCount, ++m_Frame,
                                        m_State);
    }
    // A second push of the elision context inside the SAME frame.
    bool Repeat(float duration = kDuration)
    {
        return ResolveCrossfadeLiveness(duration, m_Observation, m_RecomputeCount, m_Frame,
                                        m_State);
    }

private:
    uint64_t                   m_Frame          = 0;
    uint64_t                   m_RecomputeCount = 0;
    uint32_t                   m_PendingTail    = 0;
    LodCrossfadeTailObservation m_Observation{};
    LodCrossfadeLivenessState   m_State{};
};
} // namespace

// Feature off is the byte-identical contract: nothing is ever live, so the
// elision suppression never fires however the census reads.
TEST(LodCrossfadeLiveness, DurationZeroIsNeverLive)
{
    Spine s;
    s.DispatchWithRecompute(4096);
    s.Reduce();
    EXPECT_FALSE(s.Step(0.0f));
    EXPECT_FALSE(s.Step(-1.0f));
}

// A populated tail is a transition mid-dissolve. Eliding here is what freezes
// the record pair permanently.
TEST(LodCrossfadeLiveness, PopulatedTailIsLive)
{
    Spine s;
    s.DispatchWithRecompute(678);
    s.Reduce();
    EXPECT_TRUE(s.Step());
}

// The whole point of the rule: a settle that started no transition releases
// elision on the observation instead of waiting out a fade duration.
TEST(LodCrossfadeLiveness, EmptyTailFromTheCurrentDispatchIsNotLive)
{
    Spine s;
    s.DispatchWithRecompute(0);
    s.Reduce();
    EXPECT_FALSE(s.Step());
}

// THE regression this rule's staleness clause exists for. The census is reduced
// from a mirror one dispatch behind, so a recompute landing after the reading
// was taken may have started a fade the reading cannot see. Trusting the stale
// zero elides the scatter with a live pair retained, which freezes it at a
// weight that never hands over -- every fading instance drawn twice forever,
// plus a standing stipple of the outgoing level.
TEST(LodCrossfadeLiveness, StaleZeroIsNotTrustedAcrossANewRecompute)
{
    Spine s;
    s.DispatchWithRecompute(0);
    s.Reduce();
    ASSERT_FALSE(s.Step()) << "settled scene must go quiet";

    // Camera moves: this dispatch starts transitions, but its census has not
    // been reduced yet -- the observation still holds the previous zero.
    s.DispatchWithRecompute(512);
    EXPECT_TRUE(s.Step()) << "trusted a census taken before the newest recompute; "
                             "the pair started by that dispatch would freeze";

    // Once the census catches up it reports the fade honestly.
    s.Reduce();
    EXPECT_TRUE(s.Step());
}

// Suppression must not re-arm itself. The dispatches it forces resolve their
// gate as `Skipped`, which is not a recompute cause, so the stamp stops moving
// and the rule releases the moment an empty census arrives.
TEST(LodCrossfadeLiveness, SuppressionForcedDispatchesDoNotLatchTheWindow)
{
    Spine s;
    s.DispatchWithRecompute(512);
    s.Reduce();
    ASSERT_TRUE(s.Step());

    // The fade runs out over several suppression-forced dispatches.
    for (int i = 0; i < 4; ++i)
    {
        s.DispatchWithoutRecompute(512);
        s.Reduce();
        EXPECT_TRUE(s.Step()) << "fade still live at iteration " << i;
    }

    s.DispatchWithoutRecompute(0); // the dispatch that retires every pair
    s.Reduce();
    EXPECT_FALSE(s.Step()) << "window never closed -- suppression re-armed itself";
}

// No reading is not a zero reading. A census the producer had to throw away
// leaves the rule with nothing to judge, and the only safe answer is "a fade may
// be live".
TEST(LodCrossfadeLiveness, AnAbsentCensusIsTreatedAsLive)
{
    Spine s;
    s.DispatchWithRecompute(0);
    s.Reduce();
    ASSERT_FALSE(s.Step());

    s.ReduceStructurallyInvalid();
    EXPECT_TRUE(s.Step()) << "an absent census must fail safe, not read as settled";

    s.Reduce();
    EXPECT_FALSE(s.Step());
}

// The contract the producer's retention rests on. A census is trusted for its
// STAMP, not its age: while no dispatch has recomputed since it was taken, every
// dispatch in between had byte-identical inputs and so started no transition, and
// the reading still describes them. This is what lets a reduce that simply could
// not run this frame — the fence poll finding the previous frame's graphics work
// not yet signalled — leave the reading standing instead of forcing a dispatch.
// Discarding it there cost idle elision on ~40% of otherwise-settled frames.
TEST(LodCrossfadeLiveness, ARetainedCensusStaysTrustedWhileItsStampHolds)
{
    Spine s;
    s.DispatchWithRecompute(0);
    s.Reduce();
    ASSERT_FALSE(s.Step()) << "settled scene must go quiet";

    // Frame after frame the reduce publishes nothing. Nothing dispatched either,
    // so the stamp still covers every dispatch that has happened.
    for (int i = 0; i < 8; ++i)
    {
        s.ReduceUnavailable();
        EXPECT_FALSE(s.Step()) << "retained census abandoned at iteration " << i
                               << "; elision would drop for a reason unrelated to fading";
    }

    // The moment a dispatch recomputes, the stamp no longer covers it and the
    // retained reading must stop being trusted.
    s.DispatchWithRecompute(0);
    s.ReduceUnavailable();
    EXPECT_TRUE(s.Step()) << "a retained census outlived its stamp and was still trusted";
}

// The frame spine pushes the elision context once per scatter CALL -- owner
// spine, each utility view, then phase B. One verdict per frame: a call that
// recomputes mid-frame must not flip the answer the earlier calls acted on.
TEST(LodCrossfadeLiveness, RepeatCallsWithinAFrameReplayRatherThanAdvance)
{
    Spine s;
    s.DispatchWithRecompute(0);
    s.Reduce();
    ASSERT_FALSE(s.Step()) << "owner-spine call";

    s.DispatchWithRecompute(256); // a utility view recomputes later in the frame
    EXPECT_FALSE(s.Repeat()) << "the frame's verdict changed under calls that already acted on it";

    // The next FRAME sees it.
    EXPECT_TRUE(s.Step());
}

// The weight arithmetic that makes a frozen pair visible, pinned independently
// so a change to kLodFadeMaxWeight or the dither size cannot silently remove the
// consequence this rule exists to prevent. Mirrors lod_crossfade.glsl
// (weight 1..127) and adapter_forward.glsl (Bayer 4x4, thresholds k/16).
TEST(LodCrossfadeLiveness, AFrozenPairStrandsAnOutgoingDitherCell)
{
    constexpr uint32_t kMaxWeight          = 127u;
    constexpr double   kMaxBayerThreshold  = 15.0 / 16.0;
    constexpr double   kFrameDt            = 1.0 / 60.0;

    const double   alpha  = (kDuration - kFrameDt) / kDuration;
    const uint32_t packed = static_cast<uint32_t>(alpha * static_cast<double>(kMaxWeight) + 0.5);
    const double   weight = static_cast<double>(packed) / static_cast<double>(kMaxWeight);

    // Phase 1 (outgoing) keeps pixels where dither >= weight.
    EXPECT_GE(kMaxBayerThreshold, weight)
        << "a pair frozen one frame short of completion no longer strands an "
           "outgoing dither cell; the reasoning may be re-derived, but do it "
           "deliberately";
}
