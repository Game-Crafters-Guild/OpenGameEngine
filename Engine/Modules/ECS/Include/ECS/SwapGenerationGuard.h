#pragma once

// Consumer-side cadence guard for window-delivered change signals (the
// component dirty feed, the lifecycle event buffers). Both are
// double-buffered and swapped once per engine tick; a consumer that skips a
// swap window — disabled via SetRenderingSystemEnabled (the editor's
// play-mode pause), throttled via ConfigureRenderingSystemEveryNFrames, or
// conditionally stepped — permanently misses the entries discarded during
// the gap. The guard detects that from the source's monotonic swap
// generation so the consumer can run its one-shot recovery pass before
// trusting the current window.
//
// Only the gap arithmetic and its conventions are shared here
// (compare-then-assign, gap > 1 not >= 1, unsigned wrap, once per run at
// entry); recovery actions stay at the call sites — the consumers'
// recoveries are structurally different (bookkeeping sweep, poll fallback,
// full-lane rebuild) and folding them in would be premature abstraction.
//
// When NOT to use this: a consumer whose baseline is owned by its recovery
// pass rather than by the check does not fit the consume-at-entry contract —
// one that assigns only once a rebuild has actually re-established truth
// (sticky miss until healed), deliberately skips the check on paths some
// other mechanism heals (so the gap accrued across those runs IS the
// re-entry recovery), or re-baselines a world switch to the new source's
// current generation because a separately scheduled rebuild covers it.
// Compare-then-assign here would mark windows consumed that such a recovery
// never covered, and the guard deliberately exposes no seed operation. Keep
// that consumer's arithmetic at its call site with a comment naming the
// divergence — the editor Hierarchy panel's incremental-sync cadence guard
// (HierarchyPanel.cpp, Update()) is the canonical example.
//
// State lives on the consumer object as a member (the image-split rule
// concerns inline globals and statics, not object members).

#include "ECS/Types.h"

namespace GameEngine::ECS
{

class SwapGenerationGuard
{
public:
    // Consume the source's current swap generation and report missed
    // windows. MUST be called exactly once per consumer run, on every path,
    // at run entry — compare-then-assign: a run that skips the call accrues
    // a false gap that fires a spurious recovery on the next call.
    //
    // Returns true when at least one whole window was swapped out unseen
    // since the last call (gap > 1) — run the recovery pass before trusting
    // the current window. gap == 0 (multiple consumer steps within one
    // window) and gap == 1 (normal cadence) return false.
    //
    // A fresh guard observing a generation of 0 or 1 does NOT report a miss
    // (1 - 0 == 1): consumers are constructed before their source's first
    // swap, and first-window recovery is guaranteed by other means
    // (first-tick reconcile, lazy insert, unprimed caches) — never by this
    // predicate. Unsigned arithmetic makes a source restart (generation
    // below the cached value) read as a huge gap => recover; uint64 cannot
    // meaningfully wrap at one swap per frame.
    //
    // Generations from different worlds are not comparable: the guard pairs
    // with the world whose source produced them (the change-gate worldId
    // pairing precedent) and a world switch re-baselines it as freshly
    // constructed against the new source, first-call semantics included.
    bool ConsumeAndCheckMissed(uint64 worldId, uint64 currentGeneration)
    {
        if (worldId != m_PairedWorldId)
        {
            m_PairedWorldId = worldId;
            m_LastConsumed = 0;
        }
        const bool missed = currentGeneration - m_LastConsumed > 1;
        m_LastConsumed = currentGeneration;
        return missed;
    }

private:
    uint64 m_PairedWorldId = 0;  // world ids start at 1; 0 = never paired
    uint64 m_LastConsumed = 0;
};

} // namespace GameEngine::ECS
