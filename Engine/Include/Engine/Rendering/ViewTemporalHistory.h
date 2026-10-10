// Per-view history for temporal effects: the unjittered camera and the
// deformation clock each rendered frame used. Deliberately a sibling of
// ViewRegistry (RenderServices owns both) rather than more per-view registry
// state: unlike ViewAntiAliasing this history exists for EVERY anti-aliasing
// mode — SSSR reprojection needs the previous frame's camera even under
// MSAA/FXAA/off — and the registry stays pure view/camera bookkeeping.

#pragma once

#include "Rendering/CameraTypes.h" // CameraData, ViewId
#include "Types/Types.h"

#include <unordered_map>

namespace GameEngine::Engine::Renderer
{

// Ceiling on how far the rebased deformation lane may run from its origin
// before the history re-anchors. fp32 spacing at the ceiling is
// 2^12 * 2^-23 s = 0.49 ms — about 3 % of a 60 Hz frame — and the bounded
// advance rate of the cumulative clock (kMaxAnimationDeltaSeconds per frame)
// makes the resulting schedule predictable: one re-anchor per 68 minutes of
// rendered time at worst, taken by every view at once.
inline constexpr double kDeformationOriginCeilingSeconds = 4096.0;

// The deformation time a frame evaluates its vertex modifiers at, expressed as
// an offset from one origin every view shares rather than as absolute uptime.
// Both endpoints of a frame are formed from the same origin, so their
// difference is that frame's delta exactly at any uptime. The absolute
// cumulative clock is not: fp32 spacing is t * 2^-23, which reaches 7.8 ms at
// 18 hours of uptime and 31 ms at three days against a 16.7 ms frame — at which
// point both endpoints round to one value and the deformation term reads
// exactly zero. One origin rather than one per view so two views of one
// deforming surface — scene and game view, a split screen, a reflection probe
// and the camera it serves — see it at the same point in its wave.
struct ViewDeformationClock
{
    float TimeSeconds = 0.0f;
    // The origin TimeSeconds was formed against. Two samples carrying different
    // origins are not differenceable, so a consumer pairing them must treat the
    // mismatch as absent history.
    double Origin = 0.0;
};

// What one rendered frame of a view evaluated its temporal inputs at: the
// unjittered camera and the two animation clock lanes its light buffer carried
// (uTimeParams.zw — the rebased deformation time and
// RenderServices::GetScrollAnimationTimeSeconds, read at Advance in the same
// application frame that wrote them). A vertex modifier re-evaluated at the
// previous endpoint needs the clock that frame deformed at, not the current
// clock minus a delta.
struct ViewTemporalSample
{
    Rendering::CameraData Camera{};
    float DeformationTimeSeconds = 0.0f;
    float DeformationScrollSeconds = 0.0f;
    // Origin DeformationTimeSeconds was formed against; see ViewDeformationClock.
    double DeformationOrigin = 0.0;
};

class ViewTemporalHistory
{
public:
    // The deformation time for the application frame at `cumulativeSeconds`
    // (Time::GetCumulativeSeconds), rebased against the origin every view of
    // this history shares. The origin anchors on the first call — this
    // history's first frame, which is also a RenderServices rebuild's — and
    // re-anchors past kDeformationOriginCeilingSeconds; a view opened later
    // adopts it (its first frame has no previous anyway) and a released view
    // does not move it. Idempotent within one application frame — the
    // cumulative clock advances once per frame, so every caller of a frame
    // forms both endpoints against the same origin — and therefore safe to
    // call from the light-buffer writes and from every Advance site of the
    // same frame.
    ViewDeformationClock ResolveDeformationClock(double cumulativeSeconds);

    // Rotate the view's history once per rendered frame (first caller of a
    // frame rotates; later callers are no-ops). Returns the immediately
    // preceding rendered frame's sample when one exists, otherwise the current
    // sample (zero motion, zero clock delta) and reports false through
    // outPrevValid.
    //
    // `frameSubmittedCount` is RGFrame::SubmittedFrameCount of the frame
    // stream the view declares into, read at declare time. Every call site
    // here is a declare path, and a host can abandon a frame after the graph
    // is built, so a sample may only become a later frame's previous once that
    // count shows the frame it was stored in reached submission; a sample
    // whose frame did not is replaced rather than rotated.
    const ViewTemporalSample* Advance(Rendering::ViewId viewId, uint64 frameIndex,
                                      const ViewTemporalSample& current,
                                      uint64 frameSubmittedCount,
                                      bool* outPrevValid = nullptr);

    void ReleaseView(Rendering::ViewId viewId) { m_Entries.erase(viewId); }

private:
    struct Entry
    {
        ViewTemporalSample Current{};
        ViewTemporalSample Previous{};
        uint64 FrameStamp = ~0ull;
        bool Initialized = false;
        bool PrevValid = false;
        // Submitted-frame count when Current was stored. The frame that
        // produced Current was submitted exactly when the count has moved
        // past this.
        uint64 SubmittedFramesAtStore = 0;
    };
    std::unordered_map<Rendering::ViewId, Entry> m_Entries;
    // Origin the rebased deformation lane is formed against, shared by every
    // view, and whether it has been anchored yet. A re-anchor lands on every
    // view's next rotated pair as an origin mismatch, which is what marks
    // their motion history invalid for that one frame.
    double m_DeformationOrigin = 0.0;
    bool m_OriginAnchored = false;
};

} // namespace GameEngine::Engine::Renderer
