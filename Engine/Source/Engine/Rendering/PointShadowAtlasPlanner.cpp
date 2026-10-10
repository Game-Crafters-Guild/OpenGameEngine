#include "Engine/Rendering/PointShadowAtlasPlanner.h"

#include "Engine/Rendering/ShadowFrameInfo.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
// Coverage tier boundaries (screen pixel radius of the light's range sphere).
// Initialization uses the midpoint boundary; steady-state transitions add a
// margin so promote/demote are asymmetric (the hysteresis dead-band). A light
// whose coverage oscillates within a dead-band keeps its tier.
constexpr float kInitLowMediumPx = 128.0f;
constexpr float kInitMediumHighPx = 384.0f;
constexpr float kLowMediumMarginPx = 32.0f;  // promote >160, demote <96
constexpr float kMediumHighMarginPx = 64.0f; // promote >448, demote <320

// A challenger must exceed an incumbent's coverage by this factor to evict it
// from a full atlas — the budget-boundary hysteresis (design §4.4 / A7).
constexpr float kBudgetHysteresisFactor = 1.25f;

constexpr uint32_t kTierLow = 1u;
constexpr uint32_t kTierMedium = 2u;
constexpr uint32_t kTierHigh = 3u;

// L1b: does any changed caster's (old ∪ new) sphere intersect the light's
// influence sphere? Squared-distance compare; both are world-space.
bool AnyCasterIntersects(std::span<const ShadowCasterChangeSphere> casters,
                         const float lightSphere[4])
{
    for (const ShadowCasterChangeSphere& s : casters)
    {
        const float dx = s.X - lightSphere[0];
        const float dy = s.Y - lightSphere[1];
        const float dz = s.Z - lightSphere[2];
        const float rr = s.Radius + lightSphere[3];
        if (dx * dx + dy * dy + dz * dz <= rr * rr)
            return true;
    }
    return false;
}
} // namespace

uint32_t ResolvePointShadowTileResolution(uint32_t tier, uint32_t inheritResolution)
{
    const uint32_t res = ResolvePunctualShadowResolution(tier, inheritResolution);
    return std::min(res, kPointAtlasTileResolution);
}

float PointShadowTileScale(uint32_t tileResolution)
{
    return static_cast<float>(tileResolution) / static_cast<float>(kPointAtlasTileResolution);
}

PointShadowAtlasUV PointShadowTileToAtlasUV(float faceU, float faceV, uint32_t tileResolution)
{
    const float scale = PointShadowTileScale(tileResolution);
    // Half an ATLAS texel: clamp keeps a PCF tap at least this far from the tile
    // sub-rect edge so its bilinear comparison footprint never bleeds into the
    // cleared-far border outside the tile (the §4.5 "clamp to layer rect + gutter"
    // that replaces the pre-M1 hard [0,1] reject).
    const float halfTexel = 0.5f / static_cast<float>(kPointAtlasTileResolution);
    const float hi = scale - halfTexel;
    PointShadowAtlasUV out{};
    out.U = std::clamp(faceU * scale, halfTexel, hi);
    out.V = std::clamp(faceV * scale, halfTexel, hi);
    return out;
}

uint32_t PointShadowAtlasPlanner::DesiredTier(float coverageRadiusPx, uint32_t currentTier,
                                              bool frozen)
{
    if (currentTier == 0u) // new light: initialize directly from coverage
    {
        if (coverageRadiusPx > kInitMediumHighPx)
            return kTierHigh;
        if (coverageRadiusPx > kInitLowMediumPx)
            return kTierMedium;
        return kTierLow;
    }
    if (frozen)
        return currentTier;

    // Asymmetric thresholds relative to the current tier (dead-band = no change).
    switch (currentTier)
    {
    case kTierHigh:
        return coverageRadiusPx < (kInitMediumHighPx - kMediumHighMarginPx) ? kTierMedium : kTierHigh;
    case kTierMedium:
        if (coverageRadiusPx > (kInitMediumHighPx + kMediumHighMarginPx))
            return kTierHigh;
        if (coverageRadiusPx < (kInitLowMediumPx - kLowMediumMarginPx))
            return kTierLow;
        return kTierMedium;
    default: // kTierLow
        return coverageRadiusPx > (kInitLowMediumPx + kLowMediumMarginPx) ? kTierMedium : kTierLow;
    }
}

void PointShadowAtlasPlanner::Reset()
{
    m_States.clear();
    m_LastCasterEpoch = 0;
    m_CasterEpochSeen = false;
    // A device rebuild / view teardown recreates the atlas texture, so every
    // cached layer is gone — no slot may report "cached" until it re-renders.
    InvalidateRenderCache();
}

void PointShadowAtlasPlanner::InvalidateRenderCache()
{
    m_SlotRenderStates = {};
}

uint32_t PointShadowAtlasPlanner::CommittedTier(uint32_t sortId) const
{
    const auto it = m_States.find(sortId);
    return it != m_States.end() ? it->second.Tier : 0u;
}

PointShadowAtlasPlanner::PlanResult PointShadowAtlasPlanner::Plan(
    std::span<const Candidate> candidates, uint32_t budget, uint32_t cooldownFrames,
    uint64_t frameIndex)
{
    return Plan(candidates, budget, cooldownFrames, frameIndex, CacheInputs{});
}

PointShadowAtlasPlanner::PlanResult PointShadowAtlasPlanner::Plan(
    std::span<const Candidate> candidates, uint32_t budget, uint32_t cooldownFrames,
    uint64_t frameIndex, const CacheInputs& cache)
{
    budget = std::clamp(budget, 1u, kMaxPointShadowSlots);

    // ── Step A: reap lights that dropped out (free their slots) + tick cooldowns.
    // A light no longer offered this frame releases its slot; on re-admission it
    // starts fresh (Slot=-1, Tier re-initialized) — design §4.4 / A8.
    for (auto it = m_States.begin(); it != m_States.end();)
    {
        const bool present = std::any_of(
            candidates.begin(), candidates.end(),
            [&](const Candidate& c) { return c.SortId == it->first; });
        if (!present)
        {
            it = m_States.erase(it);
            continue;
        }
        if (it->second.TierCooldown > 0u)
            --it->second.TierCooldown;
        if (it->second.SlotCooldown > 0u)
            --it->second.SlotCooldown;
        it->second.LastFrame = frameIndex;
        ++it;
    }

    // Ensure a state exists for every candidate (new lights start uninitialized).
    // RelevantCasterEpoch seeds to the CURRENT epoch: a light first (re)offered
    // now must compare its slot record against the present epoch — any advance
    // that happened while it was absent is unattributable to it, so a stale
    // record mismatches and re-renders (never a hole), while an untouched record
    // from the same epoch stays cached.
    for (const Candidate& c : candidates)
    {
        auto [it, inserted] = m_States.try_emplace(c.SortId, LightState{});
        if (inserted)
        {
            it->second.LastFrame = frameIndex;
            it->second.RelevantCasterEpoch = cache.CasterEpoch;
        }
    }

    // ── Step A2: L1b caster-proximity keying. On an epoch advance, mark which
    // lights the change is RELEVANT to. The changed-caster list explains exactly
    // one advance from the epoch this planner last processed; every other
    // transition (first sight, multi-step jump while the view was inactive, an
    // unattributed change) conservatively affects every candidate — that is
    // precisely the pre-L1b behavior, so attribution can only remove work.
    // Runs regardless of cache.Enabled so a disable→enable window keeps the
    // per-light epochs current instead of forcing a global re-render.
    if (!m_CasterEpochSeen || cache.CasterEpoch != m_LastCasterEpoch)
    {
        const bool attributed = m_CasterEpochSeen &&
                                cache.CasterEpoch == m_LastCasterEpoch + 1u &&
                                !cache.Unattributed;
        for (const Candidate& c : candidates)
        {
            LightState& s = m_States[c.SortId];
            if (!attributed || AnyCasterIntersects(cache.ChangedCasters, c.InfluenceSphere))
                s.RelevantCasterEpoch = cache.CasterEpoch;
        }
        m_LastCasterEpoch = cache.CasterEpoch;
        m_CasterEpochSeen = true;
    }

    // ── Step B: commit tiers (explicit tier pins; Inherit is coverage-driven with
    // hysteresis + cooldown). A real change (not the initial commit) starts the
    // cooldown so the map is not re-tiered on the next frame.
    for (const Candidate& c : candidates)
    {
        LightState& s = m_States[c.SortId];
        uint32_t newTier;
        if (c.ExplicitTier != 0u)
            newTier = c.ExplicitTier;
        else
            newTier = DesiredTier(c.CoverageRadiusPx, s.Tier, s.TierCooldown > 0u);

        if (s.Tier != 0u && newTier != s.Tier && s.TierCooldown == 0u)
            s.TierCooldown = cooldownFrames;
        s.Tier = newTier;
    }

    // ── Step C: sticky slot allocation.
    // Rank candidates by coverage desc, SortId asc (deterministic).
    const uint32_t candCount = static_cast<uint32_t>(candidates.size());
    std::vector<uint32_t> ranked(candCount);
    for (uint32_t i = 0; i < candCount; ++i)
        ranked[i] = i;
    std::sort(ranked.begin(), ranked.end(),
              [&](uint32_t a, uint32_t b)
              {
                  const Candidate& ca = candidates[a];
                  const Candidate& cb = candidates[b];
                  if (ca.CoverageRadiusPx != cb.CoverageRadiusPx)
                      return ca.CoverageRadiusPx > cb.CoverageRadiusPx;
                  return ca.SortId < cb.SortId;
              });

    // Rebuild slot ownership from persistent state; a slot beyond the (possibly
    // shrunk) budget is released.
    std::array<int32_t, kMaxPointShadowSlots> slotOwner; // slot -> candidate index (-1 free)
    slotOwner.fill(-1);
    for (uint32_t ci = 0; ci < candCount; ++ci)
    {
        LightState& s = m_States[candidates[ci].SortId];
        if (s.Slot >= 0 && static_cast<uint32_t>(s.Slot) < budget)
            slotOwner[static_cast<uint32_t>(s.Slot)] = static_cast<int32_t>(ci);
        else
            s.Slot = -1; // released (budget shrank or never held)
    }

    auto coverageOf = [&](int32_t candIdx) {
        return candIdx >= 0 ? candidates[static_cast<uint32_t>(candIdx)].CoverageRadiusPx : -1.0f;
    };

    // If more incumbents survived than the budget (budget shrank at runtime),
    // evict the lowest-coverage incumbents until they fit.
    uint32_t occupied = 0;
    for (int32_t owner : slotOwner)
        if (owner >= 0)
            ++occupied;
    while (occupied > budget)
    {
        int32_t worstSlot = -1;
        for (uint32_t slot = 0; slot < budget; ++slot)
            if (slotOwner[slot] >= 0 &&
                (worstSlot < 0 ||
                 coverageOf(slotOwner[slot]) < coverageOf(slotOwner[static_cast<uint32_t>(worstSlot)])))
                worstSlot = static_cast<int32_t>(slot);
        if (worstSlot < 0)
            break;
        m_States[candidates[static_cast<uint32_t>(slotOwner[static_cast<uint32_t>(worstSlot)])].SortId]
            .Slot = -1;
        slotOwner[static_cast<uint32_t>(worstSlot)] = -1;
        --occupied;
    }

    // Fill free slots with the highest-ranked not-yet-admitted candidates. A fresh
    // admission arms the cooldown so it is not evicted again next frame (boundary
    // thrash guard).
    auto lowestFreeSlot = [&]() -> int32_t {
        for (uint32_t slot = 0; slot < budget; ++slot)
            if (slotOwner[slot] < 0)
                return static_cast<int32_t>(slot);
        return -1;
    };
    for (uint32_t r = 0; r < ranked.size(); ++r)
    {
        const uint32_t ci = ranked[r];
        LightState& s = m_States[candidates[ci].SortId];
        if (s.Slot >= 0)
            continue; // already an incumbent
        const int32_t slot = lowestFreeSlot();
        if (slot < 0)
            break; // atlas full
        s.Slot = slot;
        s.SlotCooldown = std::max(s.SlotCooldown, cooldownFrames);
        slotOwner[static_cast<uint32_t>(slot)] = static_cast<int32_t>(ci);
    }

    // Contest: a still-unadmitted challenger may evict the lowest-coverage
    // incumbent whose cooldown elapsed, but only if it out-covers it by the
    // hysteresis factor (design §4.4 / A7 — no thrash at the budget boundary).
    for (uint32_t r = 0; r < ranked.size(); ++r)
    {
        const uint32_t ci = ranked[r];
        LightState& challenger = m_States[candidates[ci].SortId];
        if (challenger.Slot >= 0)
            continue;
        int32_t weakestSlot = -1;
        for (uint32_t slot = 0; slot < budget; ++slot)
        {
            const int32_t owner = slotOwner[slot];
            if (owner < 0)
                continue;
            LightState& inc = m_States[candidates[static_cast<uint32_t>(owner)].SortId];
            if (inc.SlotCooldown > 0u)
                continue; // freshly-admitted incumbent, protected from eviction
            if (weakestSlot < 0 ||
                coverageOf(owner) < coverageOf(slotOwner[static_cast<uint32_t>(weakestSlot)]))
                weakestSlot = static_cast<int32_t>(slot);
        }
        if (weakestSlot < 0)
            continue;
        const int32_t incIdx = slotOwner[static_cast<uint32_t>(weakestSlot)];
        if (candidates[ci].CoverageRadiusPx <= coverageOf(incIdx) * kBudgetHysteresisFactor)
            continue; // not enough to justify a swap
        m_States[candidates[static_cast<uint32_t>(incIdx)].SortId].Slot = -1;
        challenger.Slot = weakestSlot;
        challenger.SlotCooldown = std::max(challenger.SlotCooldown, cooldownFrames);
        slotOwner[static_cast<uint32_t>(weakestSlot)] = static_cast<int32_t>(ci);
    }

    // ── Step D: L1a per-(view,slot) render-cache decision.
    // A committed slot re-renders only when its cached content is stale; a clean
    // slot declares no face passes and samples the retained atlas layers. Dirty
    // slots are served in importance (coverage) order under the refresh budget;
    // an over-budget dirty slot defers one frame (stays dirty, samples last frame).
    // Caching OFF => every committed slot renders and the SlotRenderState records
    // are left untouched — because the caster epoch is monotonic, a re-enable only
    // skips a slot whose epoch/content/LOD key is byte-identical to its last cached
    // render, which the always-render path kept current, so the retained layer is
    // valid.
    std::array<bool, kMaxPointShadowSlots> needsRender{};
    std::array<PointShadowDirtyCause, kMaxPointShadowSlots> dirtyCause{};
    needsRender.fill(true);
    dirtyCause.fill(PointShadowDirtyCause::FirstRender);
    if (cache.Enabled)
    {
        needsRender.fill(false);
        dirtyCause.fill(PointShadowDirtyCause::Cached);
        uint32_t rendered = 0;
        for (const uint32_t ci : ranked)
        {
            const Candidate& c = candidates[ci];
            const LightState& s = m_States[c.SortId];
            if (s.Slot < 0)
                continue;
            const uint32_t slot = static_cast<uint32_t>(s.Slot);
            SlotRenderState& srs = m_SlotRenderStates[slot];

            // Exact-equality dirty test (checked in cause-priority order). Any
            // difference re-renders the whole light's visible faces — the
            // provably-hole-free rule: a face is skipped only when the identical
            // content key was last rendered, so no receiver ever samples a layer
            // that was never drawn under the current state.
            PointShadowDirtyCause cause;
            if (!srs.Rendered)
                cause = PointShadowDirtyCause::FirstRender;
            else if (srs.SortId != c.SortId)
                cause = PointShadowDirtyCause::SlotReTenant;
            else if (!(srs.Lod == cache.Lod))
                cause = PointShadowDirtyCause::LodSelectionChanged;
            else if (srs.CasterEpoch != s.RelevantCasterEpoch)
                cause = PointShadowDirtyCause::CasterMoved;
            else if (srs.Tier != s.Tier)
                cause = PointShadowDirtyCause::TierChanged;
            else if (srs.ContentHash != c.ContentHash)
                cause = PointShadowDirtyCause::ContentChanged;
            else
                continue; // clean: needsRender stays false, cache record kept

            if (rendered >= cache.RefreshBudget)
            {
                dirtyCause[slot] = PointShadowDirtyCause::BudgetDeferred;
                continue; // defer: srs untouched => still dirty next frame
            }

            needsRender[slot] = true;
            dirtyCause[slot] = cause;
            srs.SortId = c.SortId;
            srs.ContentHash = c.ContentHash;
            srs.Tier = s.Tier;
            // L1b: the record stores the light's RELEVANT epoch, not the global
            // one — a later far-away change advances the global epoch but not
            // this light's, so the record still matches and the slot stays cached.
            srs.CasterEpoch = s.RelevantCasterEpoch;
            srs.Lod = cache.Lod;
            srs.Rendered = true;
            ++rendered;
        }
    }

    // ── Step E: emit the committed slots.
    PlanResult result{};
    uint32_t maxSlot = 0;
    for (uint32_t slot = 0; slot < budget; ++slot)
    {
        const int32_t owner = slotOwner[slot];
        if (owner < 0)
            continue;
        const Candidate& c = candidates[static_cast<uint32_t>(owner)];
        const LightState& s = m_States[c.SortId];
        SlotAssignment& a = result.Slots[slot];
        a.SortId = c.SortId;
        a.ClusterIndex = c.ClusterIndex;
        a.Tier = s.Tier;
        a.TileResolution = ResolvePointShadowTileResolution(s.Tier, c.InheritResolution);
        a.NeedsRender = needsRender[slot];
        a.DirtyCause = dirtyCause[slot];
        maxSlot = std::max(maxSlot, slot + 1u);
    }
    result.SlotCount = maxSlot;
    return result;
}

} // namespace Engine::Renderer
} // namespace GameEngine
