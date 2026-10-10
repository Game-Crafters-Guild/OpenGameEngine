#pragma once

// Point-light shadow atlas planning (arc slice M1). Turns the per-view set of
// shadow-casting point lights into a bounded, importance-ranked set of atlas
// slots with per-slot resolution tiers — the multi-light replacement for the
// single "first flagged light wins" selection.
//
// Two decisions live here, both STATEFUL across frames (keyed by the light's
// stable SortId), which is exactly why this is a planner object and not a pure
// function like PointShadowFaceCull:
//
//   1. Tier   — screen-coverage drives a Low/Medium/High resolution tier, with
//               hysteresis (asymmetric promote/demote thresholds) plus a frame
//               cooldown after any change, so a light hovering on a tier
//               boundary does NOT re-tier every frame (design §4.4 / A7). A
//               light that authored an explicit tier (Light.ShadowResolutionTier
//               != Inherit) pins to it and skips the coverage machinery.
//   2. Budget — at most `budget` lights get an atlas slot. Slots are STICKY:
//               an incumbent keeps its slot across frames and is only evicted
//               for a challenger that out-ranks it by a hysteresis margin (and
//               only once the incumbent's admission cooldown elapsed), so the
//               admitted set is stable under a slow dolly. A light that leaves
//               the candidate set frees its slot; on re-admission it starts
//               fresh (design §4.4 / A8 eviction).
//
// The atlas itself is a single D32 2D-array: one array layer per (slot, face)
// pair — the layer is the render-graph hazard/load-op/isolation unit (design
// §4.2 / A2). Lower tiers render into the top-left tileRes×tileRes SUB-RECT of a
// full-resolution layer (max-res-with-waste — §4.2 alternative (b)); the wasted
// border is accepted as the cost of keeping one texture + one comparison sampler
// on the world set. The pure tile→atlas UV helpers below mirror the receiver-side
// GLSL (clustered_lighting.glsl GE_SamplePointShadow) so the sampling math is
// unit-testable without a device (design §4.5 / A4).

#include "Engine/Rendering/ShadowCasterChanges.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{

// Every allocated array layer shares these dimensions (a 2D-array cannot have
// per-layer resolution), so the atlas is sized to the highest tier and lower
// tiers waste the border. This IS the High tier (see ResolvePunctualShadowResolution
// in ShadowFrameInfo.h): a High/Inherit slot fills the whole layer (tileScale 1.0)
// and its sampling reduces exactly to the pre-M1 single-light full-layer path.
inline constexpr uint32_t kPointAtlasTileResolution = 1024u;

// Hard ceiling on simultaneously-shadowed point lights. Bounds the SSBO slot
// array, the atlas layer count (kMaxPointShadowSlots * 6), and the per-(slot,face)
// GPU-cull / M2a-survivor key range. 16 slots == the design's "4096²-equivalent
// = 16 @1024²" envelope (§5). The runtime budget clamps to this.
inline constexpr uint32_t kMaxPointShadowSlots = 16u;

// Default shadowed-local budget: the number of point lights that cast shadows at
// once. The default is 4; choosing the shipped value is tracked in #2934.
// It is a config/node field (ShadowMapNode "pointShadowBudget"), so a pipeline
// can set it without a code change.
inline constexpr uint32_t kDefaultPointShadowBudget = 4u;

// Frames a light's tier + slot are frozen after a change, so oscillation across
// a boundary cannot thrash the (re)render of its map. ~0.4 s at 60 fps.
inline constexpr uint32_t kDefaultPointShadowCooldownFrames = 24u;

// L1a refresh budget: at most this many dirty slots re-render their faces in one
// frame; lower-importance dirty slots defer a frame (they stay dirty and sample
// last frame's atlas for one frame). Godot's MAX_UPDATE_SHADOWS analogue. The
// default equals the slot ceiling so it is a safety valve, not a common path —
// with kMaxPointShadowSlots slots a full refresh is only kMaxPointShadowSlots*6
// passes, far under a per-frame pass budget.
//
// CAVEAT if ever lowered below the slot count: a BudgetDeferred MOVED light keeps
// last frame's atlas depth (old pose) while the receiver SSBO uploads THIS frame's
// (new-pose) face VPs — a one-frame depth/VP incoherence. Godot avoids it by
// keeping the last-rendered VPs for a deferred light; a lowered budget here would
// need the same (cache the rendered VPs per slot and upload those for a deferred
// slot). At the default (== slot ceiling) deferral never fires, so this is latent.
inline constexpr uint32_t kDefaultPointShadowRefreshBudget = kMaxPointShadowSlots;

// Why a slot re-rendered (or did not) this frame — instrumentation only, surfaced
// per committed slot so the caller can log cached/rendered counts with cause tags
// (design §7.3 L1a). Ordered by the check priority the planner applies.
enum class PointShadowDirtyCause : uint8_t
{
    Cached = 0,     // clean: content, tier, LOD key and caster epoch all matched — no passes
    FirstRender,    // slot never rendered this tenant's content before
    SlotReTenant,   // a different light now owns this slot
    LodSelectionChanged, // a LOD selection knob moved — see PointShadowLodKey
    CasterMoved,    // a changed caster's bounds intersected this light's influence
                    // sphere (or the change was unattributed) — see L1b keying below
    TierChanged,    // committed resolution tier changed
    ContentChanged, // light moved / range changed / camera face-visibility changed
    BudgetDeferred, // dirty but over the refresh budget this frame — retried next
    CauseCount,
};

// The LOD selection knobs a view's point-face depth slices rasterize under, and
// the render cache's key on them. A retained slot holds depth drawn at the levels
// these knobs selected at its last render — move one and the retained silhouettes
// no longer match what a fresh render would produce. Per-view, not per-light:
// every slot in a view shares them, so a change dirties the whole view's atlas.
//
// ONE struct serves both roles on purpose. MakePointFaceLodParams
// (PointShadowLodSelection.h) builds the slices' ViewLODParams from this same
// type, so "what the planner keyed" and "what the bucketer registered" cannot
// drift apart the way two hand-copied field lists can.
//
// Field-wise exact equality, no epsilon: every field is an AUTHORED knob — a
// value somebody set, byte-identical frame to frame while nothing writes it.
// There is nothing continuous here to drift.
//
// What is absent is the point. No camera position, no viewport height, no
// projection: point-face LOD is selected from the LIGHT (see
// PointShadowLodSelection.h), so camera-side quantities never enter selection and
// so never need keying. The camera reaches a slot only through inputs the planner
// already keys discretely — the resolution Tier (hysteresis-damped screen
// coverage) and the face-visibility mask (folded into ContentHash). A slot's
// render is therefore a pure function of keyed inputs: cached depth equals what a
// fresh render would produce, with no residual drift to bound.
//
// The Tier carries one extra load worth naming, because the SSE budget is spent
// in the face's TILE: MakePointFaceLodParams derives the switch points from
// ResolvePointShadowTileResolution(Tier, InheritResolution), and only Tier is
// keyed. That is sound solely because the single caller passes a CONSTANT
// InheritResolution (kPointAtlasTileResolution — "committed tier is never
// Inherit"). Make that per-node and the tile could move with the Tier unchanged,
// leaving the derived switch points unkeyed; key TileResolution here at the same
// time.
//
// INVARIANT: every knob that can change which level a point-face slice
// rasterizes belongs here. The directional sibling keys the same class of
// inputs (CascadeShadowCache's ConfigEqual) — a knob added to one key and not
// the other leaves that side retaining depth drawn at levels nothing will
// re-render.
struct PointShadowLodKey
{
    float Bias = 0.0f;                 // global + shadow LOD bias, log2 coverage scale
    uint32_t ForceLevel = 0xFFFFFFFFu; // 0xFFFFFFFF = auto-select by coverage
    uint32_t SelectionMode = 0;        // MeshGPURegistry::LodSelectionMode as its underlying
                                       // value; a switch re-derives every mesh row's switch
                                       // points, moving picks exactly as a budget change does
    // SSE error budget, spent in FACE texels (MakePointFaceLodParams converts it
    // through the slot's tile resolution). 0 is the scatter's keep-detail
    // fail-safe, matching ViewLODParams' own default.
    float ErrorBudgetPx = 0.0f;
    float SkinnedBudgetScale = 1.0f; // tight-class multiplier on the budget above

    bool operator==(const PointShadowLodKey&) const = default;
};

// Face-slot → array layer. Each slot reserves 6 contiguous layers (its cube
// faces); a face S1-culled this frame simply skips its layer's render pass, the
// layer stays reserved (Unity URP "6 tiles per cube, unused faces reserve slots").
inline constexpr uint32_t PointShadowSlotFaceLayer(uint32_t slot, uint32_t face)
{
    return slot * 6u + face;
}

// Total atlas layers for a budget.
inline constexpr uint32_t PointShadowAtlasLayerCount(uint32_t budget)
{
    return budget * 6u;
}

// Resolution a tier renders at within the atlas, clamped to the atlas tile size
// (a tier/inherit resolution above the atlas max cannot fit a layer). Tier 0
// (Inherit) resolves to inheritResolution — the node's punctual resolution —
// then clamps; the fixed Low/Medium/High tiers resolve to 256/512/1024.
uint32_t ResolvePointShadowTileResolution(uint32_t tier, uint32_t inheritResolution);

// tileRes / kPointAtlasTileResolution ∈ (0, 1]. 1.0 for a full-layer (High/Inherit)
// slot. A face UV in [0,1] maps into the layer's [0, tileScale] sub-rect.
float PointShadowTileScale(uint32_t tileResolution);

// Map a receiver-side FACE UV (already y-flipped for the negative viewport, and
// already offset by a PCF tap) into its layer's atlas UV, clamped to the tile
// sub-rect minus a half-texel gutter so a tap can never bleed into the cleared
// border outside the tile. Mirrors clustered_lighting.glsl exactly (design §4.5).
struct PointShadowAtlasUV
{
    float U = 0.0f;
    float V = 0.0f;
};
PointShadowAtlasUV PointShadowTileToAtlasUV(float faceU, float faceV, uint32_t tileResolution);

class PointShadowAtlasPlanner
{
  public:
    // One shadow-casting point light offered to the planner this frame.
    struct Candidate
    {
        uint32_t SortId = 0;            // stable per-light id (ExtractedLight::SortId): hysteresis key
        uint32_t ClusterIndex = 0;      // packed clusterable index == the receiver shader's light index
        float CoverageRadiusPx = 0.0f;  // screen radius of the light's range sphere: tier + importance driver
        uint32_t ExplicitTier = 0;      // Light.ShadowResolutionTier as uint; 0 = Inherit (coverage-driven)
        uint32_t InheritResolution = 0; // node punctual resolution, used when the tier resolves to Inherit
        // L1a content signature: a stable hash of everything PER-LIGHT that makes
        // the cached depth stale, EXCEPT the committed tier, the caster epoch and
        // the LOD key (which the planner folds in). The caller mixes the light's
        // WORLD position + range + the 6-bit camera face mask — never a
        // camera-relative value, or every camera move would defeat the cache.
        uint64_t ContentHash = 0;
        // L1b caster-proximity keying: the light's WORLD-space influence sphere
        // {x, y, z, range}. A caster change dirties this light only when the
        // changed caster's bounds intersect this sphere — an occluder must sit
        // between the light and a lit receiver, both within `range`, so a caster
        // farther than range can never alter this light's shadow map. Same
        // world-space discipline as ContentHash (never camera-relative).
        float InfluenceSphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    // Per-frame L1a caching inputs. Bundled so the tier/slot Plan signature stays
    // readable. Default-constructed = caching OFF (kill switch / tests that only
    // exercise tier/slot logic), so every committed slot reports NeedsRender.
    struct CacheInputs
    {
        uint64_t CasterEpoch = 0;                          // per-world shadow-caster content version
        uint32_t RefreshBudget = kDefaultPointShadowRefreshBudget; // max dirty slots re-rendered/frame
        bool Enabled = false;                              // GE_SHADOW_CACHE gate
        // L1b: the changed casters that explain the LATEST CasterEpoch advance
        // (world-space, old ∪ new bounds per caster). Valid only when Unattributed
        // is false AND the planner observed the immediately preceding epoch — any
        // other transition (first sight, multi-step jump, full-lane frame, list
        // overflow) is treated as affecting every light. Span storage must outlive
        // the Plan() call; it is not retained.
        std::span<const ShadowCasterChangeSphere> ChangedCasters{};
        // True => the epoch advance cannot be attributed to specific casters
        // (structural / full-lane change, vertex-mod caster animation, sphere-list
        // overflow): every candidate light is treated as affected — exactly the
        // pre-L1b world-scoped behavior. Default true so callers that never
        // attribute keep today's semantics.
        bool Unattributed = true;
        // LOD selection knobs in force for this view's point-face slices. A
        // change re-renders every slot in the view: the knobs are global, so
        // every retained slot's levels are equally stale.
        PointShadowLodKey Lod{};
    };

    // One committed atlas slot. Slot index == its position in PlanResult::Slots.
    struct SlotAssignment
    {
        uint32_t SortId = 0;
        uint32_t ClusterIndex = 0;
        uint32_t Tier = 0;           // committed tier (1=Low, 2=Medium, 3=High)
        uint32_t TileResolution = 0; // ResolvePointShadowTileResolution(Tier, inheritRes)
        // L1a: false => this slot's content is cached; skip its face passes and
        // sample the retained atlas layers. Always true when CacheInputs::Enabled
        // is false.
        bool NeedsRender = true;
        PointShadowDirtyCause DirtyCause = PointShadowDirtyCause::FirstRender;
    };

    struct PlanResult
    {
        // Slots are STICKY (a light keeps its index across frames), so the range
        // [0, SlotCount) can contain gaps left by an eviction. An occupied slot
        // has TileResolution != 0; a gap slot is default-constructed (0). Callers
        // scan [0, SlotCount) and skip TileResolution==0.
        std::array<SlotAssignment, kMaxPointShadowSlots> Slots{};
        uint32_t SlotCount = 0; // one past the highest occupied slot
    };

    // Advance the persistent state one frame and return this frame's committed
    // slot set. `budget` clamps to kMaxPointShadowSlots; `cooldownFrames` freezes
    // a light's tier + slot after a change. `cache` drives L1a per-(view,slot)
    // render caching (default: OFF, every slot renders). Determinism: candidates
    // are ranked by CoverageRadiusPx desc, SortId asc tie-break.
    PlanResult Plan(std::span<const Candidate> candidates, uint32_t budget,
                    uint32_t cooldownFrames, uint64_t frameIndex,
                    const CacheInputs& cache);

    // Compatibility overload for callers that do not enable shadow caching.
    PlanResult Plan(std::span<const Candidate> candidates, uint32_t budget,
                    uint32_t cooldownFrames, uint64_t frameIndex);

    // Drop all persistent state (device rebuild / view teardown).
    void Reset();

    // Drop ONLY the per-(view,slot) render cache, forcing every slot to re-render
    // (FirstRender) while keeping the tier/slot assignment. The caller must invoke
    // this whenever the atlas PHYSICAL texture may have been recreated underneath a
    // cached slot — the render key tracks content identity, not GPU-resource
    // lifetime, so a fresh/Undefined physical (pool idle-eviction after the atlas
    // went un-imported, or a budget realloc changing arrayLayers) would otherwise
    // be sampled as garbage while the key still matches.
    void InvalidateRenderCache();

    // Committed tier for a light (test/introspection). 0 if the light is unknown.
    uint32_t CommittedTier(uint32_t sortId) const;

  private:
    struct LightState
    {
        uint32_t Tier = 0;         // last committed tier
        int32_t Slot = -1;         // atlas slot (-1 = not admitted)
        // Two INDEPENDENT cooldowns (design §4.4 — "cooldown on tier transitions
        // AND budget in/out"): the tier freeze must not also pin the slot, nor an
        // admission freeze pin the tier (conflating them blocks a re-tier the
        // frame after admission).
        uint32_t TierCooldown = 0; // frames until the next tier change is allowed
        uint32_t SlotCooldown = 0; // frames a fresh admission is protected from eviction
        uint64_t LastFrame = 0;    // frame this light was last a candidate (staleness reap)
        // L1b: the caster epoch of the last change RELEVANT to this light — the
        // last epoch whose changed-caster bounds intersected its influence sphere
        // (or that was unattributed). The render-cache dirty test compares the
        // slot's rendered epoch against THIS, not the global epoch, so a caster
        // moving out of range of every other light re-renders only its own.
        uint64_t RelevantCasterEpoch = 0;
    };

    // L1a per-(view,slot) rendered content record — keyed by SLOT, not SortId, so
    // it survives a light dropping out of the candidate set (m_States is reaped on
    // drop-out). A re-tenant is caught by the SortId field. NOTE: this record tracks
    // CONTENT identity only, NOT the atlas texture's GPU lifetime — a slot whose
    // key still matches is NOT safe to skip if the physical atlas was recreated
    // (pool idle-eviction / budget realloc / device rebuild). That guard lives in
    // the caller (RenderServices::EnsurePointShadowAssignment calls
    // InvalidateRenderCache on an import gap or budget change; OnDeviceRebuilt
    // Reset()s the planner).
    struct SlotRenderState
    {
        uint32_t SortId = 0;      // tenant whose content is cached in this slot's layers
        uint64_t ContentHash = 0; // Candidate::ContentHash at last render
        uint32_t Tier = 0;        // committed tier at last render
        uint64_t CasterEpoch = 0; // the tenant's RELEVANT caster epoch at last render (L1b)
        PointShadowLodKey Lod{};  // LOD selection knobs at last render
        bool Rendered = false;    // false => never rendered => always dirty
    };

    // Coverage → tier with per-current-tier hysteresis thresholds; `frozen`
    // pins the tier (tier cooldown active). New lights (currentTier 0) initialize
    // directly from coverage.
    static uint32_t DesiredTier(float coverageRadiusPx, uint32_t currentTier, bool frozen);

    std::unordered_map<uint32_t, LightState> m_States;                 // by SortId
    std::array<SlotRenderState, kMaxPointShadowSlots> m_SlotRenderStates{}; // by slot index
    // L1b: the caster epoch this planner last processed. ChangedCasters is applied
    // only across an exactly-one-step advance from here; any other transition
    // globalizes (the list cannot explain epochs this planner never saw).
    uint64_t m_LastCasterEpoch = 0;
    bool m_CasterEpochSeen = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
