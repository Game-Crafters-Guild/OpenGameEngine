#pragma once

// Pure geometry for terrain-modifier volume gizmos (display-only, read-only).
//
// Produces world-space line-list outlines for a modifier's footprint on planar
// AND spherical (planet) terrains. There are no ECS / render / service
// dependencies here — the gizmo class feeds these outlines to
// GizmoRenderContext. Kept header-only and dependency-light so it is unit-
// testable in isolation, which is also what enforces the #530 lesson: gizmo
// geometry is a pure function of the modifier's parameters and can never reach
// the modifier system's bake / dirty path.
//
// The spherical projection mirrors TerrainModifierSystem's sphere bake exactly
// (MakeSpherePlacement / EvaluateSpherePlacement in TerrainModifierSystem.cpp):
// centre direction N = normalize(worldPos - centre), tangent frame
// E1 = AnyTangent(N), E2 = cross(N, E1), and local tangent-plane metres
// l0 = R*dot(dir, E1), l1 = R*dot(dir, E2). Reusing AnyTangent (the same helper
// the bake uses) means the ring the gizmo draws is the true set of surface
// directions the bake weights — "what it affects", not an approximation.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "CBTTerrain/CBTPlanetShading.h" // AnyTangent — the tangent frame the sphere bake uses
#include "Mathematics/Vector3.h"

namespace GameEngine::Editor::SceneTools::ModifierGizmoGeometry
{

// The footprint outline the modifier stack weights against. Disc = circle
// (Radius); Rect = axis-aligned rectangle (RectHalfX/Z) rotated about +Y by yaw.
// Spline modifiers are drawn from their resolved path by the gizmo, not here.
enum class FootprintShape : std::uint8_t
{
    Disc,
    Rect,
};

// Ordered footprint-local offsets (l0, l1) in metres, in the modifier's
// footprint plane. For a planar terrain these are world XZ offsets from the
// modifier position; for a sphere they are tangent-plane metres about the
// modifier direction. Shared by both projections so the shape convention has a
// single source of truth and matches the bake's ComputeWeight / SphereShapeWeight
// rotation exactly (rect corners are R(yaw) * (±halfX, ±halfZ)).
inline void ComputeFootprintLocalRing(std::vector<std::array<float, 2>>& out,
                                      FootprintShape shape, float radius, float rectHalfX,
                                      float rectHalfZ, float yawRadians, std::uint32_t segments)
{
    out.clear();
    if (shape == FootprintShape::Rect)
    {
        const float c = std::cos(yawRadians);
        const float s = std::sin(yawRadians);
        const float corners[4][2] = {
            {rectHalfX, rectHalfZ},
            {-rectHalfX, rectHalfZ},
            {-rectHalfX, -rectHalfZ},
            {rectHalfX, -rectHalfZ},
        };
        out.reserve(4u);
        for (const auto& corner : corners)
        {
            const float lx = corner[0];
            const float lz = corner[1];
            out.push_back({c * lx - s * lz, s * lx + c * lz});
        }
        return;
    }

    const std::uint32_t count = segments < 3u ? 3u : segments;
    out.reserve(count);
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(count);
        out.push_back({radius * std::cos(a), radius * std::sin(a)});
    }
}

// Append a closed loop of line segments (consecutive from, to endpoint pairs, the
// layout GizmoRenderContext::DrawColoredLines consumes) connecting `points` in
// order, with the last point joined back to the first.
inline void AppendClosedLoopSegments(std::vector<Mathematics::Vector3>& out,
                                     const std::vector<std::array<float, 3>>& points)
{
    const std::size_t n = points.size();
    if (n < 2u)
        return;
    out.reserve(out.size() + n * 2u);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::array<float, 3>& a = points[i];
        const std::array<float, 3>& b = points[(i + 1u) % n];
        out.emplace_back(a[0], a[1], a[2]);
        out.emplace_back(b[0], b[1], b[2]);
    }
}

// ---- planar footprint outline (XZ plane) -----------------------------------

// Ordered world-space ring points of a planar footprint at height `y`. The
// footprint-local offset (l0, l1) maps directly to a world XZ offset from the
// modifier position, matching ComputeModifierBounds' rect rotation.
inline void ComputePlanarRingPoints(std::vector<std::array<float, 3>>& out, FootprintShape shape,
                                    float centerX, float y, float centerZ, float radius,
                                    float rectHalfX, float rectHalfZ, float yawRadians,
                                    std::uint32_t segments)
{
    std::vector<std::array<float, 2>> local;
    ComputeFootprintLocalRing(local, shape, radius, rectHalfX, rectHalfZ, yawRadians, segments);
    out.clear();
    out.reserve(local.size());
    for (const auto& p : local)
        out.push_back({centerX + p[0], y, centerZ + p[1]});
}

// Closed planar footprint outline as a DrawColoredLines-ready line list.
inline void AppendPlanarFootprint(std::vector<Mathematics::Vector3>& out, FootprintShape shape, float centerX,
                                  float y, float centerZ, float radius, float rectHalfX,
                                  float rectHalfZ, float yawRadians, std::uint32_t segments)
{
    std::vector<std::array<float, 3>> pts;
    ComputePlanarRingPoints(pts, shape, centerX, y, centerZ, radius, rectHalfX, rectHalfZ,
                            yawRadians, segments);
    AppendClosedLoopSegments(out, pts);
}

// ---- spherical (planet) projection -----------------------------------------

// Radial tangent frame at a modifier direction, matching MakeSpherePlacement.
struct SphereFrame
{
    std::array<float, 3> N{0.0f, 1.0f, 0.0f};  // centre direction = normalize(worldPos - centre)
    std::array<float, 3> E1{1.0f, 0.0f, 0.0f}; // AnyTangent(N) — local footprint X axis
    std::array<float, 3> E2{0.0f, 0.0f, 1.0f}; // cross(N, E1) — local footprint Z axis
    bool Valid = false;                        // false when the modifier sits on the planet centre
};

inline SphereFrame MakeSphereFrame(const std::array<float, 3>& planetCenter,
                                   const std::array<float, 3>& modifierWorldPos)
{
    SphereFrame f{};
    const float rx = modifierWorldPos[0] - planetCenter[0];
    const float ry = modifierWorldPos[1] - planetCenter[1];
    const float rz = modifierWorldPos[2] - planetCenter[2];
    const float len = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (len <= 0.0f)
        return f; // no radial direction at the centre
    f.N = {rx / len, ry / len, rz / len};
    f.E1 = CBTTerrain::AnyTangent(f.N);
    f.E2 = {f.N[1] * f.E1[2] - f.N[2] * f.E1[1], f.N[2] * f.E1[0] - f.N[0] * f.E1[2],
            f.N[0] * f.E1[1] - f.N[1] * f.E1[0]};
    f.Valid = true;
    return f;
}

// Unit surface direction for a tangent-plane local offset (l0, l1) metres:
// dir = N*sqrt(1 - (l0^2+l1^2)/R^2) + E1*(l0/R) + E2*(l1/R). This is the exact
// inverse of the bake's l0 = R*dot(dir, E1). A footprint wider than the
// hemisphere saturates at the horizon (radicand clamped at 0); the result is
// renormalized so an over-wide footprint stays a unit direction.
inline std::array<float, 3> TangentOffsetToDirection(const SphereFrame& f, float radius, float l0,
                                                     float l1)
{
    const float invR = radius > 0.0f ? 1.0f / radius : 0.0f;
    const float a = l0 * invR;
    const float b = l1 * invR;
    const float rad = 1.0f - a * a - b * b;
    const float n = rad > 0.0f ? std::sqrt(rad) : 0.0f;
    std::array<float, 3> dir = {f.N[0] * n + f.E1[0] * a + f.E2[0] * b,
                                f.N[1] * n + f.E1[1] * a + f.E2[1] * b,
                                f.N[2] * n + f.E1[2] * a + f.E2[2] * b};
    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float dinv = dl > 0.0f ? 1.0f / dl : 0.0f;
    return {dir[0] * dinv, dir[1] * dinv, dir[2] * dinv};
}

// Ordered world-space points of a footprint projected onto the sphere of
// `radius` centred at `planetCenter`. `heightAbove(dx, dy, dz)` lifts each point
// off the base sphere onto the visible relief / sculpt surface; pass a functor
// returning 0 for the base sphere. Each point is
// planetCenter + dir * (radius + heightAbove(dir)).
template <typename HeightFn>
inline void ComputeSphereProjectedRingPoints(std::vector<std::array<float, 3>>& out,
                                             const std::array<float, 3>& planetCenter,
                                             const SphereFrame& frame, float radius,
                                             FootprintShape shape, float footprintRadius,
                                             float rectHalfX, float rectHalfZ, float yawRadians,
                                             std::uint32_t segments, HeightFn&& heightAbove)
{
    out.clear();
    if (!frame.Valid || radius <= 0.0f)
        return;
    std::vector<std::array<float, 2>> local;
    ComputeFootprintLocalRing(local, shape, footprintRadius, rectHalfX, rectHalfZ, yawRadians,
                              segments);
    out.reserve(local.size());
    for (const auto& p : local)
    {
        const std::array<float, 3> dir = TangentOffsetToDirection(frame, radius, p[0], p[1]);
        const float h = static_cast<float>(heightAbove(dir[0], dir[1], dir[2]));
        const float r = radius + h;
        out.push_back({planetCenter[0] + dir[0] * r, planetCenter[1] + dir[1] * r,
                       planetCenter[2] + dir[2] * r});
    }
}

// Closed spherical footprint outline as a DrawColoredLines-ready line list.
template <typename HeightFn>
inline void AppendSphereFootprint(std::vector<Mathematics::Vector3>& out,
                                  const std::array<float, 3>& planetCenter, const SphereFrame& frame,
                                  float radius, FootprintShape shape, float footprintRadius,
                                  float rectHalfX, float rectHalfZ, float yawRadians,
                                  std::uint32_t segments, HeightFn&& heightAbove)
{
    std::vector<std::array<float, 3>> pts;
    ComputeSphereProjectedRingPoints(pts, planetCenter, frame, radius, shape, footprintRadius,
                                     rectHalfX, rectHalfZ, yawRadians, segments,
                                     std::forward<HeightFn>(heightAbove));
    AppendClosedLoopSegments(out, pts);
}

} // namespace GameEngine::Editor::SceneTools::ModifierGizmoGeometry
