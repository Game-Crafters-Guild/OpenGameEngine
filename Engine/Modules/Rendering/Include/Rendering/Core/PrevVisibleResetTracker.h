/**
 * @file PrevVisibleResetTracker.h
 * @brief Per-view pending set for the two-phase HZB prevVisible reset.
 *
 * The two-phase HZB keeps one persistent prevVisible[] occlusion-history buffer
 * PER VIEW (not per frame-in-flight). A recycled instance slot must have its
 * history reset to 0xFFFFFFFF before that view next reads it, or the new tenant
 * inherits the prior tenant's occlusion state (design C1 pop-in).
 *
 * The set of recycled slots is global (GPUScene-owned) and drained once per
 * frame, but a view's buffer is only touched on the frames that view is
 * SUBMITTED. A view that is idle for a frame (hidden viewport, shadow-only or
 * frustum-only frame) would otherwise lose those resets permanently. This
 * tracker fans each frame's recycled slots into a per-view pending set and lets
 * each view flush (and clear) only its own set when it next schedules phase A,
 * so a returning view still applies every reset it missed.
 *
 * Pure CPU accounting — owns no GPU resources — so the multi-view lifecycle is
 * unit-testable without a device. Each view's pending set is kept sorted-unique
 * (merged on append) and is therefore bounded by the live instance count.
 */

#pragma once

#include "Rendering/CameraTypes.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class PrevVisibleResetTracker
{
  public:
    // Fan this frame's recycled instance slots into EVERY tracked view's pending
    // set. `recycledSlots` is sorted + deduped in place. Views registered later
    // (their buffer created after this call) do not receive these — their
    // first-touch 0xFFFFFFFF init covers the whole buffer instead. No-op when
    // there are no tracked views (the slots are then irrelevant).
    void OnSlotsRecycled(std::vector<uint32_t>& recycledSlots);

    // A view whose prevVisible buffer was just (re)created and full-reset by its
    // first-touch init: register it and drop any pending (the init supersedes).
    void OnBufferInitialized(ViewId viewId);

    // Move a view's pending slots (sorted-unique) into `out` and clear them, so
    // the caller can coalesce + fill them before phase A reads the buffer. Empty
    // when the view is untracked or has nothing pending; the view stays tracked.
    void TakeViewPending(ViewId viewId, std::vector<uint32_t>& out);

    // Sort + dedup `slots` in place, then coalesce the consecutive indices into
    // ascending [startElement, countElements) runs. Pure, static and stateless:
    // it is what every consumer of a pending set does with it (a clustered
    // eviction becomes a few FillBuffer ranges instead of one per slot), so it
    // lives with the tracker rather than in any one consumer.
    static void CoalesceResetRuns(std::vector<uint32_t>& slots,
                                  std::vector<std::pair<std::uint32_t, std::uint32_t>>& outRuns);

    // Test / debug introspection.
    std::size_t TrackedViewCount() const { return m_Pending.size(); }
    std::size_t PendingCount(ViewId viewId) const;

  private:
    // ViewId → pending recycled slots, kept sorted + unique.
    std::unordered_map<ViewId, std::vector<uint32_t>> m_Pending;
};

} // namespace Rendering
} // namespace GameEngine
