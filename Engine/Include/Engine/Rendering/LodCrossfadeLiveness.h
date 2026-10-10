#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Engine
{
namespace Renderer
{

/// What the last executed scatter dispatch wrote into the crossfade tail, as
/// reduced from the host-visible cursor mirror one app frame later.
struct LodCrossfadeTailObservation
{
    /// False when NO reading exists at all: none has been reduced yet, or the last
    /// one was structurally invalidated (no slice reduced, a tail-capable slice
    /// past the stats array, the readback resources gone). A reading survives a
    /// reduce that merely could not run this frame — the fence gate holding it off
    /// or an unmappable mirror is a timing miss, and `RecomputeCount` below is what
    /// decides whether the retained reading still describes the newest dispatch.
    /// The rule treats false as "a fade may be live"; there is no safe default zero.
    bool     Valid = false;
    /// Sum of every scheduled slice's tail row cursors. Non-zero means the last
    /// executed dispatch emitted at least one record PAIR, i.e. a transition was
    /// mid-dissolve when it ran.
    uint32_t TailRecords = 0;
    /// Scatter recompute-cause count at the moment this reading was reduced. The
    /// reading describes every dispatch up to that count and none after it.
    uint64_t RecomputeCount = 0;
};

/// Carried across frames so repeat calls within one frame replay one verdict.
struct LodCrossfadeLivenessState
{
    uint64_t ResolvedFrame = ~0ull;
    bool     Verdict = false;
};

/// Can a crossfade tail still hold a live (mid-dissolve) record pair?
///
/// Idle elision is a fixed point for every other scatter rule, so skipping a
/// dispatch whose inputs settled reproduces the retained records exactly. A fade
/// is not: if the scatter stops running mid-transition the last frame's record
/// pair persists and the object stays permanently half-dissolved — every fading
/// instance drawn twice forever, and (while the frozen weight sits at or below
/// the Bayer dither's largest threshold, 15/16) 1 pixel in 16 of the OUTGOING
/// level stippled on screen. The caller therefore holds elision off while this
/// returns true.
///
/// The verdict is an OBSERVATION, not a prediction. Two facts make a zero tail
/// count sufficient:
///  - a transition can only START inside a dispatch, because starting one means
///    the shader picked a level that differs from the instance's history;
///  - a dispatch whose recorded inputs are byte-identical to the previous one
///    picks the same level for every instance, so it cannot start a transition.
///    (The frame clock is deliberately outside that record — it only advances
///    and completes fades already in flight, never starts one.)
/// So if the newest dispatch that ran with a recompute cause wrote no tail
/// records, and every dispatch since had identical inputs, nothing is fading and
/// the retained records are the settled single ones.
///
/// `currentRecomputeCount` is what makes the second clause checkable, and it is
/// load-bearing rather than defensive. Two lags of different length stack: the
/// reduce reads a mirror one app frame behind the dispatch that wrote it, and the
/// spine resolves a frame's verdict (UpdateIdleElisionFrameState) BEFORE that
/// frame's reduce runs — so the reading describes a dispatch TWO app frames back
/// while `currentRecomputeCount` already covers the gate evaluations of ONE frame
/// back. A dispatch that recomputed in that gap may have started a fade the
/// reading cannot see. A stamp mismatch is therefore "unknown", not "settled".
///
/// This cannot latch itself open. A frame held awake by this very suppression
/// re-evaluates its gate with unchanged inputs, which resolves the cause
/// `Skipped` (RecomputeElision.cpp — the cause is resolved independently of
/// whether elision was allowed) and `Skipped` is not one of the recompute
/// causes. So the tag stops advancing while suppression runs, and the rule
/// releases as soon as a zero reading arrives.
///
/// Elision suppression is the ONLY consumer. Nothing in pass declaration reads
/// it: the depth prepass and the colour pass both draw the crossfade tails, so
/// no attachment's access mode depends on whether a fade is live.
///
/// `frameIndex` makes the verdict per-FRAME rather than per-call. The frame
/// spine pushes the elision context once per scatter call (owner spine, each
/// utility view, then phase B), and a mid-frame recompute by one call must not
/// flip the verdict the earlier calls already acted on.
///
/// Returns false whenever the feature is off (`durationSeconds` <= 0).
bool ResolveCrossfadeLiveness(float durationSeconds,
                              const LodCrossfadeTailObservation& observation,
                              uint64_t currentRecomputeCount, uint64_t frameIndex,
                              LodCrossfadeLivenessState& state);

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
