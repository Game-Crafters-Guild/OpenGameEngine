#pragma once

#include "Spline/SplineData.h"
#include "Spline/SplineTypes.h"
#include "GenerationalVector/GenerationalVector.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>

namespace GameEngine::SplineECS
{

using SplineHandle = GenerationalVector::Handle;

// Singleton service owning all spline data. Follows the TerrainService pattern
// but uses GenerationalVector for slot management. The shared_mutex orders the
// service's own calls against each other; it does not protect spline data read
// through a pointer GetSplineData returned (see there).
class SplineService
{
public:
    static void Initialize();
    static void Shutdown();
    static SplineService& Get();
    static SplineService* TryGet();
    static bool IsInitialized();

    // Create a new empty spline and return its handle.
    SplineHandle CreateSpline(Spline::SplineType type = Spline::SplineType::CatmullRom,
                               bool closed = false);

    // Destroy a spline by handle.
    void DestroySpline(SplineHandle handle);

    // Access spline data. Returns nullptr if handle is invalid/stale.
    // The lookup takes a shared lock, but the returned pointer outlives it. Any
    // CreateSpline can grow the slot storage, which relocates every spline and
    // invalidates every pointer; DestroySpline or RebuildCache on the SAME
    // handle destroys or rewrites the data it points to. Changes to other
    // handles leave it valid. A system reading it on a job worker must declare
    // a schedule dependency on SplineExtraction, which creates and rebuilds
    // splines each frame.
    Spline::SplineData* GetSplineData(SplineHandle handle);
    const Spline::SplineData* GetSplineData(SplineHandle handle) const;

    // Check if a handle is still valid.
    bool IsValid(SplineHandle handle) const;

    // Rebuild arc-length LUT, segment AABBs, and auto-tangents for a spline.
    // Takes an exclusive lock. SplineExtraction calls it on a job worker for
    // splines marked Dirty; editor code calls it after changing control points.
    void RebuildCache(SplineHandle handle);

    uint32 GetActiveSplineCount() const;

    // Monotonic counter of spline geometry changes: bumped by CreateSpline,
    // DestroySpline, and every RebuildCache — i.e. only when a cache was really
    // rebuilt, never on a mere visit. Control-point edits mutate SplineData and
    // stamp no ECS column, so consumers that gate their work on ECS change
    // filters (TerrainModifierSystem) poll this epoch to notice them. Read it
    // AFTER SplineExtractionSystem has run for the frame — the schedule declares
    // that edge — so a same-frame edit is not seen a frame late.
    uint64 GetEditEpoch() const { return m_EditEpoch.load(std::memory_order_relaxed); }

private:
    GenerationalVector::GenerationalVector<Spline::SplineData> m_Splines;
    mutable std::shared_mutex m_Mutex;
    std::atomic<uint64> m_EditEpoch{0};

    static std::unique_ptr<SplineService> s_Instance;
};

} // namespace GameEngine::SplineECS
