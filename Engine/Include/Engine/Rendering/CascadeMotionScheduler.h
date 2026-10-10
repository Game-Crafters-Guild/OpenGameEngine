#pragma once

// CascadeMotionScheduler — round-robin render budget for camera-motion cascade
// shadow updates (GE_SHADOW_MOTION_CAP; ShadowMapRenderFeature defaults the
// cap to 2, env-overridable, 0 = off).
//
// The CascadeShadowCache already classifies WHY a cascade slot would re-render.
// Two of those causes — CameraChanged and CascadeFitChanged — are pure
// camera-motion churn: the retained layer still shows the correct world-space
// shadows, just fit to a slightly older frustum slice. During continuous
// motion all cascades carry a motion cause every frame, and re-rendering all
// of them is the single largest steady-state GPU cost of the shadow pipeline.
//
// This scheduler bounds that cost: given each cascade's update class and its
// content staleness (frames since the retained layer was committed or proven
// byte-identical to the current fit), at most `motionCap` motion-class
// cascades render per frame; the rest are DEFERRED — their passes are not
// declared, the retained (settled) layer stays, and receivers keep sampling
// it through its content-matching fit (the ShadowMapRenderFeature content-fit
// invariant). Selection is nearest-first (cascade 0 has the highest texel
// density and the most visible staleness), with a hard staleness bound: a
// cascade whose content would exceed `maxAge` frames of drift is
// force-rendered, displacing nearer cascades from the budget. If more
// cascades hit the bound than the budget allows, ALL of them render — the
// staleness bound is the quality contract, the cap is best-effort.
//
// Staleness is caller-computed from the content-fit commit stamps (ground
// truth), not from internal counters — a hidden view, a cascade-count regrow,
// or any tracking lapse shows up as large staleness and forces a render
// instead of silently deferring against arbitrarily old content.
//
// Correctness classes (caster content, contributor, under-draw, physical,
// config, first render, cull-settle) NEVER defer and NEVER consume budget:
// they render immediately, all of them, every frame they occur.
//
// Pure CPU: the only state is per-view instrumentation. The caller
// (ShadowMapRenderFeature) owns cause classification, the enable knobs, and
// applies DeferMask to both the depth cascade and its glass-tint twin so the
// pair stays coherent with the single sampling fit.

#include <array>
#include <cstdint>
#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{

enum class CascadeCacheDirtyCause : uint8_t;

// Per-frame update class of one cascade PAIR (depth slot + tint twin).
enum class CascadeUpdateClass : uint8_t
{
    Cached = 0,  // content matches every current input — skips via the cache
    Motion,      // dirty ONLY through camera motion (Camera / Fit) — deferrable
    Correctness, // any other dirty cause — must render this frame, no budget
};

// Cause → class mapping. Kept as a free function so the correctness-class
// bypass is unit-testable against the full cause enum.
CascadeUpdateClass ClassifyCascadeDirtyCause(CascadeCacheDirtyCause cause);

// Pair classification: a cascade's depth slot and its glass-tint twin defer
// or render as a unit, and BOTH retained layers are sampled through the ONE
// depth-content fit. When the tint family declares (`tintCause` non-null)
// the pair takes the more severe class — and a Motion pair is
// deferral-eligible only when the two families' retained content was
// rasterized under the SAME fit (`contentFitsLockstep`: byte-equal
// content-fit LightVPs). A transmission-visibility gap desyncs them (tint
// declares bail while depth keeps rendering), and the depth family's fresh
// content stamp then says nothing about the tint layer's age — the maxAge
// bound would not hold for it. A desynced Motion pair therefore promotes to
// Correctness and renders both families this frame. Cached pairs are exempt:
// both records byte-equal the same current inputs, which implies lockstep by
// construction.
CascadeUpdateClass ClassifyCascadePair(CascadeCacheDirtyCause depthCause,
                                       const CascadeCacheDirtyCause* tintCause,
                                       bool contentFitsLockstep);

class CascadeMotionScheduler
{
  public:
    static constexpr uint32_t kMaxCascades = 4;
    // Staleness for a slot with no committed content — never deferrable.
    static constexpr uint64_t kStalenessUnknown = ~0ull;

    struct FramePlan
    {
        // Bit c set => cascade c's depth + tint passes must NOT be declared
        // this frame; the retained layer is kept and sampled via its content
        // fit. Never set for Cached or Correctness classes.
        uint32_t DeferMask = 0;
    };

    // Plan one view's frame. `staleness[c]` = declaring frame index minus the
    // frame the retained content was last rendered OR last proven
    // byte-identical to the current fit (a Cached skip refreshes the stamp) —
    // i.e. how many frames of camera-motion drift receivers sample if the
    // slot defers this frame. Deferral requires staleness <= maxAge;
    // `motionCap` == 0 disables deferral entirely.
    FramePlan PlanFrame(uint32_t viewId, uint32_t numCascades,
                        const std::array<CascadeUpdateClass, kMaxCascades>& classes,
                        const std::array<uint64_t, kMaxCascades>& staleness,
                        uint32_t motionCap, uint32_t maxAge);

    void Reset();

    struct Stats
    {
        uint64_t Deferred = 0;        // slot-frames deferred
        uint64_t ForcedByAge = 0;     // renders forced by the staleness bound
        uint64_t ForcedBeyondCap = 0; // staleness-forced renders past the budget
    };
    // Per-view aggregate. Returns a zero Stats for an unknown view.
    const Stats& GetStats(uint32_t viewId) const;

  private:
    std::unordered_map<uint32_t, Stats> m_Views;
};

} // namespace Engine::Renderer
} // namespace GameEngine
