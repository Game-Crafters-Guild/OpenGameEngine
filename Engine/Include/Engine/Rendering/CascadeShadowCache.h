#pragma once

// CascadeShadowCache — per-(view, slot) skip/re-render decisions for the
// directional cascade shadow depth passes and their glass-tint twins
// (static-scene shadow caching, the directional analogue of the point-shadow
// L1a planner cache).
//
// A cascade layer's rasterized content is a pure function of a small input set:
// the texel-snapped cascade fit (LightVP / LightVPRel) and the caster footprint
// the cull tightens to, the main camera (GPU LOD selection consumes it at cull
// time), the relevant shadow-caster content version
// (RenderServices::ShadowCasterContentVersion — bumped on any moved / added /
// removed / vertex-mod caster, and on a bone-palette content change so a
// skeletal animation playing in place re-renders its caster rather than
// retaining a layer rasterized at an older pose), the pooled physical texture
// identity (a realloc yields a fresh Undefined image under a matching key),
// and the config/LOD knobs. When every input is byte-identical to the last
// rendered frame, re-rendering would reproduce the retained layer exactly, so
// the pass declaration can be skipped and receivers keep sampling the pooled
// persistent layer — the same contract the point-shadow atlas ships
// (PointShadowAtlasPlanner L1a: a clean slot declares no face passes).
//
// Cull lag ("settle") rule: shadow culling for frame N runs on the fit
// computed at culling time for frame N (ShadowMapRenderFeature::
// FitViewCascades), the fit the cascades then render with, unless an input
// changed between the cull and the ShadowMap node's declaration (an authored
// setting, the camera, the project resolution). The node then refits, and that
// frame's render was culled with the OLD planes — its content can be
// edge-deficient beyond the overlap margin. A re-render heals it; a cache must
// not freeze it. A record is therefore only skippable once a render happened on
// a frame whose fit equalled the previous frame's fit (CullSettled) — one extra
// re-render per fit transition buys exact parity with the always-render
// pipeline whichever fit the cull ran on.
//
// Fit-freeze qualification: with GE_SHADOW_FIT_FREEZE on, the cull-consumed
// camera is part of the same lagged input — GPU LOD selection reads the FROZEN
// camera snapshot the culled fit carries — so a refit that steps the frozen
// camera while landing byte-identical fit bytes may have rendered against the
// PREVIOUS snapshot. The settle equality therefore includes CameraViewProj when
// the caller runs the freeze (Evaluate's includeCameraInSettle). Freeze off
// keeps the fit-only equality: the live camera changes every motion frame
// anyway, and at rest the fit and the camera are both unchanged.
//
// Pure CPU state machine: no device, no RenderServices — unit-testable. The
// caller (ShadowMapRenderFeature) owns input assembly, the enable flag
// (GE_SHADOW_STATIC_CACHE, default ON), and physical-lifetime invalidation
// (adopt-change / device rebuild), mirroring the planner's split between
// content identity and GPU-resource lifetime.

#include "Engine/Rendering/ShadowCasterChanges.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{

// Why a cascade slot re-rendered (or did not) this frame. Ordered by the
// check priority Evaluate applies; instrumentation only.
enum class CascadeCacheDirtyCause : uint8_t
{
    Cached = 0,          // clean: every input matched the last settled render — skip
    FirstRender,         // slot never rendered under this record
    ContributorPresent,  // contributor depth commands (terrain/ocean) — not epoch-tracked, never cache
    ExecUnderDraw,       // last render's exec reported a silent skip path (async pipeline /
                         // material publish gate, stale-survivor walk-skip, missing GPU
                         // content) — the committed layer under-drew; render until clean
    ContributorChanged,  // contributor presence flipped since the last render (an unloaded
                         // terrain's shadow must not be retained under a matching key)
    PhysicalChanged,     // pooled physical realloc — retained layers are gone
    CasterContentChanged,// global shadow-caster content version advanced
    CameraChanged,       // main camera viewProj changed (LOD selection input)
    CascadeFitChanged,   // snapped LightVP / LightVPRel / split distances stepped
    ConfigChanged,       // resolution / cascade count / LOD knobs / caster-reduction lane
    CullNotSettled,      // last render's cull consumed a different fit (one-frame lag rule)
    CauseCount,
};

// Everything that determines one cascade layer's rasterized depth (or tint)
// content. Compared field-wise by Evaluate — exact equality, no epsilon:
// the fit is texel-snapped upstream, so equality is the common steady case.
struct CascadeRenderInputs
{
    float LightVP[16]{};
    float LightVPRel[16]{};
    // Camera viewProj the cascade cull consumes: GPU LOD selection keys on its
    // camera position. Fit-freeze ON: the cascade's FROZEN snapshot (steps
    // only on refit). Freeze off: the live camera, every frame.
    float CameraViewProj[16]{};
    // The caster footprint the cull tightens the side planes to
    // (CascadeFrameData::CasterFootprint), all zero when the cascade keeps its
    // full planes. Frozen with the fit under the freeze.
    float CasterFootprint[4]{};
    // Render-origin sector the fit was rebased against (Earth-scale lane).
    // LightVPRel covers it numerically; kept explicit so a sector step can
    // never alias through a coincidental matrix match.
    int32_t RenderOriginSector[3]{};
    uint64_t WorldId = 0;
    uint64_t CasterEpoch = 0; // latest change relevant to this cascade's light-space footprint
    uint64_t PhysicalId = 0; // raw pooled texture handle bits (depth array, or tint array for tint slots)
    uint32_t Resolution = 0;
    uint32_t NumCascades = 0;
    float ShadowLODBias = 0.0f;
    uint32_t LODForceLevel = 0;
    // Per-view SSE coverage scales the bucketer's shadow slices select LODs
    // with (RenderServices::ComputeViewSseScales). A budget or viewport-height
    // change moves SSE switch points, so retained cascades must invalidate —
    // same contract as ShadowLODBias.
    float SseThresholdToCoverage = 0.0f;
    float SseThresholdToCoverageTight = 0.0f;
    // Which mapping seeded the mesh rows' switch points
    // (MeshGPURegistry::GetLodSelectionMode, as its underlying value). Changing
    // it re-derives every row, moving switch points exactly as a budget change
    // does — same contract as SseThresholdToCoverage. Without it a live mode
    // switch would leave retained cascades on their pre-switch LOD picks while
    // the camera pass re-selected.
    uint32_t SelectionMode = 0;
    uint32_t RenderLayerMask = 0xFFFFFFFFu;
    bool CasterReduction = false;
    bool HasContributorDrawCommands = false;
};

class CascadeShadowCache
{
  public:
    // Slot layout per view: depth cascades then their glass-tint twins. The
    // tint pass shares the depth cascade's cull slices and fit, but retains
    // content in its own pooled physical, so it keeps an independent record.
    static constexpr uint32_t kMaxCascadesPerView = 4;
    static constexpr uint32_t kDepthSlotBase = 0;
    static constexpr uint32_t kTintSlotBase = kMaxCascadesPerView;
    static constexpr uint32_t kSlotsPerView = kMaxCascadesPerView * 2;

    // Fold attributed caster changes into this slot's content epoch. Only XY
    // separation from BOTH the requested and retained light-space footprints
    // can exclude a caster: directional depth passes
    // clamp depth, so casters beyond the near/far planes still affect the map.
    // Repeated calls for an epoch are idempotent (motion planning and declaration
    // both build keys). Unattributed changes, version gaps and world switches
    // refresh conservatively. No borrowed change-set storage is retained.
    uint64_t TrackCasterChanges(uint32_t viewId, uint32_t slot, uint64_t worldId,
                                const ShadowCasterChangeSet& changes, const float lightVP[16],
                                uint32_t resolution);

    struct Decision
    {
        bool Skip = false; // true => do not declare the pass; retained layer is exact
        CascadeCacheDirtyCause Cause = CascadeCacheDirtyCause::FirstRender;
    };

    // Evaluate one slot against its last-rendered record. `frameStamp` is the
    // declaring frame's monotonic index: previous-frame fit tracking is only
    // trusted when the last Evaluate happened exactly one stamp ago — an
    // Evaluate gap (cascade-count shrink, hidden view) invalidates it, so a
    // render culled against a stale-or-absent slice can never commit settled.
    // `includeCameraInSettle` (wire from the fit-freeze flag) adds
    // CameraViewProj to the settle equality — see the frozen-camera
    // qualification in the header prose; false preserves the fit-only settle
    // byte-exactly. Always updates the tracking and the stats histogram —
    // with `enabled` false the decision is never Skip but the would-be cause
    // is still counted, so a disabled run measures the achievable hit rate.
    // Never mutates the rendered record: the caller commits via OnRendered
    // AFTER it actually declares the pass.
    Decision Evaluate(uint32_t viewId, uint32_t slot, uint64_t frameStamp,
                      const CascadeRenderInputs& in, bool enabled,
                      bool includeCameraInSettle);

    // Read-only preview of the cause Evaluate would resolve for `in` this
    // frame — no tracking update, no stats. Drives the motion round-robin
    // plan phase, which needs every slot's class BEFORE the per-slot declare
    // sites run their (single, mutating) Evaluate. `pendingUnderDraw` stands
    // in for a DepthUnderDrawTracker mark the declare site has not consumed
    // yet (Peek there, Consume at declare) so the preview matches the later
    // Evaluate exactly. Shares ResolveCause with Evaluate — they cannot drift.
    CascadeCacheDirtyCause PeekCause(uint32_t viewId, uint32_t slot,
                                     const CascadeRenderInputs& in,
                                     bool pendingUnderDraw) const;

    // Commit: the pass for (viewId, slot) actually recorded this frame with `in`.
    // Call from pass exec, not from declare — a declared-then-culled pass must
    // not settle against an unwritten reverse-Z image (cleared-to-near = fully
    // shadowed). Must still follow the same frame's Evaluate (the settle stamp
    // reads the prev-fit equality Evaluate computed).
    void OnRendered(uint32_t viewId, uint32_t slot, const CascadeRenderInputs& in);

    // Exec→declare feedback: the slot's LAST declared render reported a silent
    // under-draw at exec time (DepthUnderDrawTracker). Called by the declare
    // site after consuming the mark, BEFORE the same frame's Evaluate; cleared
    // by the next OnRendered (each render is a fresh attempt — a repeat
    // under-draw re-marks next frame).
    void MarkUnderDrew(uint32_t viewId, uint32_t slot);

    // Physical-lifetime guards (content identity does not track GPU lifetime):
    // the caller invalidates on pooled-physical adopt-change and device rebuild.
    void InvalidateView(uint32_t viewId);
    void Reset();

    struct Stats
    {
        uint64_t Evaluated = 0;
        uint64_t Skipped = 0;
        std::array<uint64_t, static_cast<size_t>(CascadeCacheDirtyCause::CauseCount)> CauseCounts{};
    };
    // Per-view aggregate across all slots. Returns a zero Stats for an
    // unknown view.
    const Stats& GetStats(uint32_t viewId) const;

    // Per-view delta since the previous call, advancing the snapshot it
    // differences against. The snapshot lives WITH the running totals
    // (ViewState) precisely so a stats clear can never leave it behind:
    // differencing a cleared total against a surviving snapshot underflows
    // every field, and no equality guard catches that because a large
    // snapshot never equals a small total. Unknown view => zeros, no insert.
    Stats ConsumeStatsWindow(uint32_t viewId);

    // Edge state for the caller's zero-hit storm tripwire: stores `active`,
    // returns what it replaced. Parked with the stats for the same reason a
    // Reset is a new episode, so the first storm window after one warns
    // again instead of being suppressed by a pre-Reset flag.
    bool ExchangeStormActive(uint32_t viewId, bool active);

  private:
    struct SlotState
    {
        uint64_t ObservedWorldId = 0;
        uint64_t ObservedCasterEpoch = 0;
        uint64_t RelevantCasterEpoch = 0;
        bool HasObservedCasters = false;
        CascadeRenderInputs Rendered{}; // inputs at last committed render
        bool HasRendered = false;
        bool CullSettled = false; // last render's fit equalled the CONSECUTIVE previous frame's fit
        bool UnderDrew = false;   // exec feedback: last render silently dropped draws
        CascadeRenderInputs PrevFrame{}; // last Evaluate's inputs (settle tracking)
        bool HasPrevFrame = false;
        uint64_t PrevFrameStamp = 0; // frame stamp of the last Evaluate (gap detection)
        bool PrevFitEqual = false; // this frame's fit == the consecutive previous frame's fit
    };
    struct ViewState
    {
        std::array<SlotState, kSlotsPerView> Slots{};
        Stats ViewStats{};
        // Window-log bookkeeping, held here rather than by the caller so it
        // shares the totals' lifetime exactly (see ConsumeStatsWindow).
        Stats LoggedSnapshot{};
        bool StormActive = false;
    };

    // Cause resolution in priority order — the single implementation behind
    // Evaluate and PeekCause. Pure over (record, inputs, pending under-draw).
    static CascadeCacheDirtyCause ResolveCause(const SlotState& s, const CascadeRenderInputs& in,
                                               bool underDrew);

    std::unordered_map<uint32_t, ViewState> m_Views;
};

} // namespace Engine::Renderer
} // namespace GameEngine
