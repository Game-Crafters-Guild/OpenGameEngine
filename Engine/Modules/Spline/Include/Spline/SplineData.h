#pragma once

#include "Spline/SplineTypes.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector2.h"

#include <cstddef>
#include <vector>

namespace GameEngine::Spline
{

// Heavyweight spline data container. Stores control points, cached arc-length
// LUT, and segment bounding boxes for spatial queries.
//
// This is NOT an ECS component (contains std::vector). It lives in a service
// and is referenced by a lightweight handle in the ECS component (Phase 2).
// Follows the TerrainData pattern from TerrainService.
class SplineData
{
public:
    SplineData() = default;

    // ---- Configuration ----

    SplineType Type = SplineType::CatmullRom;

    // Authored truth: what the user ticked. Serialization, the inspector
    // checkbox and the close-the-loop gesture read this flag; geometry never
    // does — it asks IsEffectivelyClosed() instead.
    bool Closed = false;

    // ---- Control points ----

    std::vector<SplineControlPoint> Points;

    // Fewest control points a spline can hold and still describe a curve:
    // below this there is no segment to evaluate.
    static constexpr size_t kMinPointCount = 2;

    // Fewest control points a ring needs to enclose anything. Two points close
    // into A->B->A, which retraces its own centreline and encloses zero area.
    static constexpr size_t kMinClosedPointCount = 3;

    uint32 GetSegmentCount() const;
    bool IsValid() const { return Points.size() >= kMinPointCount; }

    // Generated truth: every geometry consumer — segment count, wrap sampling,
    // end handling, interior tests — asks this rather than reading Closed, so
    // no consumer can disagree about whether a loop is a loop. A closed spline
    // below kMinClosedPointCount generates as an open segment while Closed
    // stays exactly as authored, so re-adding a point restores the ring.
    [[nodiscard]] bool IsEffectivelyClosed() const
    {
        return Closed && Points.size() >= kMinClosedPointCount;
    }

    // ---- Cached data (rebuilt when dirty) ----

    // Arc-length lookup table: maps uniform distance samples to parametric t.
    // ArcLengthTable[i] = cumulative arc length at sample i.
    // ArcLengthParams[i] = parametric t at sample i.
    std::vector<float32> ArcLengthTable;
    std::vector<float32> ArcLengthParams;
    float32 TotalArcLength = 0.0f;
    uint32 ArcLengthSamples = 256;

    // Per-segment axis-aligned bounding boxes for broad-phase spatial queries.
    std::vector<Mathematics::AABB> SegmentBounds;

    // For closed splines: sampled polygon vertices (XZ plane) for interior
    // point-in-polygon tests. Built by RebuildSplineCache when the spline is
    // effectively closed, and cleared otherwise.
    std::vector<Mathematics::Vector2> ClosedPolygonXZ;

    // ---- Dirty tracking ----

    bool Dirty = true;
    uint64 Version = 0;

    void MarkDirty() { Dirty = true; ++Version; }

    // ---- Point manipulation ----

    // Replace every control point with one point per sampled frame, carrying
    // all per-point channels (Radius, Roll, Rotation, Scale) across the
    // rewrite — the resample primitive: any channel interpolated into
    // SplineFrame survives a point-list rewrite by construction. For
    // CubicBezier, tangent handles are fabricated from centered differences
    // of the new positions (Catmull-Rom-equivalent smoothness); CatmullRom
    // tangents are recomputed by the next RebuildSplineCache.
    void ReplacePointsFromFrames(const std::vector<SplineFrame>& frames);

    void AddPoint(const Mathematics::Vector3& position, float32 radius = 5.0f);
    void InsertPoint(uint32 index, const Mathematics::Vector3& position, float32 radius = 5.0f);
    // Erase one control point, shifting the survivors down. Refused (returns
    // false, nothing mutated) when the index is out of range or the spline is
    // already at kMinPointCount — callers get to report the refusal rather
    // than silently ending up with an unevaluable spline. Erasing is the whole
    // operation for every spline type: Bezier handles belong to the points
    // that carry them, and CatmullRom/Linear tangents are derived from the
    // neighbour set at evaluation time.
    bool RemovePoint(uint32 index);
    void SetPointPosition(uint32 index, const Mathematics::Vector3& position);
    void SetPointRadius(uint32 index, float32 radius);
    void SetPointRoll(uint32 index, float32 rollRadians);
    void SetPointRotation(uint32 index, const Mathematics::Vector3& rotationDegrees);
    void SetPointScale(uint32 index, const Mathematics::Vector3& scale);

    // For CubicBezier: set tangent handle offsets relative to point position.
    void SetPointTangents(uint32 index,
                          const Mathematics::Vector3& tangentIn,
                          const Mathematics::Vector3& tangentOut);
};

} // namespace GameEngine::Spline
