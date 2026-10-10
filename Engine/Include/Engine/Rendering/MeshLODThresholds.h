#pragma once

// Screen-space-error (SSE) LOD switch-point derivation, seeded into the
// GPUMesh row consumed by ge_SelectLOD in draw_command_scatter.comp.
//
// Generated chains select the coarsest level whose projected error fits a
// per-view pixel budget. For LOD k+1 with achieved simplify error e (meshopt
// error, normalized to the mesh's max AABB axis; attribute deviation and the
// chart-amalgamation / silhouette tripwires fold in via max — see
// MeshLODGenerator.cpp), the projected error in pixels at distance d is
//
//   errPx = e * maxExtent * (projScaleY / d) * viewportH / 2
//
// The scatter already computes coverage = boundingRadius * projScaleY / d, so
// the scan's "stay at LOD k" test (errPx of LOD k+1 still EXCEEDS the budget)
// becomes a plain coverage comparison:
//
//   coverage >= threshold * sseToCoverage,  where
//   threshold      = boundingRadius / (e * maxExtent)  (per-mesh, view-free)
//   sseToCoverage  = 2 * budgetPx / viewportH          (per-view push constant)
//
// Equality is exactly "LOD k+1's projected error spans budgetPx pixels".
// The switch therefore happens at a fixed per-mesh PIXEL size
// (spherePx = 2 * budgetPx * threshold) for every viewport at or above the
// mesh's break-even height: a 4K view holds detail to a longer world distance
// automatically, and shrinking a window coarsens naturally. BELOW break-even
// the fixed-pixel-size rule would coarsen past the coverage mapping this path
// replaced, so sseToCoverage is capped per mesh at LodSseScaleCeil and the slot
// switches at a fixed COVERAGE instead — see that function for the derivation.
// LODGroup bias keeps its old semantics on both sides of the cap: exp2(bias)
// multiplies coverage, which for an uncapped SSE slot is equivalent to dividing
// the budget by exp2(bias) — positive bias keeps detail in both schemes.
//
// Slots WITHOUT a trustworthy error metric keep plain coverage-space
// thresholds (no per-view scale): artist-authored chains (default table or
// authored coverages), zero-error slots (legacy / procedural meshes carry no
// error data), sloppy levels (their error is on a different scale — the
// salience cap below), and degenerate bounds. GPUMesh.lodFlags carries one
// SSE bit per slot so a single chain can mix both spaces; the scatter picks
// the comparison space per slot.
//
// MIXED-SPACE DESCENT IS ENFORCED AT SELECTION TIME, NOT HERE. The scan is
// only sound when the EFFECTIVE (coverage-space) thresholds descend, and the
// two spaces have no registration-time ordering — sseToCoverage is per-view,
// so a chain that descends in one view need not in another. ge_SelectLOD
// therefore converts every slot to coverage space and applies the descend
// clamp inline, which is correct by construction for any mix and a no-op for
// an already-descending row (so authored/all-coverage rows are byte-identical
// to the pre-SSE path). Enforcing it only within same-space runs here would
// leave a mid level UNREACHABLE at every coverage whenever a sloppy slot
// precedes a near-lossless SSE slot, silently promoting the next COARSER
// level — a quality loss and a two-level pop, not a conservative fallback.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace GameEngine
{
namespace Rendering
{

// Which mapping seeds the GPUMesh switch points. All paths stay compiled so a
// selection change can be A/B'd inside ONE process at one GPU clock state:
// comparing two builds across sessions was measured to carry a whole-session
// offset of up to 19% at a fixed pose, larger than the effect under test.
// MeshGPURegistry::SetLodSelectionMode re-derives every registered row.
//
// Coverage is the pre-SSE mapping kept byte-exact (DeriveLODThresholdsCoverage),
// so the reference arm is the real previous behaviour rather than "SSE with the
// scale removed" — the two disagree on zero-error slots, where coverage yields
// kLodCoverageErrorScale/floor and the SSE path falls back to defaultThresholds.
//
// Off zeroes every switch point (DeriveLODThresholdsOff), so ge_SelectLOD's scan
// matches on its first iteration and every instance draws LOD0 in every view,
// shadow cascades included. It is NOT expressible as a budget: a zero budget
// drives only SSE-FLAGGED slots to LOD0 and leaves authored, sloppy and
// zero-error slots switching on their own coverages, while an unbounded budget
// clamps to kLodThresholdCeil and engages levels EARLIER.
enum class LodSelectionMode : std::uint32_t
{
    Coverage = 0,
    Sse      = 1,
    Off      = 2,
};

// Persisted / debug token for a selection mode. The project setting
// rendering.lodMode and the set_lod IPC command spell it the same way, so the
// mapping lives with the enum instead of once per caller.
inline constexpr std::string_view kLodSelectionModeCoverageToken = "coverage";
inline constexpr std::string_view kLodSelectionModeSseToken      = "sse";
inline constexpr std::string_view kLodSelectionModeOffToken      = "off";

inline constexpr std::string_view ToString(LodSelectionMode mode)
{
    switch (mode)
    {
        case LodSelectionMode::Coverage: return kLodSelectionModeCoverageToken;
        case LodSelectionMode::Off:      return kLodSelectionModeOffToken;
        case LodSelectionMode::Sse:      break;
    }
    return kLodSelectionModeSseToken;
}

// False (leaving outMode untouched) for an unrecognized token, so a caller can
// reject a typo instead of silently selecting a mode nobody asked for.
inline constexpr bool TryParseLodSelectionMode(std::string_view token, LodSelectionMode& outMode)
{
    if (token == kLodSelectionModeCoverageToken)
        outMode = LodSelectionMode::Coverage;
    else if (token == kLodSelectionModeSseToken)
        outMode = LodSelectionMode::Sse;
    else if (token == kLodSelectionModeOffToken)
        outMode = LodSelectionMode::Off;
    else
        return false;
    return true;
}

// Coverage-mode threshold = kLodCoverageErrorScale / achieved-error. Anchor:
// scale 0.02 with the floor errors {.04,.10,.20} yields {.5,.2,.1}, reproducing
// the shipped defaults for LOD1/LOD2 and normalizing LOD3's hand-tuned .08 to
// .1. Calibrated to the cook's attribute weights (MeshLODConfig::NormalWeight
// 2.0) in lockstep with kLodErrorFloor — change the weights and both together.
inline constexpr float kLodCoverageErrorScale = 0.02f;
// Divide-by-zero guard, below every entry of kLodErrorFloor.
inline constexpr float kMinLodError = 0.001f;
// Per-slot floor on the achieved error fed into the SSE mapping, indexed like
// outThreshold (slot k floors LOD k+1's error). These pin the per-slot ANCHOR
// switch size — the LARGEST on-screen size at which each slot may engage
// (spherePx = 2 * budgetPx / floor, modulo the mesh's extent/radius shape
// factor): at the default 10 px budget, 500 / 200 / 100 px for a mesh whose
// maxExtent equals its bounding diameter.
//
// The floor exists because achieved error UNDERREPORTS engagement cost:
// meshopt frequently stops at the RATIO budget with an achieved error far
// below the ERROR budget, and crediting that surplus drives the derived
// threshold toward infinity — 1 / e is unbounded as e -> 0. Two cooked
// corpora measured 2026-07-25 put every non-sloppy generated level below its
// slot's floor (ElvenRealm v3: median e ~ 1e-4; Synty AncientEmpire: slot0
// median 0.0123, max 0.0200), and a floorless mapping would have made the
// coarser level engage at EVERY on-screen size for 61-100% of them — LOD0
// unreachable, the exact failure that introduced this floor. A
// better-than-floor error therefore never engages a level earlier than the
// slot's anchor; only worse-than-floor errors adapt, by delaying.
//
// Decoupled from the cook budgets since the mid-tier arc: budgets
// (MeshLODConfig::TargetError, 0.06/0.16/0.35) say how far the simplifier MAY
// go, floors say how early the result may ENGAGE. Calibrated to the cook's
// attribute weights (MeshLODConfig::NormalWeight 2.0) — the achieved error
// folds weighted attribute deviation, so raising weights inflates every error
// by roughly the same factor. Change the weights and these together.
inline constexpr float kLodErrorFloor[] = {0.04f, 0.10f, 0.20f, 0.20f};
// Hard ceiling on any SSE slot's EFFECTIVE coverage switch point, applied in
// the scatter (where the per-view budget is known). 0.5 is the default
// LOD0->LOD1 switch (MeshGPURegistry::kDefaultLODThresholds[0]) — the earliest
// any LOD may engage under the tuned default table. With the floor in place no
// default-budget cook reaches it; it guards custom budgets. Deliberately NOT
// applied to coverage-space slots: authored coverages were never ceiling-capped
// and must stay byte-identical.
inline constexpr float kLodThresholdCeil = 0.5f;
// Cap for slots that switch INTO a sloppy level. Sloppy errors are on a
// different scale, so those slots fall back to defaultThresholds — but the
// generator's skip-unemittable-levels rule can land a sloppy level in an
// EARLY slot (e.g. a one-level chain [LOD0, sloppy] on an open shell whose
// locked mid levels all skipped), and the early defaults (0.5/0.2) would
// engage the crude border-moving level at half-screen coverage. A sloppy
// level never engages above this coverage, whatever slot it fills —
// including the full-chain far slot (default 0.08), which this cap also
// pulls down. Applies to every generated sloppy level regardless of its
// error value — chain provenance is the caller's explicit authoredChain
// flag, never inferred from the error (a degenerate cook can legitimately
// achieve a sloppy error of exactly 0.0).
//
// History: shipped at 0.1; dropped to 0.03 (2026-07-21 triage) when sloppy
// shells were proven ATTRIBUTE-corrupted — position-only simplify over
// original indices smeared UVs across atlas islands (rainbow noise) and
// inherited garbage normals (black facets). Generator v4 rebuilt sloppy
// levels as attribute-honest own-vertex shells (per-face flat normals,
// area-dominant island-snapped UVs), deleting the chromatic damage class —
// yet the cap STAYS 0.03: the re-raise experiment (real 0.06 + emulated
// 0.05/0.045 at the triage poses) showed the residual artifact class,
// thin multi-chart props amalgamating into their largest chart (banners
// fading to plaster-white, moored boats to pale hulls), remains salient
// down to ~0.03 regardless of shell honesty. The salience floor is a
// property of one-step LOD chains on that prop family, not of shell
// quality; raising the cap re-opens the complaint for ~0.3-0.7 ms at the
// worst pose. Re-raising is a deliberate act gated on real mid LOD levels
// (the seam-aware mid-tier arc) plus fresh A/B evidence at the triage
// poses (SloppyCapValuePinnedAtSalienceFloor pins the contract).
inline constexpr float kLodSloppyThresholdCap = 0.03f;
// Strict-descend clamp step: a slot that is not strictly below its predecessor
// is pulled down to predecessor * this factor (keeps values positive).
inline constexpr float kLodThresholdDescendFactor = 0.5f;

// --- Per-view SSE scale ------------------------------------------------------

// Default projected-error budget, in render-target pixels, for generated
// chains. One knob: raising it coarsens everything proportionally, in pixels.
//
// NOT behaviour-neutral against the coverage table it replaces, and cannot be.
// The old table switched at a fixed COVERAGE, which is a fixed pixel error only
// for one mesh shape: the budget a mesh implicitly spent was 0.01 * viewportH *
// (maxExtent / boundingRadius), i.e. proportional to its own shape factor. That
// factor spans [2/sqrt(3), 2] by construction, and over the 435 own-vertex
// AABBs in the ElvenRealm cook (measured 2026-07-25) it ran min 1.16, median
// 1.54, p90 1.86 — so the old table really spent 10.5 to 18.0 px, median 13.9,
// at a 902 px viewport. Collapsing that spread to one number is the POINT of
// the reformulation; which number is a cost/quality choice:
//
//   10.0  no mesh coarsens versus the old table (the compact end is the corpus
//         MINIMUM, so this is the conservative bound), but the median mesh
//         holds LOD0 ~1.4x further at 902 px and ~1.7x at 1080p.
//   13.9  median mesh unchanged at 902 px; compact meshes coarsen slightly,
//         elongated ones still hold longer.
//
// 10.0 is the quality-safe end and is DELIBERATELY UNTUNED pending a
// current-generator re-cook: every corpus measured predates generator v5-v9,
// whose amalgamation/silhouette tripwires raise achieved errors, moving the
// population off the floor and changing what the budget actually governs. The
// cost is real — median LOD0/LOD1 ratio is 14.5x triangles — so revisit this
// with a runtime A/B, not by argument.
//
// The resolution dependence is intended, not a side effect: the switch holds a
// fixed on-screen error, so a 4K view legitimately holds detail ~2.5-3.3x
// further in world distance than a 1080p one. It is intended only UPWARD — the
// budget is a fraction 2*budgetPx/viewportH of the image, so a short view would
// spend it on a proportionally larger share of the frame; LodSseScaleCeil floors
// that at the coverage mapping. With this budget the break-even heights are
// 2*10/(0.02*phi) = 500 px (phi 2.0) to 866 px (phi 2/sqrt(3)).
inline constexpr float kDefaultLodErrorBudgetPx = 10.0f;
// Budget multiplier for the tight class (skinned/character chains). Character
// erosion is salient well below the prop budget: the soldier sweep read clean
// at 153 px on-screen height (errPx = 0.0492 * 153 ~= 7.5) and eroded at
// 230 px (~11.3), so the tight class spends 10.0 * 0.75 = 7.5 px, landing the
// soldier switch at ~152 px. This multiplier is the per-class-floor ask
// expressed in the same budget mechanism.
inline constexpr float kDefaultLodSkinnedBudgetScale = 0.75f;
// Per-view viewport height fallback for the first frame(s) of a view, before
// its world pass has published a render-target height.
inline constexpr uint32_t kLodSseFallbackViewportH = 1080u;

// The per-view factor that converts an SSE-normalized threshold into the
// view's coverage space: effectiveCoverage = threshold * this. Passed to the
// scatter so selection compares one space (see the header comment).
// budgetPx <= 0 returns 0, the keep-detail fail-safe: every SSE slot then
// matches at any coverage, so a slice that never sets it (tests, tools) never
// coarsens through an SSE slot.
inline constexpr float LodSseThresholdToCoverage(std::uint32_t viewportH, float budgetPx)
{
    if (budgetPx <= 0.0f)
        return 0.0f;
    const std::uint32_t h = viewportH != 0u ? viewportH : kLodSseFallbackViewportH;
    return 2.0f * budgetPx / static_cast<float>(h);
}

// Per-mesh ceiling on the factor above, in the same units: the factor at which
// an SSE slot engages at EXACTLY the coverage mapping's switch point for that
// slot. Carried on the GPUMesh row (GPUScene.h::lodSseScaleCeil) and applied by
// the scatter, so a short viewport pins to the coverage mapping instead of
// coarsening past it.
//
// A slot's effective switch point is
//   threshold * sseToCoverage = [boundingRadius / (e * maxExtent)] * 2*budgetPx/viewportH
// and the coverage mapping's value for the same floored error e is
//   kLodCoverageErrorScale / e.
// Those are equal exactly when sseToCoverage == kLodCoverageErrorScale * phi,
// where phi = maxExtent / boundingRadius is the mesh's own shape factor, so the
// two mappings cross at viewportH = 2*budgetPx / (kLodCoverageErrorScale * phi)
// — this mesh's break-even height. Clamping the factor here therefore makes the
// effective switch point min(SSE, coverage) EXACTLY: above break-even the view
// factor is the smaller of the two and nothing changes, below it the slot holds
// the coverage mapping's switch point. On a row whose switch slots are all
// SSE-normalized, no slot can select coarser than the mapping it replaced, at
// any viewport height. A mixed row — a sloppy or zero-error level in a
// non-terminal slot keeps a coverage-space threshold among SSE slots — is
// outside that guarantee: the descend clamp (LodEffectiveThreshold) can couple
// an SSE slot to its coverage predecessor.
//
// phi is the TRUE per-mesh value, not the [2/sqrt(3), 2] worst case, which is
// why this is exact rather than an over-hold: a per-view scalar clamp cannot
// know phi and must assume the compact end, holding every non-cube mesh up to
// 1.73x too long.
//
// Returns 0 for degenerate bounds, which the scatter reads as "no ceiling" —
// such a mesh carries no SSE slot for a ceiling to act on (DeriveLODThresholds
// routes it to defaultThresholds in coverage space).
inline constexpr float LodSseScaleCeil(float boundingRadius, float maxExtent)
{
    if (boundingRadius <= 0.0f || maxExtent <= 0.0f)
        return 0.0f;
    return kLodCoverageErrorScale * maxExtent / boundingRadius;
}

// --- Per-view-class budget override ------------------------------------------

// One view class's override of the global error budget, as a percentage of it.
// This is a LAYER over the global budget and mode, not a second selection
// system: a class whose override is disabled spends the global budget
// unchanged, so turning every override off is byte-identical to having none.
//
// Percent rather than pixels because the global budget stays the single
// quality dial — retuning it moves every class together, and a class says only
// how much of it to spend.
struct LodViewBudgetOverride
{
    bool  Enabled = false;
    float BudgetPercent = 100.0f;
};

// 100 = the global budget; below keeps more detail (finer), above coarsens.
// The span is the useful range either side of the global default: at the
// default 10 px budget, 10% is a 1 px budget (effectively LOD0-everywhere for
// error-metric slots) and 400% is 40 px, coarser than any value the cooked
// corpora were tuned against. Deliberately NOT clamped against the editor's
// 0..100 px persistence range for the product: that bound guards the stored
// global budget, while the effective budget is already bounded downstream by
// kLodThresholdCeil and, per mesh, by LodSseScaleCeil.
inline constexpr float kMinLodBudgetPercent = 10.0f;
inline constexpr float kMaxLodBudgetPercent = 400.0f;
inline constexpr float kDefaultLodBudgetPercent = 100.0f;

// The budget a view of this class actually spends. A disabled override returns
// the global budget UNCHANGED rather than applying 100%, so a stale stored
// percent cannot perturb a view whose override is off.
inline constexpr float ResolveLodBudgetPx(float globalBudgetPx,
                                          const LodViewBudgetOverride& classOverride)
{
    if (!classOverride.Enabled)
        return globalBudgetPx;
    const float percent =
        std::clamp(classOverride.BudgetPercent, kMinLodBudgetPercent, kMaxLodBudgetPercent);
    return globalBudgetPx * (percent / 100.0f);
}

// --- Derivation ---------------------------------------------------------------

// Enforce strictly-descending thresholds across the derived (non-coarsest)
// slots. ge_SelectLOD picks the first k with coverage >= outThreshold[k], which
// is only sound when the thresholds descend; neither achieved meshopt errors nor
// authored coverages are guaranteed monotonic. A slot that is not strictly below
// its predecessor is pulled to predecessor * kLodThresholdDescendFactor. The
// coarsest present slot (index lodCount-1) is left as its caller set it. Used
// by the authored direct-coverage path, which is single-space and so CAN be
// ordered at registration; the mixed-space derived path defers to the scan
// (LodEffectiveThreshold).
inline void ClampLODThresholdsDescending(float* outThreshold,
                                         std::uint32_t lodCount,
                                         std::uint32_t maxLods)
{
    for (std::uint32_t k = 1u; k + 1u < lodCount && k < maxLods; ++k)
    {
        if (outThreshold[k] >= outThreshold[k - 1u])
            outThreshold[k] = outThreshold[k - 1u] * kLodThresholdDescendFactor;
    }
}

// The effective coverage-space switch point for one slot, and the descend
// clamp the ge_SelectLOD scan depends on. MIRRORED IN
// draw_command_scatter.comp::ge_SelectLOD — keep the two in step.
//
// `prevEff` is the previous slot's effective threshold (kLodEffNone before the
// first slot). SSE slots convert with the per-view sseToCoverage factor, capped
// at the mesh's own sseScaleCeil (LodSseScaleCeil — 0 means no cap) so a short
// viewport never scales them past the coverage mapping, and take the ceiling;
// coverage slots pass through untouched. A slot that is not strictly below its
// predecessor is pulled to predecessor * the descend factor, exactly as
// ClampLODThresholdsDescending does at registration — so an already-descending
// row is unchanged.
inline constexpr float kLodEffNone = 3.402823466e38f; // FLT_MAX: no predecessor

inline constexpr float LodEffectiveThreshold(float threshold, bool sseSlot,
                                             float sseToCoverage, float sseScaleCeil,
                                             float prevEff)
{
    float eff = threshold;
    if (sseSlot)
    {
        const float scale = (sseScaleCeil > 0.0f && sseScaleCeil < sseToCoverage)
                                ? sseScaleCeil
                                : sseToCoverage;
        eff = threshold * scale;
        eff = eff < kLodThresholdCeil ? eff : kLodThresholdCeil;
    }
    if (eff >= prevEff)
        eff = prevEff * kLodThresholdDescendFactor;
    return eff;
}

// LodSelectionMode::Off. Every slot's switch point is 0, which ge_SelectLOD's
// scan matches on its first iteration at any coverage, so the row always draws
// LOD0. Callers set lodFlags to 0: with no SSE bit there is nothing for the
// per-view scale to move, so the row is view-independent and shadow cascades
// draw LOD0 too. Applies to authored chains as well — this path replaces the
// authored-coverage seeding, not just the derived mappings.
//
// The per-view force level (RenderServices::SetLODForceLevel) is orthogonal and
// still wins: ge_SelectLOD short-circuits on it before the scan, so a debug pin
// to LOD2 still inspects LOD2 with selection off.
inline void DeriveLODThresholdsOff(std::uint32_t maxLods, float* outThreshold)
{
    for (std::uint32_t k = 0; k < maxLods; ++k)
        outThreshold[k] = 0.0f;
}

// LodSelectionMode::Coverage. The pre-SSE empirical mapping, transplanted
// unchanged from the revision before SSE selection landed so that the reference
// arm of an A/B is the real previous behaviour. Every slot is coverage space, so
// there is no slot mask: callers set lodFlags to 0 and ge_SelectLOD's convert-
// and-clamp step is inert (this path orders the row here, and the coarsest slot
// is 0 which never trips the clamp).
//
// DO NOT "unify" this with DeriveLODThresholds. They deliberately disagree on
// zero-error slots — here e floors to kLodErrorFloor and the slot becomes
// scale/floor, while the SSE path routes no-metric slots to defaultThresholds
// (0.1 vs 0.08 at slot 2). Folding them would silently change the reference.
inline void DeriveLODThresholdsCoverage(const float* lodError,
                                       const std::uint8_t* lodSloppy,
                                       std::uint32_t lodCount,
                                       bool authoredChain,
                                       const float* defaultThresholds,
                                       std::uint32_t maxLods,
                                       float* outThreshold)
{
    for (std::uint32_t k = 0; k < maxLods; ++k)
    {
        if (k + 1u < lodCount)
        {
            if (authoredChain)
            {
                // No-metric artist chain: reproduce the default table exactly.
                outThreshold[k] = defaultThresholds[k];
            }
            // A finer-than-coarsest level: derive from LOD k+1's error unless
            // that level fell back to the sloppy simplifier (different scale).
            else if (lodSloppy[k + 1u] != 0u)
            {
                // Generated sloppy levels must not engage early whatever slot
                // they fill — capped unconditionally, whatever their error.
                outThreshold[k] =
                    std::min(defaultThresholds[k], kLodSloppyThresholdCap);
            }
            else
            {
                const std::uint32_t floorSlot = std::min<std::uint32_t>(
                    k, static_cast<std::uint32_t>(std::size(kLodErrorFloor)) - 1u);
                const float e = std::max(lodError[k + 1u],
                                         std::max(kMinLodError, kLodErrorFloor[floorSlot]));
                outThreshold[k] =
                    std::clamp(kLodCoverageErrorScale / e, 0.0f, kLodThresholdCeil);
            }
        }
        else
        {
            // Coarsest present level (and any absent slots) always match.
            outThreshold[k] = 0.0f;
        }
    }

    // The coarsest slot is already 0; enforce descent across the derived slots.
    ClampLODThresholdsDescending(outThreshold, lodCount, maxLods);
}

// LodSelectionMode::Sse. Derive per-slot switch points from achieved simplify
// errors. Returns the
// SSE slot mask (bit k set = outThreshold[k] is SSE-normalized and the scatter
// must scale coverage by the per-view sseScale before comparing; bit clear =
// plain coverage space, compared exactly as before this change).
//
// lodError / lodSloppy are indexed by ABSOLUTE LOD level (lodError[0]==0 for
// LOD0). outThreshold[k] governs leaving LOD k for LOD k+1, so it is derived
// from the error of LOD k+1:
//   - SSE slot:  boundingRadius / (max(err, kLodErrorFloor[k]) * maxExtent)
//   - sloppy:    min(defaultThresholds[k], kLodSloppyThresholdCap)  [coverage]
//   - no data (err == 0) or degenerate bounds: defaultThresholds[k] [coverage]
// defaultThresholds is the descending fallback table; the coarsest present
// slot and absent slots stay 0 (always match). All arrays have `maxLods`
// capacity.
//
// No descend clamp runs here: the derived row can mix comparison spaces, which
// have no registration-time ordering, so ge_SelectLOD applies the clamp after
// converting every slot to coverage space (LodEffectiveThreshold above).
//
// authoredChain marks an artist-authored chain (Mesh::HasAuthoredLODs()):
// artist quality carries no meshopt metric, so every present switch reproduces
// the tuned default table exactly (amendment #13), all in coverage space.
// Provenance is EXPLICIT — it must never be inferred from error values,
// because a generated sloppy level can legitimately achieve error 0.0
// (ElvenRealm v3 corpus: a 1-triangle sloppy cook with error exactly 0.0).
// Authored chains WITH switch coverages take ApplyAuthoredLODCoverage instead
// and never reach this path.
inline std::uint32_t DeriveLODThresholds(const float* lodError,
                                         const std::uint8_t* lodSloppy,
                                         std::uint32_t lodCount,
                                         bool authoredChain,
                                         float boundingRadius,
                                         float maxExtent,
                                         const float* defaultThresholds,
                                         std::uint32_t maxLods,
                                         float* outThreshold)
{
    const bool boundsUsable = boundingRadius > 0.0f && maxExtent > 0.0f;
    std::uint32_t sseSlotMask = 0u;
    for (std::uint32_t k = 0; k < maxLods; ++k)
    {
        if (k + 1u >= lodCount)
        {
            // Coarsest present level (and any absent slots) always match.
            outThreshold[k] = 0.0f;
            continue;
        }
        if (authoredChain)
        {
            // No-metric artist chain: reproduce the default table exactly.
            outThreshold[k] = defaultThresholds[k];
        }
        else if (lodSloppy[k + 1u] != 0u)
        {
            // Generated sloppy levels must not engage early whatever slot
            // they fill — capped unconditionally, whatever their error.
            outThreshold[k] =
                std::min(defaultThresholds[k], kLodSloppyThresholdCap);
        }
        else if (lodError[k + 1u] <= 0.0f || !boundsUsable)
        {
            // No error data (legacy / procedural / degenerate bounds):
            // reproduce the default table for this slot.
            outThreshold[k] = defaultThresholds[k];
        }
        else
        {
            // Floor the achieved error before inverting it: 1/e is unbounded
            // as e -> 0, and meshopt routinely stops at the ratio budget with
            // an error far below what the level actually costs to engage.
            const std::uint32_t floorSlot = std::min<std::uint32_t>(
                k, static_cast<std::uint32_t>(std::size(kLodErrorFloor)) - 1u);
            const float e = std::max(lodError[k + 1u],
                                     std::max(kMinLodError, kLodErrorFloor[floorSlot]));
            outThreshold[k] = boundingRadius / (e * maxExtent);
            sseSlotMask |= 1u << k;
        }
    }

    return sseSlotMask;
}

// Seed switch points directly from artist-authored coverages (the Phase C2
// direct-coverage path). `coverage` is parallel to Mesh::ExtraLODs, so
// coverage[k] is the screen coverage below which LOD k+1 engages — i.e. it
// governs leaving LOD k, the same index space as outThreshold (amendment #13:
// coverage[0] governs the LOD0->LOD1 switch). Missing entries, the coarsest
// present slot, and absent slots stay 0 (always match). Applies the
// strict-descending clamp at registration — authored coverages are not
// guaranteed monotonic, and this path is single-space so it can be ordered
// here. Authored chains carry no meshopt error metric, so this replaces the
// error-derived mapping wholesale when coverage is present. Always plain
// coverage space (lodFlags stays 0), and the scan's clamp is then a no-op:
// authored behavior is byte-identical to before the SSE change.
inline void ApplyAuthoredLODCoverage(const float* coverage,
                                     std::uint32_t coverageCount,
                                     std::uint32_t lodCount,
                                     std::uint32_t maxLods,
                                     float* outThreshold)
{
    for (std::uint32_t k = 0; k < maxLods; ++k)
        outThreshold[k] = (k + 1u < lodCount && k < coverageCount) ? coverage[k] : 0.0f;
    ClampLODThresholdsDescending(outThreshold, lodCount, maxLods);
}

} // namespace Rendering
} // namespace GameEngine
