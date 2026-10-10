#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewGizmos.h"
#include "Types/Color.h"

using GameEngine::Color;
using GameEngine::Editor::SceneTools::BuildBillboardBasis;
using GameEngine::Editor::SceneTools::BuildPlaneBasis;
using GameEngine::Editor::SceneTools::BuildThickLineQuad;
using GameEngine::Editor::SceneTools::ComputeGizmoIconWorldRadius;
using GameEngine::Editor::SceneTools::DrawCircleInto;
using GameEngine::Editor::SceneTools::DrawWireEllipsoid;
using GameEngine::Editor::SceneTools::GetGizmoLineGroups;
using GameEngine::Editor::SceneTools::GetGizmoTriangleGroups;
using GameEngine::Editor::SceneTools::GizmoLineBatch;
using GameEngine::Editor::SceneTools::GizmoIconPickSphere;
using GameEngine::Editor::SceneTools::GizmoRay;
using GameEngine::Editor::SceneTools::GizmoRenderContext;
using GameEngine::Editor::SceneTools::PickNearestGizmoIcon;
using GameEngine::Editor::SceneTools::ResetGizmoLineGroups;
using GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups;
using GameEngine::Mathematics::Vector3;
using GameEngine::Rendering::CameraId;
using GameEngine::Rendering::ViewId;

namespace
{
constexpr float kEpsilon = 1e-4f;

// A ray from the origin down +Z.
GizmoRay RayAlongZ()
{
    GizmoRay ray;
    ray.origin = Vector3(0.0f, 0.0f, 0.0f);
    ray.direction = Vector3(0.0f, 0.0f, 1.0f);
    return ray;
}

void ExpectOrthonormalFacing(const Vector3& pos, const Vector3& camPos)
{
    Vector3 right;
    Vector3 up;
    BuildBillboardBasis(pos, camPos, right, up);

    const Vector3 toCam = (camPos - pos).Normalize();

    EXPECT_NEAR(right.Length(), 1.0f, kEpsilon);
    EXPECT_NEAR(up.Length(), 1.0f, kEpsilon);
    EXPECT_NEAR(Vector3::Dot(right, up), 0.0f, kEpsilon);
    EXPECT_NEAR(Vector3::Dot(right, toCam), 0.0f, kEpsilon);
    EXPECT_NEAR(Vector3::Dot(up, toCam), 0.0f, kEpsilon);
    // right x up points at the camera, so the icon is never drawn mirrored.
    EXPECT_NEAR(Vector3::Dot(Vector3::Cross(right, up), toCam), 1.0f, kEpsilon);
}

// The bit of the box corner a vertex sits on. Corners are numbered like
// Mathematics::AABB::Corners: bit 0 of the index selects +X, bit 1 +Y and bit 2 +Z.
uint32_t BoxCornerBit(const Vector3& vertex, const Vector3& center)
{
    uint32_t cornerIndex = 0u;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (vertex[axis] > center[axis])
        {
            cornerIndex |= 1u << axis;
        }
    }
    return 1u << cornerIndex;
}
} // namespace

TEST(SceneViewGizmoHelpersTests, BillboardBasisFacesTheCamera)
{
    ExpectOrthonormalFacing(Vector3(1.0f, 2.0f, 3.0f), Vector3(-4.0f, 5.0f, -6.0f));
}

TEST(SceneViewGizmoHelpersTests, BillboardBasisStaysOrthonormalLookingStraightDown)
{
    ExpectOrthonormalFacing(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 10.0f, 0.0f));
}

TEST(SceneViewGizmoHelpersTests, IconRadiusIsAFixedFractionOfTheOrthoHeight)
{
    const Vector3 pos(0.0f, 0.0f, 0.0f);
    const Vector3 farCam(0.0f, 0.0f, -1000.0f);
    const Vector3 nearCam(0.0f, 0.0f, -1.0f);
    // Ortho ignores camera distance: the icon keeps its on-screen size at every zoom.
    EXPECT_FLOAT_EQ(ComputeGizmoIconWorldRadius(pos, farCam, 10.0f),
                    ComputeGizmoIconWorldRadius(pos, nearCam, 10.0f));
    EXPECT_FLOAT_EQ(ComputeGizmoIconWorldRadius(pos, nearCam, 20.0f),
                    2.0f * ComputeGizmoIconWorldRadius(pos, nearCam, 10.0f));
}

TEST(SceneViewGizmoHelpersTests, IconRadiusScalesWithDistanceWithinClamps)
{
    const Vector3 pos(0.0f, 0.0f, 0.0f);
    const float r10 = ComputeGizmoIconWorldRadius(pos, Vector3(0.0f, 0.0f, -10.0f), 0.0f);
    const float r20 = ComputeGizmoIconWorldRadius(pos, Vector3(0.0f, 0.0f, -20.0f), 0.0f);
    EXPECT_NEAR(r20, 2.0f * r10, kEpsilon);

    // Clamped at both ends: an icon never collapses to a dot or fills the view.
    const float minRadius = ComputeGizmoIconWorldRadius(pos, Vector3(0.0f, 0.0f, 0.0f), 0.0f);
    const float maxRadius = ComputeGizmoIconWorldRadius(pos, Vector3(0.0f, 0.0f, -1.0e6f), 0.0f);
    EXPECT_GT(minRadius, 0.0f);
    EXPECT_LT(minRadius, r10);
    EXPECT_FLOAT_EQ(minRadius, ComputeGizmoIconWorldRadius(pos, Vector3(0.0f, 0.0f, -0.001f), 0.0f));
    EXPECT_GT(maxRadius, r20);
    EXPECT_FLOAT_EQ(maxRadius, ComputeGizmoIconWorldRadius(pos, Vector3(0.0f, 0.0f, -2.0e6f), 0.0f));
}

TEST(SceneViewGizmoHelpersTests, CircleEmitsOneLinePerSegmentOnTheEllipse)
{
    const ViewId viewId = 7;
    ResetGizmoLineGroups(viewId);
    GizmoRenderContext context(viewId, CameraId{1});

    constexpr int kSegments = 12;
    const Vector3 center(1.0f, 2.0f, 3.0f);
    {
        GizmoLineBatch batch(context, Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f);
        DrawCircleInto(batch, center, Vector3(1.0f, 0.0f, 0.0f), 2.0f, Vector3(0.0f, 0.0f, 1.0f), 0.5f,
                       kSegments);
    }

    const auto* groups = GetGizmoLineGroups(viewId);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const std::vector<float>& v = groups->front().vertices;
    ASSERT_EQ(v.size(), static_cast<size_t>(kSegments) * 6u);

    for (size_t i = 0; i < v.size(); i += 3)
    {
        // Every endpoint lies on the ellipse x^2/2^2 + z^2/0.5^2 = 1 in the plane y = 2.
        const float x = (v[i + 0] - center.x) / 2.0f;
        const float z = (v[i + 2] - center.z) / 0.5f;
        EXPECT_NEAR(v[i + 1], center.y, kEpsilon);
        EXPECT_NEAR(x * x + z * z, 1.0f, 1e-3f);
    }
    // The outline closes: the last line ends where the first begins.
    EXPECT_NEAR(v[v.size() - 3], v[0], kEpsilon);
    EXPECT_NEAR(v[v.size() - 2], v[1], kEpsilon);
    EXPECT_NEAR(v[v.size() - 1], v[2], kEpsilon);

    ResetGizmoLineGroups(viewId);
}

TEST(SceneViewGizmoHelpersTests, WireEllipsoidDrawsThreeRingsOnItsSurface)
{
    const ViewId viewId = 8;
    ResetGizmoLineGroups(viewId);
    GizmoRenderContext context(viewId, CameraId{1});

    constexpr int kSegments = 12;
    const Vector3 center(1.0f, 2.0f, 3.0f);
    const Vector3 radii(2.0f, 0.5f, 1.5f);
    DrawWireEllipsoid(context, center, Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f),
                      Vector3(0.0f, 0.0f, 1.0f), radii, Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, kSegments);

    const auto* groups = GetGizmoLineGroups(viewId);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const std::vector<float>& v = groups->front().vertices;
    ASSERT_EQ(v.size(), 3u * static_cast<size_t>(kSegments) * 6u);

    // The rings lie in the (X, Y), (Y, Z) and (Z, X) planes, in that order, so each
    // keeps the centre's coordinate on its missing axis: Z, then X, then Y.
    constexpr size_t kMissingAxis[3] = {2u, 0u, 1u};
    const size_t floatsPerRing = static_cast<size_t>(kSegments) * 6u;
    for (size_t i = 0; i < v.size(); i += 3)
    {
        const float x = (v[i + 0] - center.x) / radii.x;
        const float y = (v[i + 1] - center.y) / radii.y;
        const float z = (v[i + 2] - center.z) / radii.z;
        EXPECT_NEAR(x * x + y * y + z * z, 1.0f, 1e-3f) << "endpoint " << i / 3;

        const size_t missingAxis = kMissingAxis[i / floatsPerRing];
        EXPECT_NEAR(v[i + missingAxis], center[missingAxis], kEpsilon)
            << "ring " << i / floatsPerRing << " endpoint " << i / 3;
    }

    ResetGizmoLineGroups(viewId);
}

// DrawWireSphere is the wire ellipsoid on the world axes with equal radii: the same
// rings, in the same planes, at the same segment count, in one batch.
TEST(SceneViewGizmoHelpersTests, WireSphereIsTheWireEllipsoidOnTheWorldAxes)
{
    const ViewId sphereViewId = 11;
    const ViewId ellipsoidViewId = 12;
    ResetGizmoLineGroups(sphereViewId);
    ResetGizmoLineGroups(ellipsoidViewId);
    GizmoRenderContext sphereContext(sphereViewId, CameraId{1});
    GizmoRenderContext ellipsoidContext(ellipsoidViewId, CameraId{1});

    const Vector3 center(1.0f, 2.0f, 3.0f);
    const float radius = 1.5f;
    const Color color(1.0f, 1.0f, 1.0f, 1.0f);
    sphereContext.DrawWireSphere(center, radius, color);
    DrawWireEllipsoid(ellipsoidContext, center, Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f),
                      Vector3(0.0f, 0.0f, 1.0f), Vector3(radius, radius, radius), color, 1.0f);

    const auto* sphereGroups = GetGizmoLineGroups(sphereViewId);
    const auto* ellipsoidGroups = GetGizmoLineGroups(ellipsoidViewId);
    ASSERT_NE(sphereGroups, nullptr);
    ASSERT_NE(ellipsoidGroups, nullptr);
    ASSERT_EQ(sphereGroups->size(), 1u);
    ASSERT_EQ(ellipsoidGroups->size(), 1u);
    EXPECT_EQ(sphereGroups->front().vertices, ellipsoidGroups->front().vertices);

    ResetGizmoLineGroups(sphereViewId);
    ResetGizmoLineGroups(ellipsoidViewId);
}

// The solid box is 12 triangles, each on one of the 6 box planes and wound so
// (b - a) x (c - a) points out of the box. The areas on each plane add up to the
// face area, and each plane holds exactly two triangles that share exactly two
// corners, and those two corners differ on two axes. The pair therefore meets on
// the face diagonal and covers the whole face.
TEST(SceneViewGizmoHelpersTests, SolidBoxPutsTwoOutwardTrianglesOnEachFace)
{
    const ViewId viewId = 9;
    ResetGizmoTriangleGroups(viewId);
    GizmoRenderContext context(viewId, CameraId{1});

    const Vector3 center(1.0f, 2.0f, 3.0f);
    const Vector3 halfExtents(0.5f, 1.0f, 2.0f);
    context.DrawSolidBox(center, halfExtents, Color(1.0f, 1.0f, 1.0f, 1.0f));

    const auto* groups = GetGizmoTriangleGroups(viewId);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const std::vector<float>& v = groups->front().vertices;
    ASSERT_EQ(v.size(), 12u * 9u);

    // faceArea[axis][side]: summed triangle area on the plane at center -/+ halfExtents.
    float faceArea[3][2] = {};
    // faceTriangleCorners[axis][side]: one mask of BoxCornerBit values per triangle on that plane.
    std::vector<uint32_t> faceTriangleCorners[3][2];
    for (size_t t = 0; t < v.size(); t += 9)
    {
        const Vector3 a(v[t + 0], v[t + 1], v[t + 2]);
        const Vector3 b(v[t + 3], v[t + 4], v[t + 5]);
        const Vector3 c(v[t + 6], v[t + 7], v[t + 8]);
        const Vector3 normal = Vector3::Cross(b - a, c - a);

        int planeAxis = -1;
        int planeSide = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            for (int side = 0; side < 2; ++side)
            {
                const float plane = center[axis] + (side == 0 ? -halfExtents[axis] : halfExtents[axis]);
                if (std::fabs(a[axis] - plane) < kEpsilon && std::fabs(b[axis] - plane) < kEpsilon &&
                    std::fabs(c[axis] - plane) < kEpsilon)
                {
                    planeAxis = axis;
                    planeSide = side;
                }
            }
        }
        ASSERT_GE(planeAxis, 0) << "triangle " << t / 9 << " is not on a box face";
        const float outward = planeSide == 0 ? -1.0f : 1.0f;
        EXPECT_GT(normal[planeAxis] * outward, 0.0f) << "triangle " << t / 9 << " faces into the box";
        faceArea[planeAxis][planeSide] += 0.5f * normal.Length();
        faceTriangleCorners[planeAxis][planeSide].push_back(BoxCornerBit(a, center) | BoxCornerBit(b, center) |
                                                            BoxCornerBit(c, center));
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        const float expectedArea =
            4.0f * halfExtents[(axis + 1) % 3] * halfExtents[(axis + 2) % 3];
        EXPECT_NEAR(faceArea[axis][0], expectedArea, kEpsilon) << "axis " << axis << " min face";
        EXPECT_NEAR(faceArea[axis][1], expectedArea, kEpsilon) << "axis " << axis << " max face";

        for (int side = 0; side < 2; ++side)
        {
            const std::vector<uint32_t>& triangles = faceTriangleCorners[axis][side];
            ASSERT_EQ(triangles.size(), 2u) << "axis " << axis << " side " << side;
            const uint32_t shared = triangles[0] & triangles[1];
            ASSERT_EQ(std::popcount(shared), 2) << "axis " << axis << " side " << side
                                                << ": the triangles do not share exactly two corners";
            const uint32_t firstCorner = static_cast<uint32_t>(std::countr_zero(shared));
            const uint32_t secondCorner = static_cast<uint32_t>(std::bit_width(shared) - 1);
            EXPECT_EQ(std::popcount(firstCorner ^ secondCorner), 2)
                << "axis " << axis << " side " << side << ": the shared corners are a face edge, not the diagonal";
        }
    }

    ResetGizmoTriangleGroups(viewId);
}

// The wire box is the 12 distinct box edges: each joins two corners that differ
// on exactly one axis.
TEST(SceneViewGizmoHelpersTests, WireBoxDrawsTheTwelveBoxEdges)
{
    const ViewId viewId = 10;
    ResetGizmoLineGroups(viewId);
    GizmoRenderContext context(viewId, CameraId{1});

    const Vector3 center(1.0f, 2.0f, 3.0f);
    const Vector3 halfExtents(0.5f, 1.0f, 2.0f);
    context.DrawWireBox(center, halfExtents, Color(1.0f, 1.0f, 1.0f, 1.0f));

    const auto* groups = GetGizmoLineGroups(viewId);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const std::vector<float>& v = groups->front().vertices;
    ASSERT_EQ(v.size(), 12u * 6u);

    // An edge is named by its axis and the min/max side of the two other axes.
    std::array<bool, 12> seen{};
    for (size_t e = 0; e < v.size(); e += 6)
    {
        int differingAxis = -1;
        int differingCount = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::fabs(v[e + axis] - v[e + 3 + axis]) > kEpsilon)
            {
                differingAxis = axis;
                ++differingCount;
            }
        }
        ASSERT_EQ(differingCount, 1) << "edge " << e / 6 << " is not along one axis";
        EXPECT_NEAR(std::fabs(v[e + differingAxis] - v[e + 3 + differingAxis]), 2.0f * halfExtents[differingAxis],
                    kEpsilon) << "edge " << e / 6;

        const int axisA = (differingAxis + 1) % 3;
        const int axisB = (differingAxis + 2) % 3;
        const int sideA = v[e + axisA] > center[axisA] ? 1 : 0;
        const int sideB = v[e + axisB] > center[axisB] ? 1 : 0;
        const size_t key = static_cast<size_t>(differingAxis * 4 + sideA * 2 + sideB);
        EXPECT_FALSE(seen[key]) << "edge " << e / 6 << " is drawn twice";
        seen[key] = true;
    }

    ResetGizmoLineGroups(viewId);
}

TEST(SceneViewGizmoHelpersTests, IconPickReturnsTheNearestHitIcon)
{
    // Listed far-first, so the nearest hit has to beat an earlier one.
    const GizmoIconPickSphere icons[] = {
        {Vector3(0.0f, 0.0f, 10.0f), 0.5f},
        {Vector3(0.1f, 0.0f, 4.0f), 0.5f},
    };
    const auto picked = PickNearestGizmoIcon(RayAlongZ(), icons);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(*picked, 1u);
}

TEST(SceneViewGizmoHelpersTests, IconPickMissesIconsBesideTheRay)
{
    const GizmoIconPickSphere icons[] = {{Vector3(2.0f, 0.0f, 5.0f), 0.5f}};
    EXPECT_FALSE(PickNearestGizmoIcon(RayAlongZ(), icons).has_value());
}

TEST(SceneViewGizmoHelpersTests, IconPickIgnoresIconsBehindTheRayOrigin)
{
    const GizmoIconPickSphere icons[] = {
        {Vector3(0.0f, 0.0f, -3.0f), 0.5f},
        {Vector3(0.0f, 0.0f, 6.0f), 0.5f},
    };
    const auto picked = PickNearestGizmoIcon(RayAlongZ(), icons);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(*picked, 1u);
}

TEST(SceneViewGizmoHelpersTests, ThickLineQuadCoversTheSegmentAtItsHalfWidthFacingTheCamera)
{
    const Vector3 camera(0.0f, 0.0f, -10.0f);
    GizmoRenderContext context(ViewId{21}, CameraId{21}, &camera);

    std::array<Vector3, 6> quad;
    ASSERT_TRUE(BuildThickLineQuad(context, Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), 2.0f,
                                   /*constantScreenSpaceWidth=*/false, /*smartDistanceScaling=*/false, quad));

    // World-space width: thickness 2 is a 0.02 half-width, offset along the
    // camera-facing perpendicular of the segment (here -Y). Two triangles:
    // (a - o, a + o, b + o) and (a - o, b + o, b - o).
    const Vector3 expected[6] = {
        Vector3(-1.0f, 0.02f, 0.0f), Vector3(-1.0f, -0.02f, 0.0f), Vector3(1.0f, -0.02f, 0.0f),
        Vector3(-1.0f, 0.02f, 0.0f), Vector3(1.0f, -0.02f, 0.0f), Vector3(1.0f, 0.02f, 0.0f)};
    for (std::size_t i = 0; i < quad.size(); ++i)
    {
        EXPECT_NEAR(quad[i].x, expected[i].x, 1e-6f) << "vertex " << i;
        EXPECT_NEAR(quad[i].y, expected[i].y, 1e-6f) << "vertex " << i;
        EXPECT_NEAR(quad[i].z, expected[i].z, 1e-6f) << "vertex " << i;
    }
}

TEST(SceneViewGizmoHelpersTests, ThickLineQuadNeedsACameraAndASegment)
{
    const Vector3 a(-1.0f, 0.0f, 0.0f);
    const Vector3 b(1.0f, 0.0f, 0.0f);
    std::array<Vector3, 6> quad;

    GizmoRenderContext noCamera(ViewId{22}, CameraId{22});
    EXPECT_FALSE(BuildThickLineQuad(noCamera, a, b, 2.0f, false, false, quad));

    const Vector3 camera(0.0f, 0.0f, -10.0f);
    GizmoRenderContext context(ViewId{22}, CameraId{22}, &camera);
    EXPECT_FALSE(BuildThickLineQuad(context, a, a, 2.0f, false, false, quad));
}

TEST(SceneViewGizmoHelpersTests, PlaneBasisSeedsFromWorldUpUnlessTheNormalIsNearVertical)
{
    Vector3 u;
    Vector3 v;

    BuildPlaneBasis(Vector3(0.0f, 0.0f, -1.0f), u, v);
    EXPECT_FLOAT_EQ(u.x, -1.0f);
    EXPECT_FLOAT_EQ(u.y, 0.0f);
    EXPECT_FLOAT_EQ(u.z, 0.0f);
    EXPECT_FLOAT_EQ(v.x, 0.0f);
    EXPECT_FLOAT_EQ(v.y, 1.0f);
    EXPECT_FLOAT_EQ(v.z, 0.0f);

    // Looking straight down: world up is parallel to the normal, so world X seeds.
    BuildPlaneBasis(Vector3(0.0f, -1.0f, 0.0f), u, v);
    EXPECT_FLOAT_EQ(u.x, 0.0f);
    EXPECT_FLOAT_EQ(u.y, 0.0f);
    EXPECT_FLOAT_EQ(u.z, -1.0f);
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.y, 0.0f);
    EXPECT_FLOAT_EQ(v.z, 0.0f);
}
