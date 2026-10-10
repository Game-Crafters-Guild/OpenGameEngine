#include "Engine/Rendering/ViewTemporalHistory.h"

namespace GameEngine::Engine::Renderer
{

using Rendering::ViewId;

ViewDeformationClock ViewTemporalHistory::ResolveDeformationClock(double cumulativeSeconds)
{
    // A re-anchor shifts every modifier's phase by frequency * origin delta, so
    // it may only land where the motion history is already invalid: this
    // history's first frame — which covers a RenderServices rebuild, since the
    // history is rebuilt with it — and the precision ceiling, where every view
    // spends one frame with an unusable previous endpoint rather than letting
    // the lane's ULP grow past a frame's delta. A consumer reads the re-anchor
    // off the rotated sample: two samples carrying different origins are not
    // differenceable. A view's own first frame and a released view do not
    // touch the origin, so views never diverge in phase.
    if (!m_OriginAnchored || cumulativeSeconds - m_DeformationOrigin > kDeformationOriginCeilingSeconds)
    {
        m_DeformationOrigin = cumulativeSeconds;
        m_OriginAnchored = true;
    }
    ViewDeformationClock clock{};
    clock.TimeSeconds = static_cast<float>(cumulativeSeconds - m_DeformationOrigin);
    clock.Origin = m_DeformationOrigin;
    return clock;
}

const ViewTemporalSample* ViewTemporalHistory::Advance(ViewId viewId, uint64 frameIndex,
                                                       const ViewTemporalSample& current,
                                                       uint64 frameSubmittedCount,
                                                       bool* outPrevValid)
{
    Entry& entry = m_Entries[viewId];
    if (entry.FrameStamp != frameIndex)
    {
        // Previous is rotated from Current below, so it always holds THIS
        // view's last DISTINCT rendered frame — the correct reprojection source
        // and the clock that frame deformed at, regardless of how far the frame
        // index advanced in between. That index is a per-window stream counter,
        // so an OnDemand view that lapses (hidden tab, collapsed pane, inactive
        // split) resumes with an arbitrary jump in it; keying on "changed"
        // rather than "+1" is what keeps the reprojection pair valid across
        // such a resume, and what makes the previous clock the one the view
        // rendered with rather than the current clock minus a frame.
        //
        // Declared is not rendered, though, and every call site here is a
        // declare path: a host can abandon a frame after the graph is built
        // (the editor does, at three returns), and rotating on it would hand a
        // later frame a "previous" that never reached a pixel. The frame
        // stream's submitted-frame count is the evidence — RGFrame::Execute
        // moves it once per frame that recorded a live pass, and an abandoned
        // or all-culled frame leaves it where it was — so an abandoned frame's
        // sample is REPLACED rather than rotated, keeping the last submitted
        // frame as previous. The rendered-level history applies the same rule
        // to its buffer pair, keyed on its own scatter pass having executed.
        const bool previousFrameSubmitted = frameSubmittedCount != entry.SubmittedFramesAtStore;
        if (!entry.Initialized || previousFrameSubmitted)
        {
            entry.PrevValid = entry.Initialized;
            entry.Previous = entry.Initialized ? entry.Current : current;
        }
        entry.Current = current;
        entry.FrameStamp = frameIndex;
        entry.Initialized = true;
        entry.SubmittedFramesAtStore = frameSubmittedCount;
    }
    if (outPrevValid)
        *outPrevValid = entry.PrevValid;
    return entry.PrevValid ? &entry.Previous : &entry.Current;
}

} // namespace GameEngine::Engine::Renderer
