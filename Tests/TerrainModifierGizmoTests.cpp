// Unit tests for the terrain modifier gizmo geometry (pure, read-only footprint
// generation). These lock the two properties the gizmo depends on: the planar
// footprint outline is the modifier's XZ footprint, and the spherical projection
// is the EXACT inverse of the sphere bake's local-coordinate mapping
// (l0 = R*dot(dir, E1)) — so the ring the gizmo draws is the true set of surface
// directions the modifier stack weights, not an approximation.

#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "SceneView/TerrainModifierGizmoGeometry.h"

namespace Geo = GameEngine::Editor::SceneTools::ModifierGizmoGeometry;

namespace
{
constexpr float kEps = 1e-4f;

float Dist(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float Len(const std::array<float, 3>& a) { return std::sqrt(Dot(a, a)); }
} // namespace

// ---- planar footprint -------------------------------------------------------

TEST(TerrainModifierGizmoGeometry, PlanarDiscRingPointsLieOnCircle)
{
    std::vector<std::array<float, 3>> pts;
    const float cx = 10.0f, y = 3.0f, cz = -4.0f, radius = 25.0f;
    Geo::ComputePlanarRingPoints(pts, Geo::FootprintShape::Disc, cx, y, cz, radius, 0.0f, 0.0f, 0.0f,
                                 48u);
    ASSERT_EQ(pts.size(), 48u);
    for (const auto& p : pts)
    {
        EXPECT_NEAR(p[1], y, kEps); // planar ring stays at the given height
        const float dx = p[0] - cx, dz = p[2] - cz;
        EXPECT_NEAR(std::sqrt(dx * dx + dz * dz), radius, 1e-3f);
    }
}

TEST(TerrainModifierGizmoGeometry, PlanarRectFourCornersHonorYaw)
{
    // yaw = 0: corners at (±hx, ±hz) offsets from centre.
    std::vector<std::array<float, 3>> pts;
    const float cx = 0.0f, cz = 0.0f, hx = 30.0f, hz = 10.0f;
    Geo::ComputePlanarRingPoints(pts, Geo::FootprintShape::Rect, cx, 0.0f, cz, 0.0f, hx, hz, 0.0f,
                                 48u);
    ASSERT_EQ(pts.size(), 4u);
    for (const auto& p : pts)
    {
        EXPECT_NEAR(std::abs(p[0]), hx, kEps);
        EXPECT_NEAR(std::abs(p[2]), hz, kEps);
    }

    // yaw = 90°: the local X axis maps to world Z, so the world extents swap.
    std::vector<std::array<float, 3>> rot;
    const float halfPi = 1.57079632679489661923f;
    Geo::ComputePlanarRingPoints(rot, Geo::FootprintShape::Rect, cx, 0.0f, cz, 0.0f, hx, hz, halfPi,
                                 48u);
    ASSERT_EQ(rot.size(), 4u);
    for (const auto& p : rot)
    {
        EXPECT_NEAR(std::abs(p[0]), hz, 1e-3f);
        EXPECT_NEAR(std::abs(p[2]), hx, 1e-3f);
    }
}

TEST(TerrainModifierGizmoGeometry, ClosedLoopSegmentsWrapBackToStart)
{
    std::vector<std::array<float, 3>> pts = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}};
    std::vector<GameEngine::Mathematics::Vector3> lines;
    Geo::AppendClosedLoopSegments(lines, pts);
    // 3 points -> 3 segments -> 3 * 2 endpoints.
    ASSERT_EQ(lines.size(), 6u);
    // Last segment's "to" is the first point (closed loop).
    EXPECT_NEAR(lines[5].x, 0.0f, kEps);
    EXPECT_NEAR(lines[5].y, 0.0f, kEps);
    EXPECT_NEAR(lines[5].z, 0.0f, kEps);
}

TEST(TerrainModifierGizmoGeometry, PlanarFootprintEmitsClosedLoopLineCount)
{
    std::vector<GameEngine::Mathematics::Vector3> lines;
    Geo::AppendPlanarFootprint(lines, Geo::FootprintShape::Disc, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 0.0f,
                               0.0f, 32u);
    EXPECT_EQ(lines.size(), 32u * 2u);
}

// ---- spherical projection ---------------------------------------------------

TEST(TerrainModifierGizmoGeometry, SphereFrameIsOrthonormalAndRadial)
{
    const std::array<float, 3> center = {0, 0, 0};
    const std::array<float, 3> pos = {100.0f, 250.0f, -70.0f};
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);
    ASSERT_TRUE(f.Valid);

    EXPECT_NEAR(Len(f.N), 1.0f, kEps);
    EXPECT_NEAR(Len(f.E1), 1.0f, kEps);
    EXPECT_NEAR(Len(f.E2), 1.0f, kEps);
    EXPECT_NEAR(Dot(f.N, f.E1), 0.0f, kEps);
    EXPECT_NEAR(Dot(f.N, f.E2), 0.0f, kEps);
    EXPECT_NEAR(Dot(f.E1, f.E2), 0.0f, kEps);

    // N is the unit radial direction from the centre.
    const float len = Len(pos);
    EXPECT_NEAR(f.N[0], pos[0] / len, kEps);
    EXPECT_NEAR(f.N[1], pos[1] / len, kEps);
    EXPECT_NEAR(f.N[2], pos[2] / len, kEps);
}

TEST(TerrainModifierGizmoGeometry, SphereFrameDegenerateAtCentreIsInvalid)
{
    const std::array<float, 3> center = {5, 6, 7};
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, center);
    EXPECT_FALSE(f.Valid);
}

TEST(TerrainModifierGizmoGeometry, TangentOffsetInvertsBakeLocalCoords)
{
    // The core correctness property: for a direction produced from tangent-plane
    // offset (l0, l1), the bake reads back l0 = R*dot(dir, E1), l1 = R*dot(dir, E2).
    // If this holds, the gizmo ring coincides with the region the bake weights.
    const std::array<float, 3> center = {0, 0, 0};
    const std::array<float, 3> pos = {0.0f, 2000.0f, 0.0f};
    const float R = 2000.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);
    ASSERT_TRUE(f.Valid);

    for (float l0 : {-300.0f, -50.0f, 0.0f, 80.0f, 500.0f})
    {
        for (float l1 : {-200.0f, 0.0f, 120.0f, 400.0f})
        {
            const std::array<float, 3> dir = Geo::TangentOffsetToDirection(f, R, l0, l1);
            EXPECT_NEAR(Len(dir), 1.0f, 1e-4f);
            EXPECT_NEAR(R * Dot(dir, f.E1), l0, 0.2f);
            EXPECT_NEAR(R * Dot(dir, f.E2), l1, 0.2f);
        }
    }
}

TEST(TerrainModifierGizmoGeometry, SphereProjectionLandsOnBaseSphere)
{
    const std::array<float, 3> center = {0, 0, 0};
    const std::array<float, 3> pos = {0.0f, 3000.0f, 0.0f};
    const float R = 3000.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);
    ASSERT_TRUE(f.Valid);

    std::vector<std::array<float, 3>> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, 200.0f,
                                          0.0f, 0.0f, 0.0f, 48u,
                                          [](float, float, float) { return 0.0f; });
    ASSERT_EQ(ring.size(), 48u);
    for (const auto& p : ring)
        EXPECT_NEAR(Dist(center, p), R, 1e-1f); // every point sits on the base sphere
}

TEST(TerrainModifierGizmoGeometry, SphereProjectionHeightLiftsRadius)
{
    const std::array<float, 3> center = {0, 0, 0};
    const std::array<float, 3> pos = {1000.0f, 0.0f, 0.0f};
    const float R = 1000.0f;
    const float lift = 37.5f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);

    std::vector<std::array<float, 3>> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, 90.0f,
                                          0.0f, 0.0f, 0.0f, 24u,
                                          [lift](float, float, float) { return lift; });
    ASSERT_EQ(ring.size(), 24u);
    for (const auto& p : ring)
        EXPECT_NEAR(Dist(center, p), R + lift, 1e-1f);
}

TEST(TerrainModifierGizmoGeometry, SphereDiscAngularRadiusMatchesFootprint)
{
    const std::array<float, 3> center = {0, 0, 0};
    const std::array<float, 3> pos = {0.0f, 5000.0f, 0.0f};
    const float R = 5000.0f;
    const float r = 400.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);

    std::vector<std::array<float, 3>> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, r, 0.0f,
                                          0.0f, 0.0f, 48u,
                                          [](float, float, float) { return 0.0f; });
    ASSERT_EQ(ring.size(), 48u);
    // A disc footprint of world radius r subtends angular radius asin(r/R) from N.
    const float expected = std::asin(r / R);
    for (const auto& p : ring)
    {
        std::array<float, 3> dir = {p[0] / R, p[1] / R, p[2] / R};
        const float ang = std::acos(std::clamp(Dot(dir, f.N), -1.0f, 1.0f));
        EXPECT_NEAR(ang, expected, 1e-3f);
    }
}

TEST(TerrainModifierGizmoGeometry, SphereOverWideFootprintStaysOnSphere)
{
    // Footprint wider than the sphere hemisphere: the radicand clamps and the ring
    // saturates at the horizon, still exactly on the sphere (unit directions).
    const std::array<float, 3> center = {0, 0, 0};
    const std::array<float, 3> pos = {0.0f, 100.0f, 0.0f};
    const float R = 100.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);

    std::vector<std::array<float, 3>> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, 500.0f,
                                          0.0f, 0.0f, 0.0f, 32u,
                                          [](float, float, float) { return 0.0f; });
    ASSERT_EQ(ring.size(), 32u);
    for (const auto& p : ring)
        EXPECT_NEAR(Dist(center, p), R, 1e-2f);
}

TEST(TerrainModifierGizmoGeometry, InvalidSphereFrameEmitsNothing)
{
    const std::array<float, 3> center = {0, 0, 0};
    Geo::SphereFrame invalid{}; // Valid == false by default
    std::vector<std::array<float, 3>> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, invalid, 1000.0f, Geo::FootprintShape::Disc,
                                          50.0f, 0.0f, 0.0f, 0.0f, 48u,
                                          [](float, float, float) { return 0.0f; });
    EXPECT_TRUE(ring.empty());
}
