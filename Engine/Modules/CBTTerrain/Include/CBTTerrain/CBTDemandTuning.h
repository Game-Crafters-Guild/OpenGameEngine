#pragma once

// CBTDemandTuning.h — the editor defaults of the walking-headroom demand shaping and the
// screen-area priority ordering the CBT terrain renderer sends to Kernel_Classify through
// CBTFrameParams::DemandTuning and CBTFrameParams::PriorityParams. A CPU-side
// tuning POLICY beside CBTNearBias.h, not a GPU layout mirror: CBTLayout.h documents the field
// semantics and cbt_layout.glsl mirrors them. One owner for the values — the render feature
// fills the frame params from these, and the headless probes that run the production demand
// model read the same constants, so a retune cannot leave a probe pinning a demand model nobody
// ships.

namespace GameEngine::CBTTerrain
{

// DemandTuning.x — near-field world facet target (m), the behind-eye/straddle force-split floor
// (finding 2): the product bar. The straddle band sits AT the eye plane (grazing ~90 degrees)
// where cap-depth detail is invisible; bounding it here reclaims the near-plane over-refinement
// the deeper 0.25 m cap would cause.
inline constexpr float kNearFieldFacetTargetM = 0.5f;

// DemandTuning.y — an off-frustum bisector keeps its depth while live occupancy is below this
// (finding 3). A bisector entirely BEHIND the eye is exempt: the keep band is the yaw buffer for
// facets just outside the frustum, so it never cancels the behind-the-eye bound's merge wish.
// On a planar terrain this is the keep step's upper mark (see kOffFrustumKeepWidestNdc).
inline constexpr float kOffFrustumKeepOcc = 0.90f;

// The planar off-frustum keep step (cbt_layout.glsl CBT_WQ_OFF_FRUSTUM_KEEP_STEP and
// CBT_PlanarKeepBand, which these four constants mirror). The gate is a loop through the pool: an
// ungated planar view re-refines its whole off-frustum field in one update, so a band read straight
// off occupancy flips that field in and out of the pool on alternate updates. Kernel_Reset integrates
// a step instead. The step rises while occupancy is at or above kOffFrustumKeepOcc, falls one step
// per update while occupancy is more than the kernel's 30% keep ramp below it, and holds in between.
//
// The schedule is set by the field it releases. Across the saturating views measured (a 576 m tiled
// terrain at maxDepth 21, from 10 m to 34 m above it, pitched 80 to 89 degrees down), the ungated
// off-frustum field's ndcExcess spans under 1 NDC to about 1,000 NDC, its 99th percentile lies between
// 64 and 1,200 NDC, and no half-octave of it holds more than about a sixth of the field. So the band
// starts at the top of that range and falls by a constant ratio: no single step then releases more
// than that sixth (the largest measured 16%, inside the 30% hold band), and the first step releases
// only its thin far tail. A fixed first band is what cycled: a band of 9.4 NDC
// released 38% to 88% of the field in one update, the occupancy fell through the hold band, and the
// step returned to the ungated field.
//
// Step 1's band (NDC): the top of the measured range.
inline constexpr float kOffFrustumKeepWidestNdc = 1024.0f;
// The band halves every this many steps. The densest octave measured held 34% of the field (32 to
// 64 NDC at 20 m, 89 degrees down), so a step releases about 17% at most, and even the saturated
// stride's octave per update cannot carry occupancy from full across the 30% hold band.
inline constexpr uint32_t kOffFrustumKeepStepsPerOctave = 2u;
// The last step, where the band is 0. Step 23 is 0.5 NDC, the bottom of the measured range. Every
// facet behind the eye sits at one ndcExcess (CBT_OFF_FRUSTUM_BEHIND_NDC, 8 NDC), so those facets
// leave together, at step 16.
inline constexpr uint32_t kOffFrustumKeepSteps = 24u;
// Steps per update while occupancy is also at or above kCBTSaturatedOccupancy (0.98, where splits are
// rolled back and in-view refinement waits), so a view that does not fit spends fewer updates full:
// measured from roots, 10 full updates became 5 (20 m, 89 degrees down). A stride of 4 overshot: the
// same view settled three steps deeper with 2.3 times the rollbacks. One step per update below 0.98.
inline constexpr uint32_t kOffFrustumKeepSaturatedStride = 2u;

// DemandTuning.z = kEdgeRescueTpeMul * Screen.z: the flat-disc edge rescue's projected-edge
// threshold as a multiple of the split threshold (px) (finding 4). The same threshold holds a
// rescued parent's children from merging while the parent's split edge still exceeds it.
inline constexpr float kEdgeRescueTpeMul = 3.0f;

// DemandTuning.w — the rescue and its merge hold fire only while occupancy is below this
// (finding 4).
inline constexpr float kEdgeRescueOcc = 0.90f;

// PriorityParams.y — below this live occupancy the screen-area floor is 0 (neutrality).
inline constexpr float kPriorityRampStartOcc = 0.85f;

// PriorityParams.z — the maximum projected-area floor at full saturation (px^2): the
// graceful-degradation facet size. Under a pinned TPE=1 pool the largest features refine to about
// this and hold.
inline constexpr float kPriorityMaxAreaFloorPx2 = 48.0f;

// PriorityParams.w — the keep-large off-frustum annulus (NDC past the frustum margin): a large facet
// within this band of the edge holds its depth on a glance-away (no demote, no rebuild).
inline constexpr float kPriorityKeepLargeNdc = 1.5f;

} // namespace GameEngine::CBTTerrain
