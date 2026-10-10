#pragma once

// Pool-health interpretation of one frame of CBTTessellationStats: is this terrain healthy, and if
// not, what does the user do about it. Lives beside the stats rather than inside the editor's IPC
// handler so the classification has exactly one definition and can be unit-tested against recorded
// stats without a live editor.

#include "CBTTerrain/CBTInstance.h"

#include <cstdint>

namespace GameEngine::CBTTerrain
{

enum class CBTPoolState : uint32_t
{
    Healthy,
    AtRoots,
    Inert,
    SplitsStarvedWithHeadroom,
    NearCapacity,
    NearCapacitySplitsBlocked,
    SaturatedSplitsBlocked,
};

struct CBTPoolHealth
{
    CBTPoolState State = CBTPoolState::Healthy;
    double Occupancy = 0.0;
    bool Saturated = false;
    bool NearCapacity = false;
    bool MergeStalled = false;
    // Splits demanded, none served, and the pool nowhere near full. Merging cannot help — the slots
    // are already free — so this is never a merge stall: something is REFUSING the splits. It is the
    // fingerprint of a decision defect rather than a capacity one, and it is what a converged tree
    // looked like while Kernel_Split yielded to neighbours that never split.
    bool SplitsStarved = false;
    // The tree stands at its seeded roots and no terrain reached the renderer: nothing is drawn, so
    // no occupancy-based reading describes real geometry. A tree at its roots that the renderer was
    // handed is not inert: it draws its roots, which is the whole mesh for ground the content-aware
    // split keeps coarse and for a terrain outside the frustum (CBTPoolState::AtRoots).
    bool Inert = false;
};

// The pool is effectively full: splits are rolled back for want of slots.
inline constexpr double kCBTSaturatedOccupancy = 0.98;
// Where the near-field gate holds occupancy under load.
inline constexpr double kCBTNearCapacityOccupancy = 0.85;

// An unserved merge is NOT a fault on its own, because a merge collapses a whole LEB diamond and
// Kernel_Classify only ever decides for one facet at a time. Two conditions still leave a converged
// tree standing at nonzero merge demand with ZERO occupancy pressure:
//
//   - PrepareSimplify needs the pair ACROSS the split edge at matching depth, not just the sibling.
//     A depth-graded tree does not always have one, and the parent-level merge metric (planar) makes
//     siblings agree without supplying that neighbour.
//   - The spherical domain still decides per facet, on that facet's own projected AREA. Diamond
//     members measure different parts of the parent, so under perspective one can ask while the
//     other does not.
//
// Reading unserved merge demand alone as a deadlock therefore reports healthy terrain as broken.
// The stall flag requires the condition that makes an unserved merge actually cost something: the
// pool is near capacity AND splits are going unserved for want of the slots a merge would free.
//
// `reachedRenderer` is whether any terrain was published to the renderer this frame: the stats
// alone cannot tell a skipped terrain from one whose roots are its whole mesh.
inline CBTPoolHealth DiagnoseCBTPool(const CBTTessellationStats& st, bool reachedRenderer)
{
    CBTPoolHealth h;
    h.Occupancy = st.PoolSize ? static_cast<double>(st.LiveCount) / st.PoolSize : 0.0;
    h.Saturated = h.Occupancy >= kCBTSaturatedOccupancy;
    h.NearCapacity = h.Occupancy >= kCBTNearCapacityOccupancy;

    const bool mergeUnserved = st.MergeDemand > 0 && st.MergeServed == 0;
    const bool splitsUnserved = st.SplitDemand > 0 && st.SplitServed == 0;
    h.MergeStalled = mergeUnserved && splitsUnserved && h.NearCapacity;
    h.SplitsStarved = splitsUnserved && !h.NearCapacity;
    // A tree at (or below) its root count has never refined. RootCount == 0 means the caller did
    // not populate it, and the reading is then unknowable — never guessed from LiveCount alone,
    // because a legitimately tiny LiveCount and an unpopulated field are the same number.
    const bool atRoots = st.RootCount > 0 && st.LiveCount <= st.RootCount;
    h.Inert = atRoots && !reachedRenderer;

    if (h.Saturated && h.MergeStalled)
        h.State = CBTPoolState::SaturatedSplitsBlocked;
    else if (h.MergeStalled)
        h.State = CBTPoolState::NearCapacitySplitsBlocked;
    else if (h.NearCapacity)
        h.State = CBTPoolState::NearCapacity;
    else if (h.SplitsStarved)
        h.State = CBTPoolState::SplitsStarvedWithHeadroom;
    else if (h.Inert)
        // Ranked BELOW starvation on purpose: an inert tree that is demanding splits and getting
        // none is described better by the refusal than by its emptiness.
        h.State = CBTPoolState::Inert;
    else if (atRoots)
        h.State = CBTPoolState::AtRoots;
    return h;
}

inline const char* CBTPoolStateDescription(CBTPoolState state)
{
    switch (state)
    {
    case CBTPoolState::SaturatedSplitsBlocked:
        return "SATURATED, SPLITS BLOCKED: the bisector pool is full, splits are being rolled back "
               "for want of slots, and the merge is freeing none — the topology is frozen. A "
               "static-camera edit or TargetPixelError change cannot re-tessellate until the "
               "camera moves. On a planar terrain the pool-pressure scale should have engaged "
               "(pressureScale > 1; check GE_CBT_POOL_PRESSURE); on a planet raise "
               "TargetPixelError, or enable the near-field gate.";
    case CBTPoolState::NearCapacitySplitsBlocked:
        return "NEAR CAPACITY, SPLITS BLOCKED: the near-field gate is holding the pool below full, "
               "but splits are going unserved and the merge is freeing nothing, so detail is "
               "capped at this altitude / relief / TargetPixelError. Move the camera to sweep the "
               "coarsening fringe, or raise TargetPixelError.";
    case CBTPoolState::NearCapacity:
        return "NEAR CAPACITY: the pool is filling but splits are still being served, so detail is "
               "approaching the pool limit for this altitude / relief / TargetPixelError. A large "
               "merge demand with zero merges served is expected here and is not a stall: it is the "
               "LEB diamond residue of a converged tree (a facet can ask to merge while its partner "
               "across the split edge does not), and the coarser the affordable target, the more of "
               "it there is.";
    case CBTPoolState::SplitsStarvedWithHeadroom:
        return "SPLITS STARVED WITH HEADROOM: splits are being demanded and none served while the "
               "pool still has most of its slots free. Capacity is not the limit and coarsening "
               "will not help — a classification or compatibility decision is refusing the splits. "
               "This is a defect to chain, not a setting to tune.";
    case CBTPoolState::Inert:
        return "INERT: the tree stands at its seeded root count and no terrain reached the "
               "renderer, so this terrain is drawing no geometry and every occupancy figure below "
               "describes an empty triangulation. Check why extraction published no terrain "
               "before reading anything else here.";
    case CBTPoolState::AtRoots:
        return "AT ROOTS: the tree is its seeded roots and draws them as the whole mesh. Expected "
               "for ground flat at the screen's resolution (the content-aware split keeps it "
               "coarse; the first height edit refines it) and for a terrain entirely outside the "
               "frustum; not a fault.";
    case CBTPoolState::Healthy:
        break;
    }
    return "HEALTHY: the pool has headroom; the triangulation refines to TargetPixelError and "
           "edits re-tessellate with a static camera.";
}

} // namespace GameEngine::CBTTerrain
