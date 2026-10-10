#pragma once

#include "Placement/SplineSurfaceConform.h"
#include "Types/Types.h"

namespace GameEngine::SplineECS { class SplineService; }

namespace GameEngine::Editor
{

// Edits rebuild a spline recipe only after this long without further change.
// Coalescing avoids per-frame entity churn (and the shadow-cache invalidation it
// drags in) during a knot drag. A recipe's FIRST build skips this window, except
// when it reads the ground, which DeferFirstBuild holds until that ground is final
// (Placement/SplineSurfaceConform.h).
inline constexpr float32 kSplineRebuildSettleSeconds = 0.25f;

// The inputs every spline recipe controller observes besides its own recipe: the
// spline data it samples and the transform that places it.
struct SplineObservedInputs
{
    uint64 SplineVersion = 0;
    uint32 DataIndex = 0;      // service handle: a scene reload or MCP
    uint32 DataGeneration = 0; // re-create binds fresh data at any version
    float32 WorldMatrix[16] = {};
    bool Valid = false;

    // Bitwise on the matrix, so a NaN in a transform cannot make an observation
    // unequal to itself and re-arm the settle window forever.
    bool operator==(const SplineObservedInputs& other) const;
};

// Reads the spline data a recipe entity binds (version and validity) alongside its
// handle and world matrix. The SplineData pointer is read only within this call on
// the main thread, never held across a CreateSpline, per SplineService's
// pointer-stability caveat.
SplineObservedInputs ObserveSpline(const SplineECS::SplineService& splineService,
                                   uint32 dataIndex,
                                   uint32 dataGeneration,
                                   const float32 (&worldMatrix)[16]);

// The observe, settle and defer state machine that decides when a spline recipe
// controller rebuilds its generated output. ObservedT is the controller's own
// observation (its SplineObservedInputs, recipe and ground digest); a change in any
// part schedules a rebuild.
//
// Per frame and recipe the controller either marks it unbuilt (its inputs are not
// buildable, its output was retired) or asks ShouldRebuild, and after a rebuild
// calls MarkBuilt with what the rebuild was made from.
template <typename ObservedT>
class SplineRebuildGate
{
public:
    // Records this frame's observation and answers whether the recipe rebuilds now.
    //
    // "Applied" counts only while every piece of output is still alive
    // (outputsAlive): a scene reload can revive the same entity id, handle slot and
    // version while the output died with the old world, so dead output always
    // forces a rebuild. Otherwise a built recipe rebuilds once an edit has been
    // stable for the settle window.
    //
    // The first build of a ground-reading recipe waits for ground that is both
    // there and final. The scene loader instantiates every entity up front and
    // provisions terrain data from the asset-resolve pump seconds later, so a
    // measurement taken in that window conforms to whatever meshes happened to
    // resolve. Later builds are covered by the surface revision in ObservedT and
    // re-place through the settle window; the first build skips that window, so it
    // is the one DeferFirstBuild holds, for a bounded time.
    [[nodiscard]] bool ShouldRebuild(const ObservedT& current,
                                     bool outputsAlive,
                                     ConformSurfaceReadiness surfaceReadiness,
                                     bool readsSurface,
                                     float32 deltaSeconds,
                                     uint32 entityId)
    {
        if (!(m_LastSeen == current))
        {
            m_LastSeen = current;
            m_StableSeconds = 0.0f;
        }
        else
        {
            m_StableSeconds += deltaSeconds;
        }

        if (m_AppliedOnce && outputsAlive && m_Applied == m_LastSeen)
            return false;

        if (DeferFirstBuild(surfaceReadiness, readsSurface, m_AppliedOnce,
                            m_StableSeconds >= kSplineRebuildSettleSeconds, deltaSeconds, entityId,
                            m_FirstBuildDeferredSeconds, m_FirstBuildDeferWarned))
            return false;

        // Still settling: commit once the edit pauses.
        if (m_AppliedOnce && outputsAlive && m_StableSeconds < kSplineRebuildSettleSeconds)
            return false;

        return true;
    }

    // Records that the output now reflects `built`.
    void MarkBuilt(const ObservedT& built)
    {
        m_LastSeen = built;
        m_Applied = built;
        m_AppliedOnce = true;
    }

    // Back to never-built, so the first-build wait starts over too: a spent budget
    // carried across would cut the next wait short.
    void MarkUnbuilt()
    {
        m_AppliedOnce = false;
        m_FirstBuildDeferredSeconds = 0.0f;
        m_FirstBuildDeferWarned = false;
    }

    [[nodiscard]] bool AppliedOnce() const { return m_AppliedOnce; }
    [[nodiscard]] const ObservedT& Applied() const { return m_Applied; }

private:
    ObservedT m_LastSeen{};
    ObservedT m_Applied{};
    float32 m_StableSeconds = 0.0f;
    bool m_AppliedOnce = false;
    // How long this recipe has deferred its FIRST build waiting for ground that is
    // both provisioned and final, and whether that budget was reported. Dropped
    // with the controller's state on a world reset, so a reload starts the wait
    // fresh (Placement/SplineSurfaceConform.h).
    float32 m_FirstBuildDeferredSeconds = 0.0f;
    bool m_FirstBuildDeferWarned = false;
};

} // namespace GameEngine::Editor
