#pragma once

// FarFieldReseedDetector — the CPU decision logic for the far-field drain cure (round-9 far-field-
// drain, design candidate 3). It is deliberately PURE: per-frame telemetry (the merge-stall signal,
// pool occupancy, per-root horizon visibility + liveness, and the active-edit footprint) goes in;
// the root set to bulk-free (CBTInstance::RegionFreeToBase) comes out. Producing the per-root
// visibility masks is the caller's job — a GPU per-root accumulator in production, a readback +
// the CPU horizon mirror (CBTPlanetShading.h SphereCornerOccluded) in the probes.
//
// It layers three protections the design requires (design §3 / slice 3):
//   * N-frame HYSTERESIS — a root must be horizon-invisible for InvisibleFramesThreshold consecutive
//     frames before it is eligible, so a transient look-away never triggers a reseed that then pops
//     back in on the look-back.
//   * INTERIOR-only selection (the retained ring) — a root is freed only when NONE of its base
//     neighbors is horizon-visible, so every freed root borders only other invisible roots and the
//     base/refined seam the free creates stays out of the visible boundary (the conformity gate).
//   * EDIT non-interference — a root on a cube face under an active edit is never freed (freeing a
//     force-refined region would fight the edit), plus RATE-LIMITING to at most one reseed per
//     ReseedCooldownFrames so the reclamation transient is bounded.

#include <array>
#include <cstdint>

#include "CBTTerrain/CBTSphereRoots.h" // BuildSphereRoots (base root adjacency)

namespace GameEngine::CBTTerrain
{

struct FarFieldReseedConfig
{
    // N: a root must be horizon-invisible this many consecutive frames before it is eligible to free
    // (the transient-look-away guard). At ~60 fps, 30 frames ≈ half a second of sustained invisibility.
    uint32_t InvisibleFramesThreshold = 30u;
    // K: minimum frames between reseeds — bounds the transient (at most one root-set free per K frames).
    uint32_t ReseedCooldownFrames = 8u;
    // Only arm while the pool is genuinely contended: below this occupancy the far field is not
    // stranded and the normal merge keeps up, so a reseed would be needless churn.
    float StallOccupancyFloor = 0.85f;
};

class FarFieldReseedDetector
{
  public:
    explicit FarFieldReseedDetector(uint32_t rootCount = kSphereRootCount,
                                    FarFieldReseedConfig config = {})
        : m_RootCount(rootCount > 32u ? 32u : rootCount), m_Config(config)
    {
        // Bake the base root adjacency (n0/n1 legs + cross-face twin) so ComputeReseedSet can test
        // the retained-ring condition without recomputing it each frame.
        const auto roots = BuildSphereRoots();
        for (uint32_t r = 0; r < m_RootCount && r < kSphereRootCount; ++r)
        {
            m_Neighbors[r][0] = roots[r].Neighbors.Neighbor0;
            m_Neighbors[r][1] = roots[r].Neighbors.Neighbor1;
            m_Neighbors[r][2] = roots[r].Neighbors.Twin;
        }
    }

    // Per-frame telemetry. rootHorizonVisibleMask/rootLiveMask are bit-per-root (bit r). editFaceMask
    // is bit-per-cube-face (bit f, 0..5) — every root on an edited face is guarded off. frameIndex is
    // a monotonic frame counter (drives hysteresis + the reseed cooldown).
    void Update(bool mergeStalled, float occupancy, uint32_t rootHorizonVisibleMask,
                uint32_t rootLiveMask, uint32_t editFaceMask, uint64_t frameIndex)
    {
        m_FrameIndex = frameIndex;
        m_RootHorizonVisibleMask = rootHorizonVisibleMask;
        m_RootLiveMask = rootLiveMask;
        m_EditFaceMask = editFaceMask;
        m_Armed = mergeStalled && occupancy >= m_Config.StallOccupancyFloor;

        for (uint32_t r = 0; r < m_RootCount; ++r)
        {
            const bool live = ((rootLiveMask >> r) & 1u) != 0u;
            const bool visible = ((rootHorizonVisibleMask >> r) & 1u) != 0u;
            // A live, horizon-occluded root accrues invisible frames; a visible one (or a root with
            // no live bisectors — nothing to drain) resets to 0.
            if (live && !visible)
            {
                if (m_InvisibleFrames[r] < 0xFFFFFFFFu)
                    ++m_InvisibleFrames[r];
            }
            else
            {
                m_InvisibleFrames[r] = 0u;
            }
        }
    }

    // Roots that have been horizon-invisible for >= N frames AND are live AND are not on an edited
    // face — the eligible set BEFORE the interior/ring restriction. Diagnostics + the detector's own
    // interior test read it.
    uint32_t InvisibleEligibleMask() const
    {
        uint32_t mask = 0u;
        for (uint32_t r = 0; r < m_RootCount; ++r)
        {
            if (m_InvisibleFrames[r] < m_Config.InvisibleFramesThreshold)
                continue;
            if (((m_RootLiveMask >> r) & 1u) == 0u)
                continue;
            const uint32_t face = r / kSlicesPerFace;
            if (((m_EditFaceMask >> face) & 1u) != 0u)
                continue; // edit non-interference: never free a root under an active edit
            mask |= (1u << r);
        }
        return mask;
    }

    // The root set to free THIS frame: the INTERIOR of the eligible invisible region (every base
    // neighbor is horizon-invisible, so the retained ring keeps the seam off-screen), gated by the
    // armed state and the reseed cooldown. 0 == nothing to reseed. On a non-zero result the caller
    // drives CBTInstance::RegionFreeToBase and then calls NotifyReseeded.
    uint32_t ComputeReseedSet() const
    {
        if (!m_Armed)
            return 0u;
        if (m_EverReseeded && (m_FrameIndex - m_LastReseedFrame) < m_Config.ReseedCooldownFrames)
            return 0u; // rate-limit: bound the transient to one reseed per K frames

        const uint32_t eligible = InvisibleEligibleMask();
        uint32_t freeSet = 0u;
        for (uint32_t r = 0; r < m_RootCount; ++r)
        {
            if (((eligible >> r) & 1u) == 0u)
                continue;
            // Interior test: free r only when NONE of its base neighbors is horizon-visible, so the
            // free never puts a base/refined seam on the visible boundary (the retained ring is every
            // invisible root that DOES touch a visible one — left to the normal merge).
            bool interior = true;
            for (uint32_t n = 0; n < 3u; ++n)
            {
                const uint32_t nb = m_Neighbors[r][n];
                if (nb == kInvalidPointer || nb >= m_RootCount)
                    continue; // planar boundary leg — no neighbor to gate on
                if (((m_RootHorizonVisibleMask >> nb) & 1u) != 0u)
                {
                    interior = false;
                    break;
                }
            }
            if (interior)
                freeSet |= (1u << r);
        }
        return freeSet;
    }

    // Record that a reseed fired: reset the freed roots' invisible counters (they are base now, so a
    // returning camera re-accrues from zero) and arm the cooldown.
    void NotifyReseeded(uint32_t freedMask, uint64_t frameIndex)
    {
        for (uint32_t r = 0; r < m_RootCount; ++r)
            if (((freedMask >> r) & 1u) != 0u)
                m_InvisibleFrames[r] = 0u;
        m_LastReseedFrame = frameIndex;
        m_EverReseeded = true;
    }

    bool Armed() const { return m_Armed; }
    uint32_t InvisibleFrames(uint32_t root) const
    {
        return root < m_RootCount ? m_InvisibleFrames[root] : 0u;
    }

  private:
    uint32_t m_RootCount;
    FarFieldReseedConfig m_Config;
    std::array<std::array<uint32_t, 3>, 32> m_Neighbors{}; // n0, n1, twin per root
    std::array<uint32_t, 32> m_InvisibleFrames{};
    uint32_t m_RootHorizonVisibleMask = 0xFFFFFFFFu;
    uint32_t m_RootLiveMask = 0u;
    uint32_t m_EditFaceMask = 0u;
    bool m_Armed = false;
    uint64_t m_FrameIndex = 0u;
    uint64_t m_LastReseedFrame = 0u;
    bool m_EverReseeded = false;
};

} // namespace GameEngine::CBTTerrain
